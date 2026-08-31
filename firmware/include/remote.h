#pragma once

#include <Arduino.h>
#include "Config.h"
#include "servos.h"

void remoteBegin();       // Master only; no-op on Slave
void remoteService();
void remoteStartLearn(uint8_t button, uint8_t remoteSlot = 0);  // slot 0=primary, 1=backup
void remoteCancelLearn();
bool remoteIsLearning();
uint8_t remoteLearnButton();
uint8_t remoteLearnSlot();
void remoteClearCode(uint8_t button, uint8_t remoteSlot = 0);
void remoteClearAllCodes(uint8_t button);
uint32_t remoteGetCode(uint8_t button, uint8_t remoteSlot = 0);
void remoteSetMap(uint8_t button, CmdId cmd);
CmdId remoteGetMap(uint8_t button);
