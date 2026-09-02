#include "button.h"
#include "role.h"
#include "now.h"
#include "store.h"
#include <Arduino.h>

/*
  Every transport ends here. The queue holds a CmdId for motion; setup/config
  that still needs a channel or a delta uses a separate slot. Nothing in this
  file runs on the caller's task except the copy into the slot.
*/

static portMUX_TYPE g_qMux = portMUX_INITIALIZER_UNLOCKED;

static CmdId g_stopCmd = CMD_NONE;       // CMD_STOP or CMD_NONE
static CmdId g_disarmAll = CMD_NONE;     // CMD_DISARM or CMD_NONE
static int8_t g_disarmCh = -1;           // 0..4, or -1
static CmdId g_motionCmd = CMD_NONE;     // enum only
static CmdId g_setupCmd = CMD_NONE;
static int16_t g_setupPayload[NUM_SERVOS];
static uint8_t g_setupLen = 0;

static bool isLocalOnly(CmdId cmd) {
  return cmd == CMD_NONE || cmd == CMD_SET_TARGETS ||
         cmd == CMD_TEACH_POSE || cmd == CMD_SET_SENSE ||
         cmd == CMD_ARM_ALL;
}

static bool isMotionTrigger(CmdId cmd) {
  return cmd == CMD_HOME || cmd == CMD_TOGGLE_WING || cmd == CMD_TOGGLE_WRIST ||
         cmd == CMD_SEQ || cmd == CMD_POSE_FOLDED || cmd == CMD_POSE_OPEN ||
         cmd == CMD_POSE_HUG || cmd == CMD_STOP || cmd == CMD_ARM_ALL;
}

static bool isSetupMotion(CmdId cmd) {
  return cmd == CMD_ARM || cmd == CMD_JOG;
}

static void execOne(CmdId cmd, const int16_t* payload, uint8_t len) {
  CmdId run = cmd;
  // COLD D is ARM ALL. Master already unicasts ARM+ch per step. Broadcasting
  // HOME would start a second sequence on the Slave.
  const bool skipNow = (cmd == CMD_HOME && !servosIsBrakeReady());
  if (!skipNow && !isLocalOnly(cmd) && roleIsMaster() && nowHasPeer()) {
    run = servosResolvePairCmd(cmd);
    nowBroadcastCmd(run, payload, len);
  }
  const uint8_t speedBefore =
      (cmd == CMD_SET_SPEED) ? servosGetSpeedPct() : 0;
  servosHandleCmd(run, payload, len);
  if (cmd == CMD_SET_SPEED) {
    if (servosGetSpeedPct() != speedBefore) storeSave();
  } else if (cmd == CMD_SET_CH_SPEED) {
    storeSave();
  } else if (cmd == CMD_TEACH_POSE || cmd == CMD_SET_SENSE) {
    storeSave();
  }
}

void buttonDispatch(ButtonSrc src, CmdId cmd, const int16_t* payload, uint8_t len) {
  (void)src;
  if (cmd == CMD_NONE || cmd == CMD_SET_ACCEL) return;

  const bool disarmAll = (cmd == CMD_DISARM) &&
                         !(payload && len >= 1 && payload[0] >= 0 &&
                           payload[0] < NUM_SERVOS);
  const bool disarmCh = (cmd == CMD_DISARM) && !disarmAll;
  // HOME is the recovery after STOP. Dropping it while g_stopping is set
  // leaves the operator with no way off a freeze.
  const bool alwaysPass =
      (cmd == CMD_STOP) || (cmd == CMD_DISARM) || (cmd == CMD_HOME);

  if (!alwaysPass && (isMotionTrigger(cmd) || isSetupMotion(cmd)) &&
      servosMotionBusy()) {
    Serial.printf("BUSY — dropped cmd=%u\n", (unsigned)cmd);
    return;
  }

  portENTER_CRITICAL(&g_qMux);
  if (cmd == CMD_STOP) {
    g_stopCmd = CMD_STOP;
    g_motionCmd = CMD_NONE;
  } else if (disarmAll) {
    g_disarmAll = CMD_DISARM;
    g_disarmCh = -1;
  } else if (disarmCh) {
    g_disarmCh = (int8_t)payload[0];
  } else if (isMotionTrigger(cmd)) {
    g_motionCmd = cmd;
  } else {
    g_setupCmd = cmd;
    g_setupLen = 0;
    if (payload && len) {
      uint8_t n = len;
      if (n > NUM_SERVOS) n = NUM_SERVOS;
      for (uint8_t i = 0; i < n; i++) g_setupPayload[i] = payload[i];
      g_setupLen = n;
    }
  }
  portEXIT_CRITICAL(&g_qMux);
}

void buttonService() {
  CmdId stop = CMD_NONE;
  CmdId disarmAll = CMD_NONE;
  int8_t disarmCh = -1;
  CmdId motion = CMD_NONE;
  CmdId setup = CMD_NONE;
  int16_t setupPayload[NUM_SERVOS];
  uint8_t setupLen = 0;

  portENTER_CRITICAL(&g_qMux);
  stop = g_stopCmd;
  g_stopCmd = CMD_NONE;
  disarmAll = g_disarmAll;
  g_disarmAll = CMD_NONE;
  disarmCh = g_disarmCh;
  g_disarmCh = -1;
  motion = g_motionCmd;
  g_motionCmd = CMD_NONE;
  setup = g_setupCmd;
  g_setupCmd = CMD_NONE;
  setupLen = g_setupLen;
  g_setupLen = 0;
  for (uint8_t i = 0; i < setupLen && i < NUM_SERVOS; i++)
    setupPayload[i] = g_setupPayload[i];
  portEXIT_CRITICAL(&g_qMux);

  // STOP, then DISARM, always. HOME is recovery and runs even during STOP
  // settle. Everything else is dropped if a halt is in flight.
  if (stop == CMD_STOP) {
    execOne(CMD_STOP, nullptr, 0);
    return;
  }
  if (disarmAll == CMD_DISARM) {
    execOne(CMD_DISARM, nullptr, 0);
    return;
  }
  if (disarmCh >= 0 && disarmCh < NUM_SERVOS) {
    int16_t p = disarmCh;
    execOne(CMD_DISARM, &p, 1);
    return;
  }

  if (motion == CMD_HOME) {
    execOne(CMD_HOME, nullptr, 0);
    return;
  }

  if (servosMotionBusy()) {
    if (motion != CMD_NONE)
      Serial.printf("BUSY — dropped cmd=%u\n", (unsigned)motion);
    if (setup != CMD_NONE && isSetupMotion(setup))
      Serial.printf("BUSY — dropped cmd=%u\n", (unsigned)setup);
    else if (setup != CMD_NONE)
      execOne(setup, setupLen ? setupPayload : nullptr, setupLen);
    return;
  }

  // Config (speed) must not be dropped because a pose was also pending.
  if (setup != CMD_NONE)
    execOne(setup, setupLen ? setupPayload : nullptr, setupLen);
  if (motion != CMD_NONE)
    execOne(motion, nullptr, 0);
}
