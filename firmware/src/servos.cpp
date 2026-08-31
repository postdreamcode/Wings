#include "servos.h"
#include "Config.h"
#include "now.h"
#include "store.h"
#include "servo_driver.h"
#include "driver/gpio.h"
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265f
#endif

/*
  This file owns poses, calibration and sequencing. It does NOT touch a servo pad
  or an LEDC register — servo_driver does, and refuses anything it cannot do
  safely. The LEDC channel map, duty conversion, pad handover and the per-frame
  step ceiling all moved there.

  Two consequences worth keeping in mind when reading the rest of this file:

  - g_actual[ch] is what the ramp has ASKED for. What the servo has actually been
    given is sdWritten(ch), which lags while the driver slews. Arrival is judged
    on the driver's value, never on g_actual.
  - The attach mask is not mirrored here. sdAttached / sdAttachMask are the single
    source of truth, so this file cannot drift out of step with the hardware.
*/

static bool g_armed = false;
static bool g_haveCommanded = false;
static uint8_t g_cycledMask = 0; // bit i: attach→closed→detach this boot
static bool g_brakeReady = false;
static bool g_armSeqOn = false;
static uint8_t g_armSeqI = 0;
static const uint8_t ARM_SEQ[NUM_SERVOS] = {
  CH_SHOULDER, CH_ELBOW1, CH_ELBOW2, CH_WRIST1, CH_WRIST2
};
static bool g_flapOn = false;
static bool g_flapPending = false;
static uint8_t g_flapCycle = 0;
static uint8_t g_flapStep = 0;  // 0 fwd 1 back (through center)
static bool g_flapPreamble = false;  // first move to center
static bool g_flapHome = false;      // last move to center
static unsigned long g_flapWristAt = 0;
static bool g_flapWristGo = false;
static unsigned long g_flapHoldUntil = 0;
static bool g_runMotion = false;
static bool g_forceDwell = false;  // D/home: hold closed pulse for estimated travel
static int8_t g_group = -1;      // stage index
static uint8_t g_groupAttachIdx = 0;
static unsigned long g_lastAttachMs = 0;
static unsigned long g_groupHoldUntil = 0;

// Stages: hug pair, or all three raises together (WR + ER + SH).
#define MAX_STAGES 6
#define MAX_STAGE_CH 3
static uint8_t g_stageCh[MAX_STAGES][MAX_STAGE_CH];
static uint8_t g_stageN[MAX_STAGES];
static uint8_t g_nStages = 0;

static int g_target[NUM_SERVOS];
static int g_actual[NUM_SERVOS];
static ChannelCal g_cal[NUM_SERVOS];
static int16_t g_poseClosed[NUM_SERVOS];
static int16_t g_poseOpen[NUM_SERVOS];
static int16_t g_poseHug[NUM_SERVOS];
static uint8_t g_sense[NUM_SERVOS];  // 1 = flip +jog µs direction

static WingPose g_pose = POSE_CLOSED;
static PathId g_path = PATH_NONE;
static uint8_t g_step = 0;  // 0 apply first, 1 second stage
static SpeedTier g_speed = SPD_FULL;
static SpeedTier g_speed2 = SPD_FULL;  // second stage
static uint8_t g_speedScalePct = SPEED_SCALE_PCT;
static uint8_t g_chSpeedPct[NUM_SERVOS] = {100, 100, 100, 100, 100};
// Inert. Was the accel=decel time of a trapezoid velocity profile, which the
// raised cosine replaced: acceleration is now a consequence of distance and
// cruise speed, not a separate setting. Kept only so the NVS reserved bytes and
// the BLE status packet round-trip unchanged.
static uint32_t g_accelMs = RAMP_ACCEL_MS;  // NVS reserved[1..2]
static bool g_poseKnown = false;  // follows BRAKE READY
static unsigned long g_setupIdleAt[NUM_SERVOS] = {0, 0, 0, 0, 0};
static uint8_t g_jogMask = 0;
static int g_jogFrom[NUM_SERVOS];
static unsigned long g_jogT0[NUM_SERVOS];
static uint32_t g_jogDur[NUM_SERVOS];

// RUN-path cosine ease (commanded µs). Frozen at group attach-complete.
static int g_rampFrom[NUM_SERVOS];
static uint32_t g_rampDist = 0;
static uint32_t g_rampDur = 0;
static unsigned long g_rampT0 = 0;
static unsigned long g_rampLastMs = 0;  // last tick the ramp clock was allowed to advance
static bool g_rampOn = false;

static bool isHugCh(uint8_t ch) {
  return ch == CH_ELBOW2 || ch == CH_WRIST2;
}

static int softMinOf(uint8_t ch) { return g_cal[ch].softMin; }
static int softMaxOf(uint8_t ch) { return g_cal[ch].softMax; }

static void pushLimits(uint8_t ch);
static void pushAllLimits();

static int clampToSoft(uint8_t ch, int us) {
  return constrain(us, softMinOf(ch), softMaxOf(ch));
}

static int homeUs(uint8_t ch) {
  return clampToSoft(ch, g_poseClosed[ch]);
}

// Dest = taught CLOSED + signed span. Open/hug µs are the other end of
// that span on THIS wing — not a copy of the other wing's absolute µs.
static int spanFromClosed(uint8_t ch, int taughtUs) {
  return clampToSoft(ch, taughtUs) - homeUs(ch);
}

static int fromClosed(uint8_t ch, int span) {
  return clampToSoft(ch, homeUs(ch) + span);
}

uint8_t servosGetEffectiveSpeedPct() {
  if (!g_brakeReady) return SPEED_SCALE_PCT;
  return g_speedScalePct;
}

uint8_t servosGetChSpeedPct(uint8_t ch) {
  if (ch >= NUM_SERVOS) return 100;
  uint8_t p = g_chSpeedPct[ch];
  return (p < 1) ? 100 : p;
}

uint8_t servosGetSpeedPct() { return g_speedScalePct; }

static uint32_t specCruiseUsPerSec() {
  uint32_t noLoad =
      ((uint32_t)SERVO_SPEC_SPAN_US * 60ul * 1000ul) /
      ((uint32_t)SERVO_SPEC_TRAVEL_DEG * (uint32_t)SERVO_SPEC_MS_PER_60DEG);
  return noLoad * (uint32_t)SERVO_SPEC_LOAD_PCT / 100ul;
}

// Command cruise from DS51150 spec × load derate × global × per-ch × path tier.
static uint32_t cruiseUsPerSec(uint8_t ch) {
  uint32_t spec = specCruiseUsPerSec();
  uint8_t pct = servosGetEffectiveSpeedPct();
  if (pct < 1) pct = 1;
  uint8_t cp = servosGetChSpeedPct(ch);
  uint32_t v = spec * (uint32_t)pct / 100ul * (uint32_t)cp / 100ul;
  if (g_speed == SPD_HALF) v /= 2ul;
  else if (g_speed == SPD_QUARTER) v /= 4ul;
  if (v < 80ul) v = 80ul;
  return v;
}

static uint32_t armCruiseUsPerSec() {
  uint32_t v = specCruiseUsPerSec() * (uint32_t)SPEED_SCALE_PCT / 100ul;
  if (v < 80ul) v = 80ul;
  return v;
}

static uint32_t cosineDurMs(uint32_t dist, uint32_t v) {
  if (dist == 0) return MIN_STAGE_HOLD_MS;
  if (v < 1) v = 1;
  uint32_t ms = (uint32_t)((uint64_t)dist * 3142ull / (2ull * v));
  if (ms < MIN_STAGE_HOLD_MS) ms = MIN_STAGE_HOLD_MS;
  return ms;
}

static float cosineFrac(uint32_t elapsed, uint32_t dur) {
  if (dur < 1 || elapsed >= dur) return 1.f;
  return 0.5f * (1.f - cosf((float)M_PI * ((float)elapsed / (float)dur)));
}

void servosSetSpeedPct(uint8_t pct) {
  if (pct < 1) pct = 1;
  if (pct > 100) pct = 100;
  g_speedScalePct = pct;
  Serial.printf("SPEED %u%%%s\n", (unsigned)g_speedScalePct,
                g_brakeReady ? "" : " (COLD crawl 10%)");
}

void servosSetChSpeedPct(uint8_t ch, uint8_t pct) {
  if (ch >= NUM_SERVOS) return;
  if (pct < 1) pct = 1;
  if (pct > 100) pct = 100;
  g_chSpeedPct[ch] = pct;
  Serial.printf("CHSPEED %s %u%%\n", SERVO_NAMES[ch], (unsigned)pct);
}

// Both exist only to preserve the stored value and the BLE status field. Nothing
// reads g_accelMs, so the only caller of the setter is the NVS load path.
uint16_t servosGetAccelMs() { return (uint16_t)g_accelMs; }

void servosSetAccelMs(uint16_t ms) {
  if (ms < RAMP_ACCEL_MIN_MS) ms = RAMP_ACCEL_MIN_MS;
  if (ms > RAMP_ACCEL_MAX_MS) ms = RAMP_ACCEL_MAX_MS;
  g_accelMs = ms;
}

