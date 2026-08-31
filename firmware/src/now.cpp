#include "now.h"
#include "role.h"
#include "store.h"
#include "servos.h"
#include "button.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

static const uint8_t NOW_MAGIC = 0xA1;
static uint8_t g_seq = 0;
static bool g_paused = false;
static bool g_ready = false;
static uint8_t g_peerMac[6] = {0};
static uint8_t g_heardMac[6] = {0};
static bool g_hasPeer = false;
static unsigned long g_lastSeenMs = 0;
static unsigned long g_lastHelloMs = 0;
static unsigned long g_lastPingMs = 0;
static uint32_t g_rxCount = 0;
static bool g_emuPending = false;
static CmdId g_emuCmd = CMD_NONE;
static int16_t g_emuPayload[NUM_SERVOS] = {};
static uint8_t g_emuLen = 0;
static uint8_t g_emuSeq = 0;

static bool macZero(const uint8_t m[6]) {
  for (int i = 0; i < 6; i++) {
    if (m[i] != 0) return false;
  }
  return true;
}

static bool macEq(const uint8_t a[6], const uint8_t b[6]) {
  return memcmp(a, b, 6) == 0;
}

static void printMac(const char* tag, const uint8_t m[6]) {
  Serial.printf("%s %02X:%02X:%02X:%02X:%02X:%02X\n", tag,
                m[0], m[1], m[2], m[3], m[4], m[5]);
}

static uint8_t xorCrc(const uint8_t* p, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) c ^= p[i];
  return c;
}

static void fillPacket(NowPacket& pkt, uint8_t cmd, const int16_t* payload, uint8_t len) {
  memset(&pkt, 0, sizeof(pkt));
  pkt.magic = NOW_MAGIC;
  pkt.version = PROTOCOL_VERSION;
  pkt.seq = ++g_seq;
  pkt.cmd = cmd;
  pkt.len = len;
  if (payload && len) {
    uint8_t n = min(len, (uint8_t)NUM_SERVOS);
    for (uint8_t i = 0; i < n; i++) pkt.payload[i] = payload[i];
  }
  pkt.crc = xorCrc((const uint8_t*)&pkt, sizeof(pkt) - 1);
}

static bool packetValid(const NowPacket& pkt) {
  if (pkt.magic != NOW_MAGIC) return false;
  if (pkt.version != PROTOCOL_VERSION) return false;
  uint8_t c = xorCrc((const uint8_t*)&pkt, sizeof(pkt) - 1);
  return c == pkt.crc;
}

static void ensurePeer(const uint8_t mac[6]) {
  if (!mac || macZero(mac) || esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = NOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    printMac("ESP-NOW add peer FAILED", mac);
  }
}

static void noteSeen(const uint8_t* mac) {
  if (!mac || macZero(mac)) return;
  if (g_hasPeer && !macEq(mac, g_peerMac)) return;
  const bool first = (g_lastSeenMs == 0) || !macEq(g_heardMac, mac);
  memcpy(g_heardMac, mac, 6);
  g_lastSeenMs = millis();
  if (first) {
    printMac("ESP-NOW heard", mac);
    storeSaveLastHeard(mac);
  }
}

static void sendAck(const uint8_t dest[6], uint8_t seq, uint8_t cmd) {
  if (!g_ready || !dest) return;
  NowPacket pkt;
  int16_t p[1] = { (int16_t)cmd };
  fillPacket(pkt, NOW_CMD_ACK, p, 1);
  pkt.seq = seq;
  pkt.crc = xorCrc((const uint8_t*)&pkt, sizeof(pkt) - 1);
  ensurePeer(dest);
  esp_now_send(dest, (uint8_t*)&pkt, sizeof(pkt));
}

