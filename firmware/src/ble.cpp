#include "ble.h"
#include "Config.h"
#include "role.h"
#include "servos.h"
#include "store.h"
#include "remote.h"
#include "now.h"
#include "button.h"
#include <NimBLEDevice.h>

// GATT layout (custom 128-bit UUIDs)
// Service:        a1700001-0000-1000-8000-00805f9b34fb
// Status (notify):a1700002-...  binary status blob
// Command (write):a1700003-...  [cmd][payload...]
// Cal (r/w):      a1700004-...  ChannelCal[5] packed
// Fob (r/w):      a1700005-...  r1[4]+r2[4]+map[4] (36)
// Poses (r/w):    a1700006-...  closed/open/hug int16[5] + sense[5]

static NimBLEServer* g_server = nullptr;
static NimBLECharacteristic* g_statusChar = nullptr;
static NimBLECharacteristic* g_cmdChar = nullptr;
static NimBLECharacteristic* g_calChar = nullptr;
static NimBLECharacteristic* g_fobChar = nullptr;
static NimBLECharacteristic* g_poseChar = nullptr;
static bool g_connected = false;
static unsigned long g_lastNotifyMs = 0;

#pragma pack(push, 1)
struct StatusBlob {
  uint8_t  proto;
  uint8_t  role;
  uint8_t  armed;
  uint8_t  wing;
  uint8_t  wrist;
  uint8_t  seq;
  int16_t  target[NUM_SERVOS];
  int16_t  actual[NUM_SERVOS];
  uint8_t  speedPct;
  uint8_t  attachMask;  // bit i = channel i pulsing
  uint16_t accelMs;     // appended so proto 2 readers stay valid
  uint8_t  brakeReady;  // 1 after first attach→home→detach on all 5 this boot
  uint8_t  chSpeed[NUM_SERVOS];
  uint8_t  slaveLink;   // NowLink
  uint8_t  slaveAgeCs;  // 100 ms since last hello/ack, 255=never
  uint8_t  localMac[6]; // WiFi STA / ESP-NOW MAC
  uint8_t  peerMac[6];  // saved peer, else last heard
};
#pragma pack(pop)

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s) override {
    (void)s;
    g_connected = true;
    Serial.println(F("BLE connected"));
    // Pause ESP-NOW on Slave during direct cal to avoid fighting
    if (!roleIsMaster()) nowPause(true);
  }
  void onDisconnect(NimBLEServer* s) override {
    (void)s;
    g_connected = false;
    Serial.println(F("BLE disconnected"));
    if (!roleIsMaster()) nowPause(false);
    NimBLEDevice::startAdvertising();
  }
};

class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.size() < 1) return;
    CmdId cmd = (CmdId)(uint8_t)v[0];
    int16_t payload[NUM_SERVOS] = {};
    uint8_t plen = 0;
    // Parse by command id first. A padded write (>=11 bytes) used to
    // look like SET_TARGETS and slam every channel.
    if (cmd == CMD_JOG && v.size() >= 4) {
      payload[0] = (uint8_t)v[1];
      payload[1] = (int16_t)((uint8_t)v[2] | ((uint8_t)v[3] << 8));
      plen = 2;
      Serial.printf("BLE JOG ch=%u d=%d pin=%u\n",
                    (unsigned)payload[0], (int)payload[1],
                    (payload[0] < NUM_SERVOS) ? SERVO_PINS[(uint8_t)payload[0]] : 99);
    } else if ((cmd == CMD_ARM || cmd == CMD_DISARM) && v.size() == 2 &&
               (uint8_t)v[1] < NUM_SERVOS) {
      // Exactly cmd+ch. No-channel ARM is ignored (not a 5-servo seq).
      payload[0] = (uint8_t)v[1];
      plen = 1;
    } else if (cmd == CMD_SET_SPEED && v.size() >= 2) {
      payload[0] = (uint8_t)v[1];
      plen = 1;
    } else if (cmd == CMD_SET_CH_SPEED && v.size() >= 3) {
      payload[0] = (uint8_t)v[1];
      payload[1] = (uint8_t)v[2];
      plen = 2;
    } else if ((cmd == CMD_TEACH_POSE || cmd == CMD_SET_SENSE) && v.size() >= 3) {
      payload[0] = (uint8_t)v[1];
      payload[1] = (uint8_t)v[2];
      plen = 2;
    } else if (cmd == CMD_SET_TARGETS) {
      Serial.println(F("BLE SET_TARGETS ignored"));
      return;
    }
    Serial.printf("BLE cmd=%u len=%u\n", (unsigned)cmd, (unsigned)v.size());
    buttonDispatch(BTN_BLE, cmd, plen ? payload : nullptr, plen);
  }
};

class CalCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.size() < sizeof(ChannelCal) * NUM_SERVOS) return;
    const ChannelCal* cal = (const ChannelCal*)v.data();
    for (uint8_t i = 0; i < NUM_SERVOS; i++) {
      ChannelCal& dst = servosCal(i);
      dst = cal[i];
      dst.pad = 0;
      servosClampSoftInsideHard(i);
    }
    storeSave();
    Serial.println(F("BLE cal written + NVS saved"));
  }
  void onRead(NimBLECharacteristic* c) override {
    ChannelCal buf[NUM_SERVOS];
    for (uint8_t i = 0; i < NUM_SERVOS; i++) buf[i] = servosCal(i);
    c->setValue((uint8_t*)buf, sizeof(buf));
  }
};

class FobCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    // layout: 4xuint32 primary + 4xuint32 backup + 4xuint8 map = 36 bytes
    // legacy 20-byte writes still accepted (primary + map only)
    PersistData& d = storeData();
    if (v.size() >= 36) {
      for (uint8_t i = 0; i < FOB_BUTTONS; i++) {
        memcpy(&d.fobCode[i][0], v.data() + i * 4, 4);
        memcpy(&d.fobCode[i][1], v.data() + 16 + i * 4, 4);
      }
      memcpy(d.fobMap, v.data() + 32, 4);
    } else if (v.size() >= 20) {
      for (uint8_t i = 0; i < FOB_BUTTONS; i++) {
        memcpy(&d.fobCode[i][0], v.data() + i * 4, 4);
      }
      memcpy(d.fobMap, v.data() + 16, 4);
    } else {
      return;
    }
    storeSave();
    Serial.println(F("BLE fob map written + NVS saved"));
  }
  void onRead(NimBLECharacteristic* c) override {
    uint8_t buf[36];
    PersistData& d = storeData();
    for (uint8_t i = 0; i < FOB_BUTTONS; i++) {
      memcpy(buf + i * 4, &d.fobCode[i][0], 4);
      memcpy(buf + 16 + i * 4, &d.fobCode[i][1], 4);
    }
    memcpy(buf + 32, d.fobMap, 4);
    c->setValue(buf, sizeof(buf));
  }
};

#pragma pack(push, 1)
struct PoseBlob {
  int16_t closed[NUM_SERVOS];
  int16_t open[NUM_SERVOS];
  int16_t hug[NUM_SERVOS];
  uint8_t sense[NUM_SERVOS];
};
#pragma pack(pop)

static void fillPoseBlob(PoseBlob& p) {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    p.closed[i] = servosGetPoseUs(i, POSE_CLOSED);
    p.open[i] = servosGetPoseUs(i, POSE_OPEN);
    p.hug[i] = servosGetPoseUs(i, POSE_HUG);
    p.sense[i] = servosGetSense(i);
  }
}

class PeerCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    if (!roleIsMaster()) return;
    std::string v = c->getValue();
    if (v.size() < 6) return;
    uint8_t mac[6];
    memcpy(mac, v.data(), 6);
    nowSetPeerMac(mac);
    storeSave();
    Serial.println(F("BLE peer written + NVS saved"));
  }
  void onRead(NimBLECharacteristic* c) override {
    uint8_t mac[6];
    nowGetStatusPeerMac(mac);
    c->setValue(mac, 6);
  }
};

class PoseCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (v.size() < sizeof(PoseBlob)) return;
    const PoseBlob* p = (const PoseBlob*)v.data();
    for (uint8_t i = 0; i < NUM_SERVOS; i++) {
      servosSetPoseUs(i, POSE_CLOSED, p->closed[i]);
      servosSetPoseUs(i, POSE_OPEN, p->open[i]);
      servosSetPoseUs(i, POSE_HUG, p->hug[i]);
      servosSetSense(i, p->sense[i] != 0);
    }
    storeSave();
    Serial.println(F("BLE poses written + NVS saved"));
  }
  void onRead(NimBLECharacteristic* c) override {
    PoseBlob p;
    fillPoseBlob(p);
    c->setValue((uint8_t*)&p, sizeof(p));
  }
};