void servosClampSoftInsideHard(uint8_t ch) {
  ChannelCal& c = g_cal[ch];
  c.hardMin = constrain(c.hardMin, SERVO_ABS_MIN, SERVO_ABS_MAX);
  c.hardMax = constrain(c.hardMax, SERVO_ABS_MIN, SERVO_ABS_MAX);
  if (c.hardMin >= c.hardMax - 20) {
    c.hardMin = DEFAULT_HARD_MIN;
    c.hardMax = DEFAULT_HARD_MAX;
  }
  c.softMin = constrain(c.softMin, c.hardMin, c.hardMax - 20);
  c.softMax = constrain(c.softMax, c.softMin + 20, c.hardMax);
  c.center = constrain(c.center, c.softMin, c.softMax);
  // Every limit setter funnels through here, so this is the one place that has to
  // tell the driver. Tightening a window under a live or prepared channel makes
  // the driver re-clamp rather than jump.
  pushLimits(ch);
}

void servosDefaultPosesFromCal() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    if (isHugCh(i)) {
      g_poseClosed[i] = g_cal[i].center;
      g_poseOpen[i] = g_cal[i].center;
      g_poseHug[i] = g_cal[i].softMax;
    } else {
      g_poseClosed[i] = g_cal[i].softMin;
      g_poseOpen[i] = g_cal[i].softMax;
      g_poseHug[i] = g_cal[i].softMax;
    }
    g_poseClosed[i] = (int16_t)clampToSoft(i, g_poseClosed[i]);
    g_poseOpen[i] = (int16_t)clampToSoft(i, g_poseOpen[i]);
    g_poseHug[i] = (int16_t)clampToSoft(i, g_poseHug[i]);
  }
}

void servosAlignActualToTargets() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    g_actual[i] = g_target[i];
  }
  g_haveCommanded = true;
}

void servosApplyLastCmd(const int16_t us[NUM_SERVOS]) {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    int last = clampToSoft(i, us[i]);
    int closed = homeUs(i);
    g_actual[i] = last;
    if (abs(last - closed) > ARRIVE_US) {
      Serial.printf("BOOT %s lastcmd %d (away from closed %d)\n",
                    SERVO_NAMES[i], last, closed);
    }
  }
  g_haveCommanded = true;
}

void servosAuditEnvelope() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    const int c = homeUs(i);
    const int o = fromClosed(i, spanFromClosed(i, g_poseOpen[i]));
    const int h = fromClosed(i, spanFromClosed(i,
                      isHugCh(i) ? g_poseHug[i] : g_poseOpen[i]));
    int lo = c, hi = c;
    if (o < lo) lo = o;
    if (o > hi) hi = o;
    if (h < lo) lo = h;
    if (h > hi) hi = h;
    const int smin = softMinOf(i);
    const int smax = softMaxOf(i);
    if (lo < smin || hi > smax) {
      Serial.printf("LIMIT %s taught %d..%d outside soft %d..%d\n",
                    SERVO_NAMES[i], lo, hi, smin, smax);
    } else {
      Serial.printf("LIMIT %s span open=%+d hug=%+d room %d below / %d above\n",
                    SERVO_NAMES[i], o - c, h - c, lo - smin, smax - hi);
    }
  }
  // Runs right after the stored calibration is applied, and store writes g_cal
  // through servosCal() without going near the setters, so this is where NVS
  // limits reach the driver.
  pushAllLimits();
}

void servosApplyDefaults() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    g_cal[i].hardMin = DEFAULT_HARD_MIN;
    g_cal[i].hardMax = DEFAULT_HARD_MAX;
    g_cal[i].softMin = DEFAULT_SOFT_MIN;
    g_cal[i].softMax = DEFAULT_SOFT_MAX;
    g_cal[i].center = SERVO_CENTER;
    g_cal[i].invert = 0;
    g_cal[i].pad = 0;
    g_sense[i] = 0;
    g_target[i] = SERVO_CENTER;
    g_actual[i] = SERVO_CENTER;
  }
  servosDefaultPosesFromCal();
  g_pose = POSE_CLOSED;
  pushAllLimits();
}

void abortPath() {
  g_path = PATH_NONE;
  g_step = 0;
  g_speed = SPD_FULL;
  g_speed2 = SPD_FULL;
  g_runMotion = false;
  g_forceDwell = false;
  g_group = -1;
  g_groupHoldUntil = 0;
  g_nStages = 0;
  g_rampOn = false;
  g_flapOn = false;
  g_flapPending = false;
  g_armSeqOn = false;
}

static bool chAttached(uint8_t ch) { return sdAttached(ch); }

/*
  States intent. The driver clamps to the configured window and moves the emitted
  pulse there no faster than PULSE_MAX_STEP_US per frame, so a caller cannot
  produce a step the operator could not interrupt — including this file's own
  ramp catching up after a stalled loop.
*/
static void commandUs(uint8_t ch, int us) {
  if (ch >= NUM_SERVOS || !sdAttached(ch)) return;
  sdCommand(ch, us);
}

/*
  Calibration lives here, enforcement lives in the driver, so any change to the
  window has to be pushed down. Nothing outside these limits is ever emitted,
  whatever this file asks for.
*/
static void pushLimits(uint8_t ch) {
  if (ch >= NUM_SERVOS) return;
  sdSetLimits(ch, g_cal[ch].hardMin, g_cal[ch].hardMax,
              g_cal[ch].softMin, g_cal[ch].softMax);
}

static void pushAllLimits() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) pushLimits(i);
}

/*
  storeSaveLastCmd writes NVS, which blocks for milliseconds. It used to run from
  inside a release, putting a flash write in the middle of the motion path. It is
  deferred to an idle tick instead — see serviceLastCmdSave.
*/
static bool g_lastCmdDirty = false;

static void traceNoteLoopCore();  // defined with the trace ring below

void servosBegin() {
  traceNoteLoopCore();
  servosApplyDefaults();
  g_armed = false;
  g_haveCommanded = false;
  g_cycledMask = 0;
  g_brakeReady = false;
  g_poseKnown = false;
  g_jogMask = 0;
  g_flapOn = false;
  g_flapPending = false;
  g_flapPreamble = false;
  g_flapHome = false;
  g_runMotion = false;
  g_forceDwell = false;
  g_group = -1;
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    g_chSpeedPct[i] = 100;
    g_setupIdleAt[i] = 0;
  }
  abortPath();

  // Configures the one shared timer, parks all five pads and proves the phase
  // reference is toggling. A false return means nothing is allowed to attach, so
  // it is worth saying out loud rather than discovering it at ARM time.
  if (!sdBegin()) {
    Serial.println(F("*** SERVO DRIVER REFUSED TO START — nothing can attach ***"));
  }
  pushAllLimits();
}

ChannelCal& servosCal(uint8_t ch) { return g_cal[ch]; }
bool servosIsArmed() { return g_armed; }
bool servosIsBrakeReady() { return g_brakeReady; }
uint8_t servosAttachMask() { return sdAttachMask(); }
WingPose getWingPose() { return g_pose; }
bool pathIsActive() { return g_path != PATH_NONE || g_flapOn || g_armSeqOn; }
PathId pathGet() { return g_path; }

int servosGetTarget(uint8_t ch) {
  return (ch < NUM_SERVOS) ? g_target[ch] : 0;
}
int servosGetActual(uint8_t ch) {
  return (ch < NUM_SERVOS) ? g_actual[ch] : 0;
}

uint8_t servosGetSense(uint8_t ch) {
  return (ch < NUM_SERVOS) ? g_sense[ch] : 0;
}

void servosSetSense(uint8_t ch, bool flip) {
  if (ch >= NUM_SERVOS) return;
  g_sense[ch] = flip ? 1 : 0;
}

int16_t servosGetPoseUs(uint8_t ch, WingPose pose) {
  if (ch >= NUM_SERVOS) return 0;
  if (pose == POSE_OPEN) return g_poseOpen[ch];
  if (pose == POSE_HUG) return g_poseHug[ch];
  return g_poseClosed[ch];
}

void servosSetPoseUs(uint8_t ch, WingPose pose, int us) {
  if (ch >= NUM_SERVOS) return;
  int v = clampToSoft(ch, us);
  if (pose == POSE_OPEN) g_poseOpen[ch] = (int16_t)v;
  else if (pose == POSE_HUG) g_poseHug[ch] = (int16_t)v;
  else g_poseClosed[ch] = (int16_t)v;
}

void servosTeachPose(uint8_t ch, WingPose pose) {
  if (ch >= NUM_SERVOS) return;
  servosSetPoseUs(ch, pose, g_target[ch]);
  Serial.printf("TEACH %s %s = %d\n", SERVO_NAMES[ch],
                pose == POSE_OPEN ? "OPEN" : pose == POSE_HUG ? "HUG" : "CLOSED",
                (int)servosGetPoseUs(ch, pose));
}

void servosSetTarget(uint8_t ch, int us) {
  if (ch >= NUM_SERVOS) return;
  g_target[ch] = clampToSoft(ch, us);
}

void servosSetTargets(const int16_t us[NUM_SERVOS]) {
  (void)us;
  // Dead. Old pair sync copied one wing's µs onto the other and left
  // lastcmd at hug/open. Do not write dest or actual from this path.
  Serial.println(F("SET_TARGETS ignored"));
}

static bool attachOne(uint8_t i);

