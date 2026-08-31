#pragma once

#include "servos.h"

enum ButtonSrc : uint8_t {
  BTN_BLE = 0,
  BTN_FOB,
  BTN_SERIAL,
  BTN_NOW
};

// Motion only — same function every comm path ends on.
void buttonExec(CmdId cmd, const int16_t* payload, uint8_t len);

// BLE / fob / serial: if Master has a saved peer (sync), send the button
// over ESP-NOW first, then exec locally. Slave BLE is exec only.
// ESP-NOW recv must call buttonExec, not this (no re-send).
void buttonDispatch(ButtonSrc src, CmdId cmd, const int16_t* payload, uint8_t len);
