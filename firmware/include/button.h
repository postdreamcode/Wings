#pragma once

#include "servos.h"

enum ButtonSrc : uint8_t {
  BTN_BLE = 0,
  BTN_FOB,
  BTN_SERIAL,
  BTN_NOW
};

// Any task, including NimBLE and ESP-NOW recv. Copies the request and
// returns. Does not run motion, resolve pair cmds, or touch a pad.
void buttonDispatch(ButtonSrc src, CmdId cmd, const int16_t* payload, uint8_t len);

// Loop task only. Drains the queue, broadcasts on Master+sync, then runs
// the command. Call once per loop, before servosService.
void buttonService();