static void bumpSetupIdle(uint8_t ch) {
  if (ch < NUM_SERVOS) g_setupIdleAt[ch] = 0;
}

static void startJogEaseAt(uint8_t ch, uint32_t v) {
  if (ch >= NUM_SERVOS) return;
  g_jogFrom[ch] = g_actual[ch];
  g_jogDur[ch] = cosineDurMs((uint32_t)abs(g_target[ch] - g_actual[ch]), v);
  g_jogT0[ch] = millis();
  g_jogMask |= (1u << ch);
  bumpSetupIdle(ch);
}

static void startJogEase(uint8_t ch) {
  startJogEaseAt(ch, cruiseUsPerSec(ch));
}

void servosJog(uint8_t ch, int deltaUs) {
  if (ch >= NUM_SERVOS) return;
  abortPath();
  int signedDelta = g_sense[ch] ? deltaUs : -deltaUs;
  if (!chAttached(ch)) g_target[ch] = g_actual[ch];
  servosSetTarget(ch, g_target[ch] + signedDelta);
  if (!attachOne(ch)) return;
  g_armed = true;
  startJogEase(ch);
  Serial.printf("JOG ch=%u pin=%u tgt=%d\n",
                (unsigned)ch, SERVO_PINS[ch], g_target[ch]);
}

void servosSetSoftMin(uint8_t ch, int us) {
  if (ch >= NUM_SERVOS) return;
  g_cal[ch].softMin = us;
  servosClampSoftInsideHard(ch);
  g_target[ch] = clampToSoft(ch, g_target[ch]);
}
void servosSetSoftMax(uint8_t ch, int us) {
  if (ch >= NUM_SERVOS) return;
  g_cal[ch].softMax = us;
  servosClampSoftInsideHard(ch);
  g_target[ch] = clampToSoft(ch, g_target[ch]);
}
void servosSetHardMin(uint8_t ch, int us) {
  if (ch >= NUM_SERVOS) return;
  g_cal[ch].hardMin = us;
  servosClampSoftInsideHard(ch);
}
void servosSetHardMax(uint8_t ch, int us) {
  if (ch >= NUM_SERVOS) return;
  g_cal[ch].hardMax = us;
  servosClampSoftInsideHard(ch);
}
void servosSetCenter(uint8_t ch, int us) {
  if (ch >= NUM_SERVOS) return;
  g_cal[ch].center = constrain(us, softMinOf(ch), softMaxOf(ch));
}
// ---------------------------------------------------------------- trace
/*
  Diagnostic ring for the motion path. Nothing here prints: a USB CDC write
  blocks for milliseconds and the fault being hunted is timing-sensitive, so
  printing inline would perturb what we are trying to observe. Dump with the
  serial `trace` command after a wig-out, while the board is still powered.

  This originally existed to catch the first-pulse fragment: a connect landing
  inside the frame's HIGH phase gives the servo only the remainder of that pulse,
  and a remainder in the 500-900 us band is a valid command to a position well
  below target. The driver now makes that impossible by construction — handovers
  happen in the idle gap, verified on hardware over 2500 attaches with zero short
  pulses — so the fragment probe is gone and the interesting rows are now the
  REFUSE ones: what the driver declined to do, and why.
*/

enum TraceEv : uint8_t {
  TR_ATTACH = 1,
  TR_DETACH,
  TR_JUMP,
  TR_DONE,
  TR_STALL,
  TR_BLOCK,   // service gap caused by this firmware's own blocking attach
  TR_REFUSE,  // driver declined a handover; detail = SdResult
};

// Not a servo index: keeps stage/aggregate rows out of the channel column.
static const uint8_t TR_NO_CH = 0xFE;

struct TraceRec {
  uint32_t tMs;
  uint8_t  ev;
  uint8_t  ch;
  uint8_t  padWasHigh;  // pad level sampled before the transition
  uint8_t  ctx;         // low nibble = path id, high nibble = stage + 1
  int16_t  us;          // commanded value, or elapsed ms for TR_DONE
  int16_t  detail;      // ATTACH: first-pulse fragment us, 0 = landed in the gap
                        // DETACH: settle dwell remaining ms, -1 = no group hold
                        // JUMP:   step size us
                        // DONE:   ramp duration ms
                        // STALL:  service gap ms
};

static TraceRec g_trace[192];
static const uint16_t TRACE_N = (uint16_t)(sizeof(g_trace) / sizeof(g_trace[0]));
static uint16_t g_traceHead = 0;
static uint32_t g_traceTotal = 0;

// A short first pulse is the event this whole exercise exists to catch, and the
// main ring holds only about a dozen moves. Keeping those rows in a separate
// list means ordinary traffic — including a recovery move after a wig-out —
// cannot scroll the evidence away.
static TraceRec g_slam[8];
static const uint8_t SLAM_N = (uint8_t)(sizeof(g_slam) / sizeof(g_slam[0]));
static uint8_t  g_slamHead = 0;
static uint32_t g_slamTotal = 0;

// Attaches that ran since the last service tick, so a gap caused by
// the driver's own gap and latch waits are not misread as a ramp stall.
static uint8_t g_attachSinceService = 0;

static uint8_t traceCtx() {
  const uint8_t p = (uint8_t)((uint8_t)g_path & 0x0F);
  const uint8_t g = (uint8_t)((g_group < 0) ? 0 : (uint8_t)((g_group + 1) & 0x0F));
  return (uint8_t)((g << 4) | p);
}

static int16_t traceClamp(int v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}

static void traceAdd(uint8_t ev, uint8_t ch, uint8_t padHigh, int us, int detail) {
  TraceRec r;
  r.tMs = millis();
  r.ev = ev;
  r.ch = ch;
  r.padWasHigh = padHigh;
  r.ctx = traceCtx();
  r.us = traceClamp(us);
  r.detail = traceClamp(detail);

  g_trace[g_traceHead] = r;
  g_traceHead = (uint16_t)((g_traceHead + 1) % TRACE_N);
  if (g_traceTotal != 0xFFFFFFFFu) g_traceTotal++;

  // A refused handover is the sticky event now. Nothing moved, but the driver
  // declining means a stage did not start, and that must survive later traffic
  // in the ring the same way a slam used to.
  if (ev == TR_REFUSE) {
    g_slam[g_slamHead] = r;
    g_slamHead = (uint8_t)((g_slamHead + 1) % SLAM_N);
    if (g_slamTotal != 0xFFFFFFFFu) g_slamTotal++;
  }
}

// servosBegin runs on the Arduino loop task, so this records the core that owns
// the motion path. Reported by the trace dump, since a motion command arriving
// on the NimBLE task instead is itself a finding.
static int g_loopCore = -1;
static void traceNoteLoopCore() { g_loopCore = (int)xPortGetCoreID(); }

void servosTraceClear() {
  g_traceHead = 0;
  g_traceTotal = 0;
  g_slamHead = 0;
  g_slamTotal = 0;
  Serial.println(F("TRACE cleared"));
}

static void tracePrintRow(const TraceRec& r) {
  const char* ev = "?";
  switch (r.ev) {
    case TR_ATTACH: ev = "ATTACH"; break;
    case TR_DETACH: ev = "DETACH"; break;
    case TR_JUMP:   ev = "JUMP";   break;
    case TR_DONE:   ev = "DONE";   break;
    case TR_STALL:  ev = "STALL";  break;
    case TR_BLOCK:  ev = "BLOCK";  break;
    case TR_REFUSE: ev = "REFUSE"; break;
    default: break;
  }

  const char* flag = "";
  if (r.ev == TR_REFUSE) {
    flag = sdResultName((SdResult)r.detail);
  } else if (r.ev == TR_DETACH && r.detail >= 0 && r.detail < 20) {
    flag = "  released inside one frame of the final write";
  }

  Serial.printf("  %8lu  %-6s  %-11s  %d   p%u s%u  %6d  %6d%s\n",
                (unsigned long)r.tMs, ev,
                (r.ch < NUM_SERVOS) ? SERVO_NAMES[r.ch] : "-",
                (int)r.padWasHigh,
                (unsigned)(r.ctx & 0x0F), (unsigned)(r.ctx >> 4),
                (int)r.us, (int)r.detail, flag);
}

void servosTraceDump() {
  Serial.println(F("REFUSE detail = SdResult; the driver declined, nothing connected"));
  Serial.println(F("DETACH detail = settle dwell still remaining, ms (-1 = no group hold)"));
  Serial.println(F("padHigh       = pad was HIGH immediately before the transition"));
  Serial.println(F("p/s           = path id / stage index at the time"));
  Serial.printf("driver        : refOk=%d faultMask=0x%02X loopCore=%d\n",
                sdRefOk() ? 1 : 0, sdFaultMask(), g_loopCore);

  const uint8_t slamShown = (g_slamTotal < SLAM_N) ? (uint8_t)g_slamTotal : SLAM_N;
  Serial.printf("\nREFUSED HANDOVERS: %lu total, last %u kept\n",
                (unsigned long)g_slamTotal, (unsigned)slamShown);
  if (slamShown == 0) {
    Serial.println(F("  none — every handover the driver was asked for succeeded"));
  } else {
    Serial.println(F("      t_ms  event   channel      pad  path     us  detail"));
    for (uint8_t k = 0; k < slamShown; k++) {
      tracePrintRow(g_slam[(uint8_t)((g_slamHead + SLAM_N - slamShown + k) % SLAM_N)]);
    }
  }

  const uint16_t count = (g_traceTotal < TRACE_N) ? (uint16_t)g_traceTotal : TRACE_N;
  Serial.printf("\nTRACE: %lu events, last %u kept\n",
                (unsigned long)g_traceTotal, (unsigned)count);
  Serial.println(F("      t_ms  event   channel      pad  path     us  detail"));
  for (uint16_t k = 0; k < count; k++) {
    tracePrintRow(g_trace[(uint16_t)((g_traceHead + TRACE_N - count + k) % TRACE_N)]);
  }
}