static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (g_paused) return;
  if (len < (int)sizeof(NowPacket)) return;
  uint8_t me[6];
  WiFi.macAddress(me);
  if (mac && macEq(mac, me)) return;
  NowPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (!packetValid(pkt)) {
    Serial.println(F("ESP-NOW: bad packet"));
    return;
  }

  g_rxCount++;

  if (pkt.cmd == NOW_CMD_HELLO || pkt.cmd == NOW_CMD_ACK) {
    if (roleIsMaster()) noteSeen(mac);
    return;
  }

  if (pkt.cmd == NOW_CMD_PING) {
    if (!roleIsMaster() && mac) sendAck(mac, pkt.seq, pkt.cmd);
    return;
  }

  // Master never runs motion from ESP-NOW (own broadcast must not re-ARM).
  if (roleIsMaster()) return;

  if (pkt.cmd == CMD_SET_TARGETS || pkt.cmd == CMD_TEACH_POSE ||
      pkt.cmd == CMD_SET_SENSE) {
    return;
  }

  // Queue one button. Motion runs in nowService() on loop — same as BLE.
  if (g_emuPending && g_emuSeq == pkt.seq) return;
  g_emuCmd = (CmdId)pkt.cmd;
  g_emuLen = pkt.len;
  if (g_emuLen > NUM_SERVOS) g_emuLen = NUM_SERVOS;
  memcpy(g_emuPayload, pkt.payload, sizeof(g_emuPayload));
  g_emuSeq = pkt.seq;
  g_emuPending = true;
  Serial.printf("ESP-NOW button cmd=%u seq=%u\n", (unsigned)pkt.cmd, pkt.seq);
}

static bool isBroadcast(const uint8_t* mac) {
  if (!mac) return true;
  for (int i = 0; i < 6; i++) {
    if (mac[i] != 0xFF) return false;
  }
  return true;
}

static void onSent(const uint8_t* mac, esp_now_send_status_t status) {
  // Broadcast has no 802.11 ACK — IDF reports fail even when it went out.
  if (isBroadcast(mac)) return;
  if (status == ESP_NOW_SEND_SUCCESS) {
    if (roleIsMaster()) noteSeen(mac);
    return;
  }
  static unsigned long lastFailMs = 0;
  if ((millis() - lastFailMs) > 3000) {
    lastFailMs = millis();
    Serial.println(F("ESP-NOW: unicast send fail"));
  }
}

static void lockChannel() {
  uint8_t ch = 0;
  wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&ch, &sec);
  if (ch != NOW_CHANNEL) {
    esp_wifi_set_channel(NOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  }
}

static void restorePeers() {
  if (g_hasPeer) ensurePeer(g_peerMac);
  uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  ensurePeer(bcast);
}

void nowBegin() {
  // Radio off so NimBLE can init. nowRebind() starts WiFi after bleBegin().
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);

  if (esp_now_init() != ESP_OK) {
    Serial.println(F("ESP-NOW init FAILED"));
    return;
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);

  memcpy(g_peerMac, storeData().peerMac, 6);
  g_hasPeer = !macZero(g_peerMac);
  if (g_hasPeer) {
    memcpy(g_heardMac, g_peerMac, 6);
    ensurePeer(g_peerMac);
    printMac("ESP-NOW peer", g_peerMac);
  } else if (storeLoadLastHeard(g_heardMac) && !macZero(g_heardMac)) {
    printMac("ESP-NOW last heard", g_heardMac);
  } else {
    Serial.println(F("ESP-NOW: no peer MAC (LINK in app, or 'peer <mac>')"));
  }

  restorePeers();

  g_ready = true;
  Serial.printf("ESP-NOW ready  MAC=%s  role=%s\n",
                WiFi.macAddress().c_str(), roleName());
}

