#pragma once

#include <Arduino.h>
#include "Config.h"

// Last-commanded show pose (not measured shaft angle).
enum WingPose : uint8_t { POSE_CLOSED = 0, POSE_OPEN = 1, POSE_HUG = 2 };

enum SpeedTier : uint8_t { SPD_FULL = 0, SPD_HALF = 1, SPD_QUARTER = 2 };

enum PathId : uint8_t {
  PATH_NONE = 0,
  PATH_OPEN,
  PATH_CLOSE,
  PATH_HUG,
  PATH_OPEN_THEN_HUG,
  PATH_UNHUG_THEN_CLOSE,
  PATH_UNHUG_THEN_OPEN
};

#pragma pack(push, 1)
struct ChannelCal {
  int16_t softMin;
  int16_t softMax;
  int16_t hardMin;
  int16_t hardMax;
  int16_t center;
  // Reserved, always 0. Was an ESP-NOW left-wing mirror about center, made
  // unnecessary by sending poses as a signed span from each wing's own taught
  // CLOSED (see spanFromClosed). Kept so the NVS layout is unchanged; do not
  // reuse without a calibration migration. Horn direction is g_sense, not this.
  uint8_t invert;
  uint8_t pad;
};
#pragma pack(pop)

void servosBegin();
void servosApplyDefaults();
bool servosIsArmed();       // LIVE: PWM attached
bool servosIsBrakeReady();  // this boot: all 5 have attach→home→detach
void servosArm();  // serial bench only: seq SH ER EH WR WH
void servosArmCh(uint8_t ch);  // SETUP: one channel to taught CLOSED, then brake
void servosDisarm();
void servosDisarmCh(uint8_t ch);
uint8_t servosAttachMask();
void servosAlignActualToTargets();
void servosApplyLastCmd(const int16_t us[NUM_SERVOS]);
void servosHome(bool forceArm);  // D: unhug then close, quarter speed
void servosService();

// Motion-path diagnostic ring. Records pad state and pulse timing at every
// attach, detach, ramp anomaly and loop stall, without printing.
void servosTraceDump();
void servosTraceClear();

void servosSetTarget(uint8_t ch, int us);
void servosSetTargets(const int16_t us[NUM_SERVOS]);
void servosJog(uint8_t ch, int deltaUs);  // eased; + = more open/hug after sense
int  servosGetTarget(uint8_t ch);
int  servosGetActual(uint8_t ch);
bool servosAllArrived();

ChannelCal& servosCal(uint8_t ch);
void servosClampSoftInsideHard(uint8_t ch);
void servosSetSoftMin(uint8_t ch, int us);
void servosSetSoftMax(uint8_t ch, int us);
void servosSetHardMin(uint8_t ch, int us);
void servosSetHardMax(uint8_t ch, int us);
void servosSetCenter(uint8_t ch, int us);

uint8_t servosGetSense(uint8_t ch);
void servosSetSense(uint8_t ch, bool flip);
int16_t servosGetPoseUs(uint8_t ch, WingPose pose);
void servosTeachPose(uint8_t ch, WingPose pose);  // stamp current target
void servosSetPoseUs(uint8_t ch, WingPose pose, int us);
void servosDefaultPosesFromCal();

void applyPoseOpen();
void applyPoseClosed();
void applyPoseHug();

void cmdToggleWing();   // A
void cmdHug();          // B
void startSequence();   // C flap: hug ±10° about center, wrist lags
void abortPath();
void servosStop();  // abort + snap target=actual + detach (brake)
bool pathIsActive();
PathId pathGet();
WingPose getWingPose();

enum CmdId : uint8_t {
  CMD_NONE = 0,
  CMD_ARM,
  CMD_DISARM,
  CMD_HOME,
  CMD_TOGGLE_WING,
  CMD_TOGGLE_WRIST,  // B hug (id kept so learned fob map stays valid)
  CMD_SEQ,
  CMD_SET_TARGETS,
  CMD_JOG,
  CMD_POSE_FOLDED,
  CMD_POSE_OPEN,
  CMD_TEACH_POSE,    // payload: ch, pose (0/1/2)
  CMD_SET_SENSE,     // payload: ch, 0|1
  CMD_SET_SPEED,     // payload: percent 1-100
  CMD_SET_ACCEL,     // RETIRED, no longer accepted; id held so later ids keep value
  CMD_STOP,          // abort path, snap target=actual, detach (brake)
  CMD_POSE_HUG,
  CMD_SET_CH_SPEED   // payload: ch, percent 1-100
};

uint8_t servosGetSpeedPct();           // saved global slider (NVS)
uint8_t servosGetEffectiveSpeedPct();  // 10% while COLD
uint8_t servosGetChSpeedPct(uint8_t ch);
void servosSetSpeedPct(uint8_t pct);
void servosSetChSpeedPct(uint8_t ch, uint8_t pct);
// Inert: nothing reads the value. Retained so the NVS reserved bytes and the
// BLE status field keep round-tripping. Acceleration comes from the cosine ease.
uint16_t servosGetAccelMs();           // saved (NVS reserved[1..2])
void servosSetAccelMs(uint16_t ms);

void servosHandleCmd(CmdId cmd, const int16_t* payload, uint8_t payloadLen);
// Master+sync: A/B toggle → absolute pose so Slave cannot invert the move.
CmdId servosResolvePairCmd(CmdId cmd);
void servosAuditEnvelope();  // serial: pose vs soft, room beyond taught