/*
  Adopts whatever the driver actually prepared, which may be tighter than what was
  asked for. Comparing against sdPreparedUs rather than trusting that sdPrepared
  is true matters: the position the servo wakes up holding is the prepared value,
  not the request, and the two differ whenever the soft window clamps.
*/
static uint8_t groupCount(int g);
static uint8_t groupChAt(int g, uint8_t idx);

static void adoptPrepared(uint8_t ch) {
  g_actual[ch] = sdPreparedUs(ch);
  if (g_attachSinceService < 255) g_attachSinceService++;
}

static bool attachAtUs(uint8_t i, int us) {
  if (i >= NUM_SERVOS) return false;
  if (sdAttached(i)) return true;

  const uint8_t wasHigh = gpio_get_level((gpio_num_t)SERVO_PINS[i]) ? 1 : 0;
  const SdResult r = sdAttachNow(i, constrain(us, SERVO_ABS_MIN, SERVO_ABS_MAX));
  if (r != SD_OK) {
    // A refusal is the driver working. Nothing is connected, so the caller's
    // stage simply does not start.
    Serial.printf("ATTACH refuse %s — %s\n", SERVO_NAMES[i], sdResultName(r));
    traceAdd(TR_REFUSE, i, wasHigh, us, (int)r);
    return false;
  }
  adoptPrepared(i);
  traceAdd(TR_ATTACH, i, wasHigh, g_actual[i], 0);
  Serial.printf("PWM ch%u %s -> GPIO%u us=%d\n",
                (unsigned)i, SERVO_NAMES[i], SERVO_PINS[i], g_actual[i]);
  return true;
}

/*
  Hands a whole stage over inside ONE idle gap, in one critical section.

  This is what the single shared timer buys and it is measured, not assumed: on
  hardware both pads of a pair rise on the same counter reset, worst skew 0 us
  over 500 group cycles. One gap wait for the stage instead of one per servo, and
  either the whole stage goes live or none of it does — a half-attached stage
  mid-move is worse than a refused one.
*/
static bool attachStageTogether(int g) {
  uint8_t chs[MAX_STAGE_CH];
  uint8_t n = 0;

  for (uint8_t i = 0; i < groupCount(g); i++) {
    const uint8_t ch = groupChAt(g, i);
    if (ch >= NUM_SERVOS || sdAttached(ch)) continue;
    if (n >= MAX_STAGE_CH) break;   // groupCount cannot exceed this, but chs is a buffer
    const SdResult p = sdPrepare(ch, clampToSoft(ch, g_actual[ch]));
    if (p != SD_OK) {
      Serial.printf("STAGE refuse %s — prepare %s\n", SERVO_NAMES[ch], sdResultName(p));
      return false;
    }
    chs[n++] = ch;
  }
  if (n == 0) return true;

  // One latch wait for the stage rather than one per servo: the channels share a
  // timer, so they all go live on the same frame.
  const uint32_t t0 = millis();
  for (;;) {
    bool live = true;
    for (uint8_t k = 0; k < n; k++) if (!sdDutyLive(chs[k])) live = false;
    if (live) break;
    if ((uint32_t)(millis() - t0) > SD_LIVE_TIMEOUT_MS) {
      Serial.println(F("STAGE refuse — duty never went live"));
      return false;
    }
  }

  uint8_t wasHigh[MAX_STAGE_CH];
  for (uint8_t k = 0; k < n; k++) {
    wasHigh[k] = gpio_get_level((gpio_num_t)SERVO_PINS[chs[k]]) ? 1 : 0;
  }

  const SdResult r = sdAttachGroup(chs, n);
  if (r != SD_OK) {
    Serial.printf("STAGE refuse — group attach %s\n", sdResultName(r));
    traceAdd(TR_REFUSE, TR_NO_CH, 0, (int)n, (int)r);
    return false;
  }

  for (uint8_t k = 0; k < n; k++) {
    adoptPrepared(chs[k]);
    traceAdd(TR_ATTACH, chs[k], wasHigh[k], g_actual[chs[k]], 0);
  }
  return true;
}

static bool attachOne(uint8_t i) {
  if (i >= NUM_SERVOS) return false;
  if (chAttached(i)) return true;
  if (!g_haveCommanded) {
    Serial.printf("ATTACH refuse %s — no lastcmd\n", SERVO_NAMES[i]);
    return false;
  }
  int us = clampToSoft(i, g_actual[i]);
  g_actual[i] = us;
  return attachAtUs(i, us);
}

static bool attachArmClosed(uint8_t ch) {
  if (ch >= NUM_SERVOS) return false;
  int us = (int)g_poseClosed[ch];
  if (us < SERVO_ABS_MIN || us > SERVO_ABS_MAX) {
    Serial.printf("ARM refuse %s closed=%d not in %d..%d\n",
                  SERVO_NAMES[ch], us, SERVO_ABS_MIN, SERVO_ABS_MAX);
    return false;
  }
  // Taught closed is ARM authority. Do not soft-clamp toward open.
  g_haveCommanded = true;
  g_actual[ch] = us;
  g_target[ch] = us;
  return attachAtUs(ch, us);
}

static void seqAdd1(uint8_t ch) {
  if (g_nStages >= MAX_STAGES) return;
  g_stageCh[g_nStages][0] = ch;
  for (uint8_t k = 1; k < MAX_STAGE_CH; k++) g_stageCh[g_nStages][k] = 255;
  g_stageN[g_nStages] = 1;
  g_nStages++;
}

static void seqAdd2(uint8_t a, uint8_t b) {
  if (g_nStages >= MAX_STAGES) return;
  g_stageCh[g_nStages][0] = a;
  g_stageCh[g_nStages][1] = b;
  for (uint8_t k = 2; k < MAX_STAGE_CH; k++) g_stageCh[g_nStages][k] = 255;
  g_stageN[g_nStages] = 2;
  g_nStages++;
}

static void seqAdd3(uint8_t a, uint8_t b, uint8_t c) {
  if (g_nStages >= MAX_STAGES) return;
  g_stageCh[g_nStages][0] = a;
  g_stageCh[g_nStages][1] = b;
  g_stageCh[g_nStages][2] = c;
  g_stageN[g_nStages] = 3;
  g_nStages++;
}

static bool chNeedsMove(uint8_t ch);

// Open/close: all three raises together (WR+ER+SH). Hugs stay a pair.
// D force-dwell: hugs one-at-a-time, then the three raises together.
static void buildSeq() {
  g_nStages = 0;
  const bool openLike =
      (g_path == PATH_OPEN ||
       (g_path == PATH_OPEN_THEN_HUG && g_step == 0) ||
       (g_path == PATH_UNHUG_THEN_OPEN && g_step == 1));
  const bool closeLike =
      (g_path == PATH_CLOSE ||
       (g_path == PATH_UNHUG_THEN_CLOSE && g_step == 1));
  const bool hugLike =
      (g_path == PATH_HUG ||
       (g_path == PATH_OPEN_THEN_HUG && g_step == 1));
  const bool unhugLike =
      ((g_path == PATH_UNHUG_THEN_CLOSE || g_path == PATH_UNHUG_THEN_OPEN) &&
       g_step == 0);

  if (openLike) {
    if (chNeedsMove(CH_WRIST2) || chNeedsMove(CH_ELBOW2))
      seqAdd2(CH_WRIST2, CH_ELBOW2);
    seqAdd3(CH_WRIST1, CH_ELBOW1, CH_SHOULDER);
  } else if (closeLike) {
    if (g_forceDwell) {
      seqAdd1(CH_WRIST2);
      seqAdd1(CH_ELBOW2);
    } else if (chNeedsMove(CH_WRIST2) || chNeedsMove(CH_ELBOW2)) {
      seqAdd2(CH_WRIST2, CH_ELBOW2);
    }
    seqAdd3(CH_WRIST1, CH_ELBOW1, CH_SHOULDER);
  } else if (hugLike || unhugLike) {
    seqAdd2(CH_WRIST2, CH_ELBOW2);
  }

  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    bool inSeq = false;
    for (uint8_t s = 0; s < g_nStages && !inSeq; s++) {
      for (uint8_t k = 0; k < g_stageN[s]; k++) {
        if (g_stageCh[s][k] == i) { inSeq = true; break; }
      }
    }
    if (!inSeq) g_target[i] = g_actual[i];
  }
  Serial.printf("SEQ path=%u step=%u stages=%u dwell=%d\n",
                (unsigned)g_path, (unsigned)g_step, (unsigned)g_nStages,
                g_forceDwell ? 1 : 0);
}