static void fillStatus(StatusBlob& s) {
  s.proto = PROTOCOL_VERSION;
  s.role = (uint8_t)roleGet();
  s.armed = servosIsArmed() ? 1 : 0;
  s.wing = (uint8_t)getWingPose();
  s.wrist = pathIsActive() ? 1 : 0;
  s.seq = pathIsActive() ? 1 : 0;
  s.speedPct = servosGetSpeedPct();
  s.attachMask = servosAttachMask();
  s.accelMs = servosGetAccelMs();
  s.brakeReady = servosIsBrakeReady() ? 1 : 0;
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    s.chSpeed[i] = servosGetChSpeedPct(i);
  }
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    s.target[i] = (int16_t)servosGetTarget(i);
    s.actual[i] = (int16_t)servosGetActual(i);
  }
  s.slaveLink = nowSlaveLink();
  s.slaveAgeCs = nowSlaveAgeCs();
  nowGetLocalMac(s.localMac);
  nowGetStatusPeerMac(s.peerMac);
}

void bleBegin() {
  char name[32];
  snprintf(name, sizeof(name), "%s-%c-p%d", BLE_DEVICE_PREFIX,
           roleIsMaster() ? 'M' : 'S', PROTOCOL_VERSION);

  NimBLEDevice::init(name);
  NimBLEDevice::setMTU(185);  // status blob is 50 bytes; default ATT MTU 23 drops notify
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = g_server->createService(
      "a1700001-0000-1000-8000-00805f9b34fb");

  g_statusChar = svc->createCharacteristic(
      "a1700002-0000-1000-8000-00805f9b34fb",
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  g_cmdChar = svc->createCharacteristic(
      "a1700003-0000-1000-8000-00805f9b34fb",
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  g_cmdChar->setCallbacks(new CmdCallbacks());

  g_calChar = svc->createCharacteristic(
      "a1700004-0000-1000-8000-00805f9b34fb",
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  g_calChar->setCallbacks(new CalCallbacks());

  g_fobChar = svc->createCharacteristic(
      "a1700005-0000-1000-8000-00805f9b34fb",
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  g_fobChar->setCallbacks(new FobCallbacks());

  StatusBlob st;
  fillStatus(st);
  g_statusChar->setValue((uint8_t*)&st, sizeof(st));

  ChannelCal calBuf[NUM_SERVOS];
  for (uint8_t i = 0; i < NUM_SERVOS; i++) calBuf[i] = servosCal(i);
  g_calChar->setValue((uint8_t*)calBuf, sizeof(calBuf));

  uint8_t fobBuf[36];
  for (uint8_t i = 0; i < FOB_BUTTONS; i++) {
    memcpy(fobBuf + i * 4, &storeData().fobCode[i][0], 4);
    memcpy(fobBuf + 16 + i * 4, &storeData().fobCode[i][1], 4);
  }
  memcpy(fobBuf + 32, storeData().fobMap, 4);
  g_fobChar->setValue(fobBuf, sizeof(fobBuf));

  g_poseChar = svc->createCharacteristic(
      "a1700006-0000-1000-8000-00805f9b34fb",
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  g_poseChar->setCallbacks(new PoseCallbacks());
  PoseBlob pb;
  fillPoseBlob(pb);
  g_poseChar->setValue((uint8_t*)&pb, sizeof(pb));

  NimBLECharacteristic* peerChar = svc->createCharacteristic(
      "a1700007-0000-1000-8000-00805f9b34fb",
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  peerChar->setCallbacks(new PeerCallbacks());
  uint8_t peerBuf[6] = {0};
  nowGetStatusPeerMac(peerBuf);
  peerChar->setValue(peerBuf, 6);

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(svc->getUUID());
  adv->setName(name);
  adv->start();

  Serial.printf("BLE advertising as %s  proto=%d\n", name, PROTOCOL_VERSION);
}

bool bleIsConnected() { return g_connected; }

void bleNotifyStatus() {
  if (!g_statusChar) return;
  StatusBlob st;
  fillStatus(st);
  g_statusChar->setValue((uint8_t*)&st, sizeof(st));
  if (g_connected) g_statusChar->notify();
}

void bleService() {
  unsigned long now = millis();
  if (g_connected && (now - g_lastNotifyMs) >= 200) {
    g_lastNotifyMs = now;
    bleNotifyStatus();
  }
}
