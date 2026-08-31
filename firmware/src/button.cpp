#include "button.h"
#include "role.h"
#include "now.h"
#include "store.h"

static bool isLocalOnly(CmdId cmd) {
  return cmd == CMD_NONE || cmd == CMD_SET_TARGETS ||
         cmd == CMD_TEACH_POSE || cmd == CMD_SET_SENSE;
}

void buttonExec(CmdId cmd, const int16_t* payload, uint8_t len) {
  servosHandleCmd(cmd, payload, len);
  if (cmd == CMD_SET_SPEED || cmd == CMD_SET_CH_SPEED)
    storeSave();
}

void buttonDispatch(ButtonSrc src, CmdId cmd, const int16_t* payload, uint8_t len) {
  if (isLocalOnly(cmd)) {
    buttonExec(cmd, payload, len);
    return;
  }
  // Sync = Master + saved peer. Isolated Master/Slave BLE skips this.
  CmdId exec = cmd;
  if (src != BTN_NOW && roleIsMaster() && nowHasPeer()) {
    exec = servosResolvePairCmd(cmd);
    nowBroadcastCmd(exec, payload, len);
  }
  buttonExec(exec, payload, len);
}