static uint8_t groupCount(int g) {
  if (g < 0 || g >= (int)g_nStages) return 0;
  return g_stageN[g];
}

static uint8_t groupChAt(int g, uint8_t idx) {
  if (g < 0 || g >= (int)g_nStages || idx >= g_stageN[g]) return 255;
  return g_stageCh[g][idx];
}

/*
  Arrival is judged on what the servo has actually been GIVEN, not on what the
  ramp asked for.

  g_actual is the commanded value and reaches the target as soon as the cosine
  finishes; sdWritten is what the driver has emitted, and it lags while the driver
  slews. Testing g_actual would report arrival with the pulse still travelling,
  which is how the release used to fire before the servo got there.
*/
static bool chNeedsMove(uint8_t ch) {
  if (ch >= NUM_SERVOS) return false;
  const int at = sdAttached(ch) ? sdWritten(ch) : g_actual[ch];
  return abs(at - g_target[ch]) > ARRIVE_US;
}

static uint8_t stageAttachedCount(int g) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < groupCount(g); i++) {
    const uint8_t ch = groupChAt(g, i);
    if (ch < NUM_SERVOS && sdAttached(ch)) n++;
  }
  return n;
}

// True once the driver has emitted everything commanded for the stage, so the
// ramp is not running ahead of the hardware.
static bool groupSettled(int g) {
  for (uint8_t i = 0; i < groupCount(g); i++) {
    const uint8_t ch = groupChAt(g, i);
    if (ch < NUM_SERVOS && sdAttached(ch) && !sdSettled(ch)) return false;
  }
  return true;
}

static bool groupArrived(int g) {
  for (uint8_t i = 0; i < groupCount(g); i++) {
    uint8_t ch = groupChAt(g, i);
    if (chNeedsMove(ch)) return false;
  }
  return true;
}

static unsigned long groupDwellMs(int g) {
  uint32_t hold = MIN_STAGE_HOLD_MS;
  for (uint8_t i = 0; i < groupCount(g); i++) {
    uint8_t ch = groupChAt(g, i);
    int d = abs((int)g_poseOpen[ch] - (int)g_poseClosed[ch]);
    int dh = abs((int)g_poseHug[ch] - (int)g_poseClosed[ch]);
    if (dh > d) d = dh;
    uint32_t ms = cosineDurMs((uint32_t)d, cruiseUsPerSec(ch));
    if (ms > hold) hold = ms;
  }
  return hold;
}

static void beginGroupRamp(int g) {
  g_rampOn = false;
  g_rampDist = 0;
  g_rampDur = MIN_STAGE_HOLD_MS;
  for (uint8_t i = 0; i < NUM_SERVOS; i++) g_rampFrom[i] = g_actual[i];

  for (uint8_t i = 0; i < groupCount(g); i++) {
    uint8_t ch = groupChAt(g, i);
    if (ch >= NUM_SERVOS) continue;
    uint32_t d = (uint32_t)abs(g_target[ch] - g_actual[ch]);
    if (d > g_rampDist) g_rampDist = d;
    uint32_t ms = cosineDurMs(d, cruiseUsPerSec(ch));
    if (ms > g_rampDur) g_rampDur = ms;
  }

  g_rampT0 = millis();
  g_rampLastMs = g_rampT0;
  g_rampOn = true;
  Serial.printf("EASE dist=%lu dur=%lu\n",
                (unsigned long)g_rampDist, (unsigned long)g_rampDur);
}

static void serviceCosineRamp() {
  if (!g_rampOn || g_group < 0) return;
  unsigned long now = millis();

  /*
    While the driver is still catching up, hold the ramp clock rather than letting
    it run. Advancing it would put the profile ahead of the hardware, and the
    difference would then arrive as one large step the moment the driver caught up
    — exactly the shape of a jump. Pushing T0 forward by the skipped interval
    preserves the ease rather than re-basing it, which would restart the curve.
  */
  if (!groupSettled(g_group)) {
    g_rampT0 += (unsigned long)(now - g_rampLastMs);
    g_rampLastMs = now;
    return;
  }
  g_rampLastMs = now;

  uint32_t elapsed = (uint32_t)(now - g_rampT0);
  float s = cosineFrac(elapsed, g_rampDur);
  bool done = (elapsed >= g_rampDur) || (g_rampDist == 0);

  for (uint8_t i = 0; i < groupCount(g_group); i++) {
    uint8_t ch = groupChAt(g_group, i);
    if (ch >= NUM_SERVOS || !chAttached(ch)) continue;
    int from = g_rampFrom[ch];
    int to = g_target[ch];
    int span = to - from;
    int next = (done || span == 0) ? to : from + (int)lroundf((float)span * s);
    if (next != g_actual[ch]) {
      // A cosine tick is single-digit us at these durations. Anything large
      // means the service loop stalled and the ramp is catching up in one step.
      const int step = next - g_actual[ch];
      if (step > 80 || step < -80) traceAdd(TR_JUMP, ch, 0, next, step);
      g_actual[ch] = next;
      commandUs(ch, g_actual[ch]);
    }
  }

  if (done) {
    g_rampOn = false;
    traceAdd(TR_DONE, TR_NO_CH, 0, (int)elapsed, (int)g_rampDur);
  }
}

/*
  Hands the pad back to the driver, which times the release so no pulse is cut
  short and rests the pad at 0 V.

  A false return is not cosmetic. The driver could not prove the frame was idle,
  so it deliberately LEFT THE PAD ROUTED rather than truncate a pulse — the servo
  is still being driven and only the supply e-stop will stop it. That cannot be
  silent, so it prints even though printing from the motion path is otherwise
  avoided: by this point there is nothing left to perturb.
*/
static bool releaseOnePin(uint8_t i) {
  const uint8_t wasHigh = gpio_get_level((gpio_num_t)SERVO_PINS[i]) ? 1 : 0;
  // Settle dwell still remaining. Now that the dwell is stamped at arrival this
  // should be a healthy positive number; it used to be stamped at attach and was
  // therefore long expired by the time a move finished.
  const int holdLeft = (g_groupHoldUntil == 0)
                         ? -1
                         : (int)((long)g_groupHoldUntil - (long)millis());

  const bool ok = sdDetach(i);
  traceAdd(TR_DETACH, i, wasHigh, g_actual[i], holdLeft);
  if (!ok) {
    Serial.printf("*** RELEASE REFUSED %s — PAD STILL LIVE, faultMask=0x%02X ***\n",
                  SERVO_NAMES[i], sdFaultMask());
    Serial.println(F("*** servo is still driven; cut the supply to stop it ***"));
  }
  return ok;
}

static void noteCycled(uint8_t i) {
  if (i >= NUM_SERVOS) return;
  if (abs(g_actual[i] - homeUs(i)) > ARRIVE_US) return;
  g_cycledMask |= (1u << i);
  if (!g_brakeReady &&
      g_cycledMask == (uint8_t)((1u << NUM_SERVOS) - 1u)) {
    g_brakeReady = true;
    g_poseKnown = true;
    Serial.println(F("BRAKE READY"));
  }
}

static void detachOne(uint8_t i) {
  if (!sdAttached(i)) return;
  /*
    Adopt what the servo was actually GIVEN before letting go of it.

    Mid-move the ramp has commanded further than the driver has emitted, and
    g_actual is what gets persisted as lastcmd and used as the next attach
    position. Keeping the commanded value here would record a position the horn
    never reached and produce a jump at the start of the next move. Also makes the
    noteCycled home test below judge where the servo is, not where it was going.
  */
  g_actual[i] = sdWritten(i);
  noteCycled(i);
  g_jogMask &= (uint8_t)~(1u << i);
  g_setupIdleAt[i] = 0;
  releaseOnePin(i);
  // Derived from the driver, so a refused release correctly leaves this armed.
  if (!g_runMotion) g_armed = (sdAttachMask() != 0);
  g_lastCmdDirty = true;
}

static void detachGroup(int g) {
  for (uint8_t i = 0; i < groupCount(g); i++) {
    uint8_t ch = groupChAt(g, i);
    if (ch < NUM_SERVOS) detachOne(ch);
  }
}

static void detachAll() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    if (!sdAttached(i)) continue;
    g_actual[i] = sdWritten(i);   // see detachOne
    g_jogMask &= (uint8_t)~(1u << i);
    g_setupIdleAt[i] = 0;
    releaseOnePin(i);
  }
  g_lastCmdDirty = true;
}

static void startPath(PathId path, SpeedTier s1, SpeedTier s2);

static void attachNextInGroup() {
  if (g_group < 0 || g_group >= (int)g_nStages) return;
  while (g_groupAttachIdx < groupCount(g_group)) {
    uint8_t ch = groupChAt(g_group, g_groupAttachIdx);
    g_groupAttachIdx++;
    if (ch >= NUM_SERVOS) continue;
    attachOne(ch);
    g_lastAttachMs = millis();
    return;
  }
}

static void kickGroup() {
  while (g_group >= 0 && g_group < (int)g_nStages) {
    g_groupAttachIdx = 0;
    g_groupHoldUntil = 0;  // stamped at ARRIVAL, not here — see serviceGroup
    // RUN open/close: the whole stage goes live on one frame. Staggering was a
    // jerk plus ~300 ms sat on the first raise before the others moved.
    if (!g_forceDwell) {
      if (attachStageTogether(g_group)) g_groupAttachIdx = groupCount(g_group);
      g_lastAttachMs = millis();
    } else {
      attachNextInGroup();
    }
    return;
  }
}

