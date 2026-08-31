#pragma once

#include <Arduino.h>
#include "Config.h"
#include "servos.h"

struct PersistData {
  uint16_t magic;
  ChannelCal cal[NUM_SERVOS];
  uint32_t fobCode[FOB_BUTTONS][FOB_REMOTES];
  uint8_t  fobMap[FOB_BUTTONS];
  uint8_t  peerMac[6];
  int16_t  poseClosed[NUM_SERVOS];
  int16_t  poseOpen[NUM_SERVOS];
  int16_t  poseHug[NUM_SERVOS];
  uint8_t  sense[NUM_SERVOS];
  uint8_t  reserved[8];  // [0]=speedPct  [1..2]=accelMs LE  [3..7]=chSpeed[5] (0=100)
};

struct PersistDataV171 {
  uint16_t magic;
  ChannelCal cal[NUM_SERVOS];
  uint32_t fobCode[FOB_BUTTONS][FOB_REMOTES];
  uint8_t  fobMap[FOB_BUTTONS];
  uint8_t  peerMac[6];
  uint8_t  reserved[8];
};

void storeBegin();
bool storeLoad();
bool storeSave();
void storeDefaults(PersistData& d);
PersistData& storeData();
void storeApplyToServos();
void storePullFromServos();
void storeSaveLastCmd();  // separate key — does not resize PersistData
bool storeLoadLastCmd();
void storeSaveLastHeard(const uint8_t mac[6]);  // separate key
bool storeLoadLastHeard(uint8_t out[6]);
