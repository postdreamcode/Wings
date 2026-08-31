#pragma once

#include <Arduino.h>
#include "Config.h"
#include "servos.h"

#pragma pack(push, 1)
struct NowPacket {
  uint8_t  magic;      // 0xA1
  uint8_t  version;    // PROTOCOL_VERSION
  uint8_t  seq;
  uint8_t  cmd;        // CmdId
  int16_t  payload[NUM_SERVOS];
  uint8_t  len;        // meaningful payload ints
  uint8_t  crc;        // simple XOR of prior bytes
};
#pragma pack(pop)

enum NowLink : uint8_t {
  NOW_LINK_NONE   = 0,  // Master: nothing heard. Slave: idle
  NOW_LINK_HEARD  = 1,  // Master: Slave hello/ack, peer not saved
  NOW_LINK_LIVE   = 2,  // Master: saved peer, seen < NOW_ALIVE_MS
  NOW_LINK_STALE  = 3,  // Master: saved peer, silent
  NOW_LINK_PAUSED = 4   // Slave: BLE cal — ESP-NOW RX paused
};

void nowBegin();
void nowRebind();  // after NimBLE — BLE init drops ESP-NOW RX
void nowService();
uint32_t nowRxCount();
uint8_t nowWifiChannel();
void nowBroadcastCmd(CmdId cmd, const int16_t* payload = nullptr, uint8_t len = 0);
// Send only. No motion. Master + sync uses this before buttonExec.
void nowSyncTargets();  // no-op
void nowSetPeerMac(const uint8_t mac[6]);
void nowClearPeer();
bool nowHasPeer();
void nowGetPeerMac(uint8_t out[6]);
void nowGetLocalMac(uint8_t out[6]);
void nowGetStatusPeerMac(uint8_t out[6]);  // saved peer, else last heard
uint8_t nowSlaveLink();
uint8_t nowSlaveAgeCs();  // 100 ms units, 255 = never
void nowPause(bool paused);  // while Slave is in direct BLE cal
bool nowIsPaused();