static void armGroupHold() {
  if (g_groupHoldUntil != 0) return;
  unsigned long hold = MIN_STAGE_HOLD_MS;
  if (g_forceDwell) {
    unsigned long d = groupDwellMs(g_group);
    if (d > hold) hold = d;
  }
  g_groupHoldUntil = millis() + hold;
}

static void serviceGroup() {
  if (!g_runMotion || g_group < 0 || g_group >= (int)g_nStages) return;

  if (g_groupAttachIdx < groupCount(g_group)) {
    unsigned long now = millis();
    if (now - g_lastAttachMs >= ATTACH_STAGGER_MS) attachNextInGroup();
    return;
  }

  /*
    A stage that attached nothing must not sit here forever.

    serviceCosineRamp only writes attached channels, so with none attached g_actual
    never moves, groupArrived never becomes true, and the path waits below with the
    wing parked mid-pose until someone presses STOP. Refusing the move outright is
    the honest outcome, and it leaves no pad pulsing.
  */
  if (stageAttachedCount(g_group) == 0) {
    Serial.printf("STAGE %d attached nothing — path aborted\n", (int)g_group);
    traceAdd(TR_REFUSE, TR_NO_CH, 0, g_group, (int)SD_NOT_PREPARED);
    abortPath();
    return;
  }

  // Attach complete — start the time-based cosine once, then wait for arrival + hold.
  if (!g_rampOn && !groupArrived(g_group)) beginGroupRamp(g_group);
  if (g_rampOn) return;
  if (!groupArrived(g_group)) return;

  // Wait for the driver to have emitted everything it was given before calling
  // the move finished. Arrival above is measured on sdWritten, so this is belt
  // and braces for a channel that arrived while another is still slewing.
  if (!groupSettled(g_group)) return;

  /*
    Dwell is stamped HERE, at arrival, which is the fix for the extra motion after
    a move ended.

    It used to be stamped when the attach completed, and g_rampDur almost always
    exceeds MIN_STAGE_HOLD_MS, so the dwell had already expired by the time the
    ramp finished. detachGroup then fired one service iteration after the final
    pulse write — a few milliseconds — which is not long enough for the frame to
    latch. The servo could lose the pad before ever receiving a pulse at the
    target position.
  */
  armGroupHold();
  if (millis() < g_groupHoldUntil) return;

  Serial.printf("STAGE %d done — pulse off\n", (int)g_group);
  detachGroup(g_group);
  g_group++;
  g_groupAttachIdx = 0;
  g_groupHoldUntil = 0;
  kickGroup();
}

static void startRunMotion(PathId path, SpeedTier s1, SpeedTier s2, bool forceDwell) {
  g_jogMask = 0;
  detachAll();  // Vin-brake leftovers so D/A/B never stack on a live raise
  startPath(path, s1, s2);
  g_runMotion = true;
  g_forceDwell = forceDwell;
  g_armed = true;
  g_group = 0;
  g_groupAttachIdx = 0;
  g_groupHoldUntil = 0;
  buildSeq();
  kickGroup();
}

static void applyParkHug() {
  g_target[CH_ELBOW2] = homeUs(CH_ELBOW2);
  g_target[CH_WRIST2] = homeUs(CH_WRIST2);
}

void applyPoseClosed() {
  g_target[CH_SHOULDER] = homeUs(CH_SHOULDER);
  g_target[CH_ELBOW1] = homeUs(CH_ELBOW1);
  g_target[CH_WRIST1] = homeUs(CH_WRIST1);
  applyParkHug();
}

void applyPoseOpen() {
  g_target[CH_SHOULDER] = fromClosed(CH_SHOULDER, spanFromClosed(CH_SHOULDER, g_poseOpen[CH_SHOULDER]));
  g_target[CH_ELBOW1] = fromClosed(CH_ELBOW1, spanFromClosed(CH_ELBOW1, g_poseOpen[CH_ELBOW1]));
  g_target[CH_WRIST1] = fromClosed(CH_WRIST1, spanFromClosed(CH_WRIST1, g_poseOpen[CH_WRIST1]));
  applyParkHug();
}

void applyPoseHug() {
  g_target[CH_SHOULDER] = fromClosed(CH_SHOULDER, spanFromClosed(CH_SHOULDER, g_poseOpen[CH_SHOULDER]));
  g_target[CH_ELBOW1] = fromClosed(CH_ELBOW1, spanFromClosed(CH_ELBOW1, g_poseOpen[CH_ELBOW1]));
  g_target[CH_WRIST1] = fromClosed(CH_WRIST1, spanFromClosed(CH_WRIST1, g_poseOpen[CH_WRIST1]));
  g_target[CH_ELBOW2] = fromClosed(CH_ELBOW2, spanFromClosed(CH_ELBOW2, g_poseHug[CH_ELBOW2]));
  g_target[CH_WRIST2] = fromClosed(CH_WRIST2, spanFromClosed(CH_WRIST2, g_poseHug[CH_WRIST2]));
}

static void applyCurrentStep() {
  if (g_path == PATH_OPEN || (g_path == PATH_OPEN_THEN_HUG && g_step == 0) ||
      (g_path == PATH_UNHUG_THEN_OPEN && g_step == 1)) {
    applyPoseOpen();
  } else if (g_path == PATH_CLOSE || (g_path == PATH_UNHUG_THEN_CLOSE && g_step == 1)) {
    applyPoseClosed();
  } else if (g_path == PATH_HUG || (g_path == PATH_OPEN_THEN_HUG && g_step == 1)) {
    applyPoseHug();
  } else if ((g_path == PATH_UNHUG_THEN_CLOSE || g_path == PATH_UNHUG_THEN_OPEN) &&
             g_step == 0) {
    applyParkHug();
    // Freeze raise/expand so D does not finish an in-progress open first
    g_target[CH_SHOULDER] = g_actual[CH_SHOULDER];
    g_target[CH_ELBOW1] = g_actual[CH_ELBOW1];
    g_target[CH_WRIST1] = g_actual[CH_WRIST1];
  }
}

static void startPath(PathId path, SpeedTier s1, SpeedTier s2) {
  abortPath();
  g_path = path;
  g_step = 0;
  g_speed = s1;
  g_speed2 = s2;
  applyCurrentStep();
}

static void beginFlap();

static void finishPath() {
  if (g_path == PATH_OPEN || g_path == PATH_UNHUG_THEN_OPEN) g_pose = POSE_OPEN;
  else if (g_path == PATH_HUG || g_path == PATH_OPEN_THEN_HUG) g_pose = POSE_HUG;
  else if (g_path == PATH_CLOSE || g_path == PATH_UNHUG_THEN_CLOSE) {
    g_pose = POSE_CLOSED;
  }
  bool keepFlap = g_flapPending;
  abortPath();
  g_flapPending = keepFlap;
}

static void servicePath() {
  if (g_path == PATH_NONE) return;
  // Do not use AllArrived here. After startPath, targets often already
  // match lastcmd (A-close at home) and a loop tick would finish
  // the path before buildSeq/kickGroup runs.
  if (!g_runMotion) return;
  if (g_nStages == 0) return;
  if (g_group >= 0 && g_group < (int)g_nStages) return;

  bool twoStage = (g_path == PATH_OPEN_THEN_HUG ||
                   g_path == PATH_UNHUG_THEN_CLOSE ||
                   g_path == PATH_UNHUG_THEN_OPEN);
  if (twoStage && g_step == 0) {
    g_step = 1;
    g_speed = g_speed2;
    applyCurrentStep();
    g_group = 0;
    g_groupAttachIdx = 0;
    g_groupHoldUntil = 0;
    buildSeq();
    kickGroup();
    return;
  }
  finishPath();
  g_runMotion = false;
  g_forceDwell = false;
  g_armed = (sdAttachMask() != 0);
  Serial.println(F("PATH done — pulses off"));
  if (g_flapPending && g_pose == POSE_OPEN) {
    g_flapPending = false;
    beginFlap();
  }
}

// dest and first PWM = taught CLOSED only. lastcmd / hug / open never.
static bool pulseArmCh(uint8_t ch) {
  if (ch >= NUM_SERVOS) return false;
  g_jogMask &= (uint8_t)~(1u << ch);
  if (!attachArmClosed(ch)) return false;
  g_armed = true;
  bumpSetupIdle(ch);
  Serial.printf("ARM %s closed=%d (pad after duty check)\n",
                SERVO_NAMES[ch], (int)g_target[ch]);
  return true;
}

void servosArmCh(uint8_t ch) {
  if (ch >= NUM_SERVOS) return;
  abortPath();
  pulseArmCh(ch);
}

// Serial bench only: SETUP pulse each job, in order. Not BLE / not fob.
void servosArm() {
  abortPath();
  g_armSeqOn = true;
  g_armSeqI = 0;
  Serial.println(F("ARM seq SH ER EH WR WH"));
  if (!pulseArmCh(ARM_SEQ[0])) {
    g_armSeqOn = false;
    Serial.println(F("ARM seq abort — attach refused"));
  }
}