void nowRebind() {
  if (!g_ready) return;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(20);
  esp_wifi_start();
  // Must keep default modem sleep — WIFI_PS_NONE aborts when NimBLE is on.
  esp_wifi_set_channel(NOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_now_deinit();
  if (esp_now_init() != ESP_OK) {
    Serial.println(F("ESP-NOW rebind FAILED"));
    g_ready = false;
    return;
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
  restorePeers();
  g_ready = true;
  Serial.println(F("ESP-NOW rebound after BLE"));
}

uint32_t nowRxCount() { return g_rxCount; }

uint8_t nowWifiChannel() {
  uint8_t ch = 0;
  wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&ch, &sec);
  return ch;
}

void nowService() {
  if (!g_ready) return;
  if (g_emuPending) {
    g_emuPending = false;
    Serial.printf("ESP-NOW emulate cmd=%u\n", (unsigned)g_emuCmd);
    buttonExec(g_emuCmd, g_emuPayload, g_emuLen);
  }
  if (servosAttachMask() != 0) return;
  lockChannel();
  if (g_paused) return;
  unsigned long now = millis();
  const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  if (roleIsMaster()) {
    if (now - g_lastPingMs < NOW_HELLO_MS) return;
    g_lastPingMs = now;
    NowPacket pkt;
    fillPacket(pkt, NOW_CMD_PING, nullptr, 0);
    esp_now_send(bcast, (uint8_t*)&pkt, sizeof(pkt));
    if (g_hasPeer) {
      ensurePeer(g_peerMac);
      fillPacket(pkt, NOW_CMD_PING, nullptr, 0);
      esp_now_send(g_peerMac, (uint8_t*)&pkt, sizeof(pkt));
    }
    return;
  }
  if (now - g_lastHelloMs < NOW_HELLO_MS) return;
  g_lastHelloMs = now;
  NowPacket pkt;
  fillPacket(pkt, NOW_CMD_HELLO, nullptr, 0);
  esp_now_send(bcast, (uint8_t*)&pkt, sizeof(pkt));
}

void nowSetPeerMac(const uint8_t mac[6]) {
  if (!mac || macZero(mac)) {
    nowClearPeer();
    return;
  }
  const bool sameHeard = !macZero(g_heardMac) && macEq(mac, g_heardMac);
  memcpy(g_peerMac, mac, 6);
  memcpy(storeData().peerMac, mac, 6);
  g_hasPeer = true;
  memcpy(g_heardMac, mac, 6);
  storeSaveLastHeard(mac);
  if (!sameHeard) g_lastSeenMs = 0;

  if (esp_now_is_peer_exist(mac)) {
    esp_now_del_peer(mac);
  }
  ensurePeer(mac);
  printMac("ESP-NOW peer set", mac);
}

void nowClearPeer() {
  if (g_hasPeer && !macZero(g_peerMac)) {
    memcpy(g_heardMac, g_peerMac, 6);
    storeSaveLastHeard(g_heardMac);
    if (esp_now_is_peer_exist(g_peerMac)) {
      esp_now_del_peer(g_peerMac);
    }
  }
  memset(g_peerMac, 0, 6);
  memset(storeData().peerMac, 0, 6);
  g_hasPeer = false;
  Serial.println(F("ESP-NOW peer cleared — last heard kept for LINK"));
}

bool nowHasPeer() { return g_hasPeer; }
void nowGetPeerMac(uint8_t out[6]) { memcpy(out, g_peerMac, 6); }

void nowGetLocalMac(uint8_t out[6]) {
  WiFi.macAddress(out);
}

void nowGetStatusPeerMac(uint8_t out[6]) {
  if (g_hasPeer) memcpy(out, g_peerMac, 6);
  else memcpy(out, g_heardMac, 6);
}

uint8_t nowSlaveLink() {
  if (!roleIsMaster()) {
    return g_paused ? (uint8_t)NOW_LINK_PAUSED : (uint8_t)NOW_LINK_NONE;
  }
  const bool recent = (g_lastSeenMs != 0) &&
                      ((millis() - g_lastSeenMs) < NOW_ALIVE_MS);
  if (g_hasPeer) {
    return recent ? (uint8_t)NOW_LINK_LIVE : (uint8_t)NOW_LINK_STALE;
  }
  return recent ? (uint8_t)NOW_LINK_HEARD : (uint8_t)NOW_LINK_NONE;
}

uint8_t nowSlaveAgeCs() {
  if (g_lastSeenMs == 0) return 255;
  unsigned long age = (millis() - g_lastSeenMs) / 100;
  if (age > 255) return 255;
  return (uint8_t)age;
}

void nowPause(bool paused) { g_paused = paused; }
bool nowIsPaused() { return g_paused; }

void nowBroadcastCmd(CmdId cmd, const int16_t* payload, uint8_t len) {
  if (!g_ready || !roleIsMaster() || g_paused) return;
  if (cmd == CMD_NONE || cmd == CMD_SET_TARGETS || cmd == CMD_TEACH_POSE ||
      cmd == CMD_SET_SENSE) {
    return;
  }

  NowPacket pkt;
  fillPacket(pkt, (uint8_t)cmd, payload, len);

  const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_err_t e = esp_now_send(bcast, (uint8_t*)&pkt, sizeof(pkt));
  if (e != ESP_OK) {
    Serial.println(F("ESP-NOW: send error — local button already ran"));
  }
}

void nowSyncTargets() {
  // Kept so old callers link. Do not push Master's µs to Slave.
}