static void serviceArmSeq() {
  if (!g_armSeqOn) return;
  if (g_runMotion || g_flapOn) {
    g_armSeqOn = false;
    return;
  }
  uint8_t ch = ARM_SEQ[g_armSeqI];
  if (chAttached(ch) || (g_jogMask & (1u << ch))) return;
  g_armSeqI++;
  if (g_armSeqI >= NUM_SERVOS) {
    g_armSeqOn = false;
    g_pose = POSE_CLOSED;
    Serial.println(F("ARM seq done"));
    return;
  }
  if (!pulseArmCh(ARM_SEQ[g_armSeqI])) {
    g_armSeqOn = false;
    Serial.println(F("ARM seq abort — attach refused"));
  }
}

void servosDisarm() {
  abortPath();
  detachAll();
  // Derived, not asserted: if the driver refused a release the pad is still live
  // and claiming DISARMED would be a lie about a servo that is still driven.
  g_armed = (sdAttachMask() != 0);
  Serial.println(g_armed ? F("DISARM INCOMPLETE — a pad is still live")
                         : F("DISARMED"));
}

// STOP is not "hold PWM". Abort, keep where the servo actually is, detach so
// the ANNIMOS brake holds. Do not change g_pose (may be mid-path).
void servosStop() {
  abortPath();
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    // Halt at the emitted position, not the commanded one: mid-move the ramp is
    // ahead of the driver, and stopping at the commanded value would ask for one
    // last move rather than stopping.
    if (sdAttached(i)) g_actual[i] = sdWritten(i);
    g_target[i] = g_actual[i];
  }
  detachAll();
  g_armed = (sdAttachMask() != 0);
  Serial.println(g_armed ? F("STOP INCOMPLETE — a pad is still live")
                         : F("STOP — last actual, detached (brake)"));
}

void servosDisarmCh(uint8_t ch) {
  if (ch >= NUM_SERVOS) return;
  abortPath();
  detachOne(ch);
  Serial.printf("DISARMED ch%u %s\n", (unsigned)ch, SERVO_NAMES[ch]);
}

void servosHome(bool forceArm) {
  (void)forceArm;
  startRunMotion(PATH_UNHUG_THEN_CLOSE, SPD_QUARTER, SPD_QUARTER, true);
  Serial.println(F("D staged unhug then close"));
}

bool servosAllArrived() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    if (abs(g_actual[i] - g_target[i]) > ARRIVE_US) return false;
  }
  return true;
}

static bool requireBrakeReady(const char* what) {
  if (g_brakeReady) return true;
  Serial.printf("COLD — %s needs ARM/HOME first\n", what);
  return false;
}

void cmdToggleWing() {
  if (!requireBrakeReady("A")) return;
  if (g_pose == POSE_CLOSED) {
    startRunMotion(PATH_OPEN, SPD_FULL, SPD_FULL, false);
    Serial.println(F("A OPEN WR+ER+SH together"));
  } else if (g_pose == POSE_OPEN) {
    startRunMotion(PATH_CLOSE, SPD_FULL, SPD_FULL, false);
    Serial.println(F("A CLOSE WR+ER+SH together"));
  } else {
    startRunMotion(PATH_UNHUG_THEN_CLOSE, SPD_HALF, SPD_FULL, false);
    Serial.println(F("A unhug then CLOSE staged"));
  }
}

void cmdHug() {
  if (!requireBrakeReady("B")) return;
  if (g_pose == POSE_CLOSED) {
    startRunMotion(PATH_OPEN_THEN_HUG, SPD_FULL, SPD_HALF, false);
    Serial.println(F("B OPEN then HUG staged"));
  } else if (g_pose == POSE_OPEN) {
    startRunMotion(PATH_HUG, SPD_HALF, SPD_HALF, false);
    Serial.println(F("B HUG staged"));
  } else {
    startRunMotion(PATH_UNHUG_THEN_OPEN, SPD_HALF, SPD_FULL, false);
    Serial.println(F("B unhug to OPEN staged"));
  }
}

CmdId servosResolvePairCmd(CmdId cmd) {
  if (cmd == CMD_TOGGLE_WING) {
    if (g_pose == POSE_CLOSED) return CMD_POSE_OPEN;
    return CMD_POSE_FOLDED;
  }
  if (cmd == CMD_TOGGLE_WRIST) {
    if (g_pose == POSE_HUG) return CMD_POSE_OPEN;
    return CMD_POSE_HUG;
  }
  return cmd;
}

static int destUsForPose(uint8_t ch, WingPose pose) {
  if (ch >= NUM_SERVOS) return SERVO_CENTER;
  if (pose == POSE_HUG) {
    if (isHugCh(ch)) return fromClosed(ch, spanFromClosed(ch, g_poseHug[ch]));
    return fromClosed(ch, spanFromClosed(ch, g_poseOpen[ch]));
  }
  if (pose == POSE_OPEN) {
    if (isHugCh(ch)) return homeUs(ch);
    return fromClosed(ch, spanFromClosed(ch, g_poseOpen[ch]));
  }
  return homeUs(ch);
}

static bool actualAtPose(WingPose pose) {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    if (abs(g_actual[i] - destUsForPose(i, pose)) > ARRIVE_US) return false;
  }
  return true;
}

static bool hugsParked() {
  return abs(g_actual[CH_ELBOW2] - destUsForPose(CH_ELBOW2, POSE_CLOSED)) <= ARRIVE_US &&
         abs(g_actual[CH_WRIST2] - destUsForPose(CH_WRIST2, POSE_CLOSED)) <= ARRIVE_US;
}

static bool raisesAtOpen() {
  return abs(g_actual[CH_SHOULDER] - destUsForPose(CH_SHOULDER, POSE_OPEN)) <= ARRIVE_US &&
         abs(g_actual[CH_ELBOW1] - destUsForPose(CH_ELBOW1, POSE_OPEN)) <= ARRIVE_US &&
         abs(g_actual[CH_WRIST1] - destUsForPose(CH_WRIST1, POSE_OPEN)) <= ARRIVE_US;
}

static void cmdPoseOpen() {
  if (!requireBrakeReady("OPEN")) return;
  if (actualAtPose(POSE_OPEN)) {
    g_pose = POSE_OPEN;
    Serial.println(F("OPEN already there"));
    return;
  }
  if (!hugsParked()) {
    startRunMotion(PATH_UNHUG_THEN_OPEN, SPD_HALF, SPD_FULL, false);
    Serial.println(F("OPEN unhug then open"));
  } else {
    startRunMotion(PATH_OPEN, SPD_FULL, SPD_FULL, false);
    Serial.println(F("OPEN WR+ER+SH"));
  }
}

static void cmdPoseClosed() {
  if (!requireBrakeReady("CLOSE")) return;
  if (actualAtPose(POSE_CLOSED)) {
    g_pose = POSE_CLOSED;
    Serial.println(F("CLOSE already there"));
    return;
  }
  if (!hugsParked()) {
    startRunMotion(PATH_UNHUG_THEN_CLOSE, SPD_HALF, SPD_FULL, false);
    Serial.println(F("CLOSE unhug then close"));
  } else {
    startRunMotion(PATH_CLOSE, SPD_FULL, SPD_FULL, false);
    Serial.println(F("CLOSE WR+ER+SH"));
  }
}

static int flapDeltaUs() {
  return (int)((uint32_t)SERVO_SPEC_SPAN_US * (uint32_t)SEQ_FLAP_DEG /
               (uint32_t)SERVO_SPEC_TRAVEL_DEG);
}

static int flapCenterUs(uint8_t ch) {
  return clampToSoft(ch, g_cal[ch].center);
}

// Hug pose is forward. If hug == center, treat +µs as forward.
static int flapFwdSign(uint8_t ch) {
  int c = flapCenterUs(ch);
  return ((int)g_poseHug[ch] >= c) ? 1 : -1;
}

static int flapDestUs(uint8_t ch, uint8_t step) {
  int c = flapCenterUs(ch);
  int d = flapDeltaUs();
  int s = flapFwdSign(ch);
  if (step == 0) return clampToSoft(ch, c + s * d);
  return clampToSoft(ch, c - s * d);
}

static int flapStepDest(uint8_t ch) {
  if (g_flapPreamble || g_flapHome) return flapCenterUs(ch);
  return flapDestUs(ch, g_flapStep);
}

static void kickFlapStep() {
  g_speed = SPD_HALF;
  if (!chAttached(CH_ELBOW2)) attachOne(CH_ELBOW2);
  if (!chAttached(CH_WRIST2)) attachOne(CH_WRIST2);
  g_target[CH_ELBOW2] = flapStepDest(CH_ELBOW2);
  g_target[CH_WRIST2] = g_actual[CH_WRIST2];
  if (chNeedsMove(CH_ELBOW2)) startJogEase(CH_ELBOW2);
  g_flapWristGo = false;
  g_flapWristAt = millis() + (unsigned long)SEQ_FLAP_WRIST_LAG_MS;
  g_flapHoldUntil = 0;
}

static void finishFlap() {
  g_flapOn = false;
  g_flapPreamble = false;
  g_flapHome = false;
  detachOne(CH_ELBOW2);
  detachOne(CH_WRIST2);
  g_armed = (sdAttachMask() != 0);
  g_pose = POSE_OPEN;
  Serial.println(F("FLAP done — hugs at center, brake"));
}

static void beginFlap() {
  abortPath();
  g_flapOn = true;
  g_flapPending = false;
  g_flapPreamble = true;
  g_flapHome = false;
  g_flapCycle = 0;
  g_flapStep = 0;
  g_speed = SPD_HALF;
  g_pose = POSE_OPEN;
  g_armed = true;
  Serial.printf("FLAP ±%ddeg (%d us) thru-center %u strokes wrist lag %u ms\n",
                SEQ_FLAP_DEG, flapDeltaUs(),
                (unsigned)SEQ_FLAP_CYCLES,
                (unsigned)SEQ_FLAP_WRIST_LAG_MS);
  kickFlapStep();
}

static void serviceFlap() {
  if (!g_flapOn) return;
  unsigned long now = millis();
  if (!g_flapWristGo && now >= g_flapWristAt) {
    g_flapWristGo = true;
    g_target[CH_WRIST2] = flapStepDest(CH_WRIST2);
    if (!chAttached(CH_WRIST2)) attachOne(CH_WRIST2);
    if (chNeedsMove(CH_WRIST2)) startJogEase(CH_WRIST2);
  }
  if (!g_flapWristGo) return;
  if ((g_jogMask & ((1u << CH_ELBOW2) | (1u << CH_WRIST2))) != 0) return;
  if (chNeedsMove(CH_ELBOW2) || chNeedsMove(CH_WRIST2)) return;
  if (SEQ_HOLD_MS > 0) {
    if (g_flapHoldUntil == 0) {
      g_flapHoldUntil = now + (unsigned long)SEQ_HOLD_MS;
      return;
    }
    if (now < g_flapHoldUntil) return;
  }

  if (g_flapPreamble) {
    g_flapPreamble = false;
    g_flapCycle = 0;
    g_flapStep = 0;
    kickFlapStep();
    return;
  }
  if (g_flapHome) {
    finishFlap();
    return;
  }

  if (g_flapStep == 0) {
    if (g_flapCycle >= SEQ_FLAP_CYCLES) {
      g_flapHome = true;
      kickFlapStep();
      return;
    }
    g_flapStep = 1;
    kickFlapStep();
    return;
  }

  g_flapCycle++;
  if (g_flapCycle >= SEQ_FLAP_CYCLES) {
    g_flapHome = true;
    kickFlapStep();
    return;
  }
  g_flapStep = 0;
  kickFlapStep();
}

void startSequence() {
  if (!requireBrakeReady("FLAP")) return;
  if (g_pose == POSE_OPEN && raisesAtOpen()) {
    beginFlap();
    return;
  }
  if (!hugsParked()) {
    startRunMotion(PATH_UNHUG_THEN_OPEN, SPD_HALF, SPD_FULL, false);
    Serial.println(F("FLAP — unhug then open first"));
  } else {
    startRunMotion(PATH_OPEN, SPD_FULL, SPD_FULL, false);
    Serial.println(F("FLAP — open first"));
  }
  g_flapPending = true;
}

static void cmdPoseHug() {
  if (!requireBrakeReady("HUG")) return;
  if (actualAtPose(POSE_HUG)) {
    g_pose = POSE_HUG;
    Serial.println(F("HUG already there"));
    return;
  }
  if (!raisesAtOpen()) {
    startRunMotion(PATH_OPEN_THEN_HUG, SPD_FULL, SPD_HALF, false);
    Serial.println(F("HUG open then hug"));
  } else {
    startRunMotion(PATH_HUG, SPD_HALF, SPD_HALF, false);
    Serial.println(F("HUG wrists+elbow hug"));
  }
}

static void serviceJogEase() {
  if (g_runMotion || g_jogMask == 0) return;
  unsigned long now = millis();
  for (uint8_t ch = 0; ch < NUM_SERVOS; ch++) {
    if (!(g_jogMask & (1u << ch)) || !chAttached(ch)) continue;
    uint32_t elapsed = (uint32_t)(now - g_jogT0[ch]);
    float s = cosineFrac(elapsed, g_jogDur[ch]);
    bool done = elapsed >= g_jogDur[ch];
    int from = g_jogFrom[ch];
    int to = g_target[ch];
    int span = to - from;
    int next = (done || span == 0) ? to : from + (int)lroundf((float)span * s);
    next = clampToSoft(ch, next);
    if (next != g_actual[ch]) {
      g_actual[ch] = next;
      commandUs(ch, g_actual[ch]);
    }
    if (done) {
      g_jogMask &= (uint8_t)~(1u << ch);
      bumpSetupIdle(ch);
    }
  }
}

static void serviceSetupIdle() {
  if (g_runMotion || g_flapOn) return;
  unsigned long now = millis();
  for (uint8_t ch = 0; ch < NUM_SERVOS; ch++) {
    if (!chAttached(ch)) continue;
    if (g_jogMask & (1u << ch)) continue;
    if (chNeedsMove(ch)) continue;
    if (g_setupIdleAt[ch] == 0) {
      g_setupIdleAt[ch] = now + SETUP_IDLE_DETACH_MS;
      continue;
    }
    if (now >= g_setupIdleAt[ch]) {
      Serial.printf("IDLE detach %s\n", SERVO_NAMES[ch]);
      detachOne(ch);
    }
  }
}

/*
  Deferred so a flash write never lands inside the motion path. Only runs once
  everything is genuinely idle and no pad is live, which is also the only moment
  g_actual is stable enough to be worth persisting.
*/
static void serviceLastCmdSave() {
  if (!g_lastCmdDirty) return;
  if (g_runMotion || g_rampOn || g_flapOn || g_armSeqOn) return;
  if (g_jogMask != 0 || sdAttachMask() != 0) return;
  g_lastCmdDirty = false;
  storeSaveLastCmd();
}

void servosService() {
  // Emits pending pulses on the frame cadence and supervises the phase
  // reference. Runs FIRST and unconditionally: supervision has to sample far
  // faster than the 20 ms frame it is watching, and loop() calls this every
  // iteration with no delay.
  sdService();

  // A stalled service loop is what turns a smooth ramp into a single large step,
  // so the gap is recorded rather than inferred. An attach still blocks for the
  // gap and latch waits, so a gap that follows one is this firmware waiting on
  // purpose rather than the symptom being hunted. Logged separately.
  static uint32_t lastServiceMs = 0;
  const uint32_t nowMs = millis();
  if (lastServiceMs != 0) {
    const uint32_t gap = nowMs - lastServiceMs;
    if (gap > 30) {
      traceAdd(g_attachSinceService ? TR_BLOCK : TR_STALL, TR_NO_CH, 0,
               (int)g_attachSinceService, (int)gap);
    }
  }
  lastServiceMs = nowMs;
  g_attachSinceService = 0;

  serviceGroup();
  serviceCosineRamp();
  servicePath();
  serviceJogEase();
  serviceFlap();
  serviceSetupIdle();
  serviceArmSeq();
  serviceLastCmdSave();
}

void servosHandleCmd(CmdId cmd, const int16_t* payload, uint8_t payloadLen) {
  switch (cmd) {
    case CMD_ARM:
      if (payload && payloadLen >= 1 && payload[0] >= 0 && payload[0] < NUM_SERVOS) {
        Serial.printf("CMD ARM ch=%u\n", (unsigned)payload[0]);
        servosArmCh((uint8_t)payload[0]);
      } else {
        Serial.println(F("CMD ARM ignore — need ch 0-4 (serial `arm` is bench seq)"));
      }
      break;
    case CMD_DISARM:
      if (payload && payloadLen >= 1 && payload[0] >= 0 && payload[0] < NUM_SERVOS)
        servosDisarmCh((uint8_t)payload[0]);
      else
        servosDisarm();
      break;
    case CMD_HOME:         servosHome(true); break;
    case CMD_TOGGLE_WING:  cmdToggleWing(); break;
    case CMD_TOGGLE_WRIST: cmdHug(); break;
    case CMD_SEQ:          startSequence(); break;
    case CMD_POSE_FOLDED:  cmdPoseClosed(); break;
    case CMD_POSE_OPEN:    cmdPoseOpen(); break;
    case CMD_POSE_HUG:     cmdPoseHug(); break;
    case CMD_STOP:         servosStop(); break;
    case CMD_SET_TARGETS:
      if (payload && payloadLen >= NUM_SERVOS) servosSetTargets(payload);
      break;
    case CMD_JOG:
      if (payload && payloadLen >= 2)
        servosJog((uint8_t)payload[0], (int)payload[1]);
      break;
    case CMD_TEACH_POSE:
      if (payload && payloadLen >= 2)
        servosTeachPose((uint8_t)payload[0], (WingPose)payload[1]);
      break;
    case CMD_SET_SENSE:
      if (payload && payloadLen >= 2)
        servosSetSense((uint8_t)payload[0], payload[1] != 0);
      break;
    case CMD_SET_SPEED:
      if (payload && payloadLen >= 1)
        servosSetSpeedPct((uint8_t)payload[0]);
      break;
    case CMD_SET_CH_SPEED:
      if (payload && payloadLen >= 2)
        servosSetChSpeedPct((uint8_t)payload[0], (uint8_t)payload[1]);
      break;
    default:
      break;
  }
}
