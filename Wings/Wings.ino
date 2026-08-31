/*
  Wings — Run controller + dual setup pages
  Mega 2560 + 3.5" TFT + 4 servos + 4-ch momentary RF remote

  Modes:
    RUN         — A/B/C/D remote (and on-screen test buttons)
    SETUP MAIN  — S1/S2 calibrate (same workflow as ServoTest)
    SETUP TIPS  — S3/S4 calibrate

  Remote (last-commanded state, not measured position):
    A — toggle main open/closed
    B — toggle tips (only when main is open)
    C — coordinated light-flap sequence
    D — emergency home (interrupts all motion)
*/

#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <TouchScreen.h>
#include <Servo.h>
#include <EEPROM.h>
#include "Config.h"

MCUFRIEND_kbv tft;
TouchScreen ts = TouchScreen(XP, YP, XM, YM, TS_OHMS);

Servo servos[4];
const uint8_t SERVO_PINS[4] = { SERVO1_PIN, SERVO2_PIN, SERVO3_PIN, SERVO4_PIN };

// ---------- Palette ----------
#define C_BG       0x0000
#define C_TRACK    0x4A49
#define C_WHITE    0xFFFF
#define C_BLACK    0x0000
#define C_BTN      0xC618
#define C_BTN_SEL  0xFFE0
#define C_ARM_OFF  0xB000
#define C_ARM_ON   0x1320
#define C_RED      0xF800
#define C_YELLOW   0xFFE0
#define C_CYAN     0x07FF
#define C_GREEN    0x07E0
#define C_MARK_H   0x8410
#define C_MARK_S   0xFFFF
#define C_DIM      0xBDF7
#define C_ORANGE   0xFD20

// ---------- Enums ----------
enum AppMode : uint8_t { MODE_RUN = 0, MODE_SETUP_MAIN = 1, MODE_SETUP_TIPS = 2 };
enum LinkMode : uint8_t { LINK_IND = 0, LINK_MIRROR = 1, LINK_COPY = 2 };
enum ActiveTarget : uint8_t { ACT_A = 0, ACT_B = 1, ACT_BOTH = 2 };
enum LimitTier : uint8_t { TIER_SOFT = 0, TIER_HARD = 1 };
enum WingState : uint8_t { WING_CLOSED = 0, WING_OPEN = 1 };
enum TipState  : uint8_t { TIP_A = 0, TIP_B = 1 }; // softMin <-> softMax
enum SeqPhase  : uint8_t {
  SEQ_IDLE = 0,
  SEQ_OPEN_MAIN,
  SEQ_HOLD_OPEN,
  SEQ_TIPS_B,
  SEQ_HOLD_B,
  SEQ_TIPS_A,
  SEQ_HOLD_A,
  SEQ_TIPS_HOME,
  SEQ_DONE
};

struct Persist {
  uint16_t magic;
  int16_t  softMin[4], softMax[4];
  int16_t  hardMin[4], hardMax[4];
  int16_t  center[4];
  int16_t  pos[4];
  uint8_t  linkMain, linkTips;
  uint8_t  travelDeg;
  uint8_t  tierMain, tierTips;
  uint8_t  wingState, tipState;
};

struct Btn {
  int16_t x, y, w, h;
  const char* label;
  uint16_t color;
};

// ---------- Cal / motion state ----------
bool armed = false;
AppMode appMode = MODE_RUN;

int hardMin[4], hardMax[4];
int softMin[4], softMax[4];
int center[4];
int target[4];
int actual[4];

LinkMode linkMain = LINK_MIRROR;
LinkMode linkTips = LINK_MIRROR;
ActiveTarget active = ACT_BOTH;
LimitTier tierMain = TIER_SOFT;
LimitTier tierTips = TIER_SOFT;
uint8_t travelDeg = SERVO_TRAVEL_DEG;

WingState wingState = WING_CLOSED;
TipState tipState = TIP_A;

SeqPhase seqPhase = SEQ_IDLE;
uint8_t seqCycle = 0;
unsigned long seqWaitUntil = 0;
bool seqActive = false;

unsigned long lastRampMs = 0;
unsigned long lastBtnMs = 0;
unsigned long lastValueMs = 0;
unsigned long statusUntilMs = 0;
const char* statusMsg = nullptr;
uint16_t statusColor = C_WHITE;

bool valuesDirty = true;
bool slidersDirty = true;
bool uiReady = false;
bool dragging = false;
bool eepromValid = false;
bool runDirty = true;

int16_t handleY1 = -1;
int16_t handleY2 = -1;
int lastDrawnUs1 = -1;
int lastDrawnUs2 = -1;

// Remote edge detect
bool remPrev[4] = { false, false, false, false };
unsigned long remChangeMs[4] = { 0, 0, 0, 0 };
bool remStable[4] = { false, false, false, false };

const char* lastRemoteLabel = "-";

// ---------- Layout ----------
const int SLIDER_W = 78;
const int SLIDER_H = 200;
const int SLIDER1_X = 22;
const int SLIDER2_X = 220;
const int SLIDER_Y = 92;
const int HANDLE_H = 28;

const int Y_TOP = 8;
const int Y_MODE = 48;
const int Y_VAL = 300;
const int Y_NUDGE = 352;
const int Y_LIM = 396;
const int Y_UTIL = 440;

// Setup buttons
Btn btnSoft = {110, Y_TOP, 42, 34, "SF", C_BTN};
Btn btnHard = {154, Y_TOP, 42, 34, "HD", C_BTN};
Btn btnArm  = {200, Y_TOP, 112, 34, "ARM", C_ARM_OFF};

Btn btnInd  = {8,   Y_MODE, 58, 34, "IND", C_BTN};
Btn btnMir  = {70,  Y_MODE, 58, 34, "MIR", C_BTN};
Btn btnCpy  = {132, Y_MODE, 58, 34, "CPY", C_BTN};
Btn btnAct1 = {204, Y_MODE, 36, 34, "1", C_BTN};
Btn btnAct2 = {244, Y_MODE, 36, 34, "2", C_BTN};
Btn btnActB = {284, Y_MODE, 28, 34, "B", C_BTN};

Btn btnN50m = {8,   Y_NUDGE, 74, 38, "-50", C_BTN};
Btn btnN10m = {86,  Y_NUDGE, 74, 38, "-10", C_BTN};
Btn btnN10p = {164, Y_NUDGE, 74, 38, "+10", C_BTN};
Btn btnN50p = {242, Y_NUDGE, 70, 38, "+50", C_BTN};

Btn btnSetMin = {8,   Y_LIM, 100, 34, "MIN", C_BTN};
Btn btnSetCtr = {114, Y_LIM, 92,  34, "SETC", C_BTN};
Btn btnSetMax = {212, Y_LIM, 100, 34, "MAX", C_BTN};

Btn btnGoCtr = {8,   Y_UTIL, 60, 34, "GOC", C_BTN};
Btn btnSweep = {72,  Y_UTIL, 60, 34, "SWP", C_BTN};
Btn btnSave  = {136, Y_UTIL, 60, 34, "SAVE", C_BTN};
Btn btnLoad  = {200, Y_UTIL, 54, 34, "LOAD", C_BTN};
Btn btnBack  = {258, Y_UTIL, 54, 34, "RUN", C_BTN};

// Run buttons
Btn btnRunA = {16,  300, 140, 48, "A WING", C_BTN};
Btn btnRunB = {164, 300, 140, 48, "B TIPS", C_BTN};
Btn btnRunC = {16,  358, 140, 48, "C FLAP", C_BTN};
Btn btnRunD = {164, 358, 140, 48, "D HOME", C_ORANGE};
Btn btnToMain = {16,  420, 140, 44, "SETUP MAIN", C_BTN};
Btn btnToTips = {164, 420, 140, 44, "SETUP TIPS", C_BTN};

// ---------- Prototypes ----------
void drawApp();
void drawRunUI();
void drawSetupUI();
void updateSliders(bool force);
void updateValues(bool force);
void updateRunUI(bool force);
void setStatus(const char* msg, uint16_t color, uint16_t ms = 900);
void clampSoftInsideHard(uint8_t i0, uint8_t i1);
void gotoHome(bool fromRemote);
void startSequence();
void abortSequence();
void serviceSequence();
void applyRemoteEdge(uint8_t ch);
bool allArrived(uint8_t i0, uint8_t i1);
bool allArrived4();

// ---------- Bank helpers ----------
uint8_t bankBase() { return (appMode == MODE_SETUP_TIPS) ? 2 : 0; }
LinkMode& bankLink() { return (appMode == MODE_SETUP_TIPS) ? linkTips : linkMain; }
LimitTier& bankTier() { return (appMode == MODE_SETUP_TIPS) ? tierTips : tierMain; }

int tmin(uint8_t i) {
  LimitTier t = (i < 2) ? tierMain : tierTips;
  return (t == TIER_HARD) ? hardMin[i] : softMin[i];
}
int tmax(uint8_t i) {
  LimitTier t = (i < 2) ? tierMain : tierTips;
  return (t == TIER_HARD) ? hardMax[i] : softMax[i];
}

void clampSoftInsideHard(uint8_t i0, uint8_t i1) {
  for (uint8_t i = i0; i <= i1; i++) {
    softMin[i] = constrain(softMin[i], hardMin[i], hardMax[i] - 20);
    softMax[i] = constrain(softMax[i], softMin[i] + 20, hardMax[i]);
    center[i] = constrain(center[i], softMin[i], softMax[i]);
  }
}

void applyLink(uint8_t i0, uint8_t i1, bool fromFirst) {
  LinkMode lm = (i0 < 2) ? linkMain : linkTips;
  if (lm == LINK_IND) return;
  uint8_t src = fromFirst ? i0 : i1;
  uint8_t dst = fromFirst ? i1 : i0;
  int d = target[src] - center[src];
  if (lm == LINK_COPY)
    target[dst] = constrain(center[dst] + d, tmin(dst), tmax(dst));
  else
    target[dst] = constrain(center[dst] - d, tmin(dst), tmax(dst));
}

void setTarget(uint8_t i, int us) {
  target[i] = constrain(us, tmin(i), tmax(i));
  uint8_t i0 = (i < 2) ? 0 : 2;
  applyLink(i0, i0 + 1, i == i0);
  valuesDirty = true;
  slidersDirty = true;
  runDirty = true;
}

void setPairTargets(uint8_t i0, int us0, int us1) {
  target[i0] = constrain(us0, tmin(i0), tmax(i0));
  target[i0 + 1] = constrain(us1, tmin(i0 + 1), tmax(i0 + 1));
  valuesDirty = true;
  slidersDirty = true;
  runDirty = true;
}

void driveImmediatePair(uint8_t i0) {
  for (uint8_t i = i0; i < i0 + 2; i++) {
    actual[i] = target[i];
    if (armed) servos[i].writeMicroseconds(actual[i]);
  }
}

void writeAllImmediate() {
  for (uint8_t i = 0; i < 4; i++) {
    actual[i] = target[i];
    if (armed) servos[i].writeMicroseconds(actual[i]);
  }
  slidersDirty = true;
  valuesDirty = true;
  runDirty = true;
}

void rampTowardTargets() {
  if (!armed || dragging) return;
  unsigned long now = millis();
  if (now - lastRampMs < RAMP_INTERVAL_MS) return;
  lastRampMs = now;

  bool moved = false;
  for (uint8_t i = 0; i < 4; i++) {
    if (actual[i] != target[i]) {
      actual[i] += constrain(target[i] - actual[i], -RAMP_STEP_US, RAMP_STEP_US);
      servos[i].writeMicroseconds(actual[i]);
      moved = true;
    }
  }
  if (moved) {
    slidersDirty = true;
    valuesDirty = true;
    runDirty = true;
  }
}

bool arrived(uint8_t i) {
  return abs(actual[i] - target[i]) <= ARRIVE_US;
}

bool allArrived(uint8_t i0, uint8_t i1) {
  for (uint8_t i = i0; i <= i1; i++) if (!arrived(i)) return false;
  return true;
}

bool allArrived4() { return allArrived(0, 3); }

// ---------- Named poses (from soft limits / centers) ----------
void poseMainClosed() {
  // Soft mode envelope is the working range for run commands
  LimitTier saved = tierMain;
  tierMain = TIER_SOFT;
  setPairTargets(0, softMin[0], softMin[1]);
  tierMain = saved;
  wingState = WING_CLOSED;
}

void poseMainOpen() {
  LimitTier saved = tierMain;
  tierMain = TIER_SOFT;
  setPairTargets(0, softMax[0], softMax[1]);
  tierMain = saved;
  wingState = WING_OPEN;
}

void poseTipsA() {
  LimitTier saved = tierTips;
  tierTips = TIER_SOFT;
  setPairTargets(2, softMin[2], softMin[3]);
  tierTips = saved;
  tipState = TIP_A;
}

void poseTipsB() {
  LimitTier saved = tierTips;
  tierTips = TIER_SOFT;
  setPairTargets(2, softMax[2], softMax[3]);
  tierTips = saved;
  tipState = TIP_B;
}

void poseTipsHome() {
  LimitTier saved = tierTips;
  tierTips = TIER_SOFT;
  setPairTargets(2, center[2], center[3]);
  tierTips = saved;
  tipState = TIP_A;
}

void gotoHome(bool fromRemote) {
  abortSequence();
  // D is a software e-close: always allow motion even if UI-disarmed
  if (!armed) {
    armed = true;
    if (appMode != MODE_RUN) drawArmBtn();
  }
  poseTipsHome();
  poseMainClosed();
  if (fromRemote) lastRemoteLabel = "D";
  setStatus("HOME", C_ORANGE, 1200);
  runDirty = true;
}

// ---------- Sequence ----------
void abortSequence() {
  seqActive = false;
  seqPhase = SEQ_IDLE;
  seqCycle = 0;
  seqWaitUntil = 0;
}

void startSequence() {
  abortSequence();
  seqActive = true;
  seqPhase = SEQ_OPEN_MAIN;
  seqCycle = 0;
  lastRemoteLabel = "C";
  setStatus("SEQ START", C_CYAN, 800);
  runDirty = true;
}

void serviceSequence() {
  if (!seqActive || !armed) return;
  unsigned long now = millis();

  switch (seqPhase) {
    case SEQ_OPEN_MAIN:
      poseMainOpen();
      poseTipsHome();
      seqPhase = SEQ_HOLD_OPEN;
      break;

    case SEQ_HOLD_OPEN:
      if (allArrived(0, 3)) {
        seqWaitUntil = now + SEQ_HOLD_MS;
        seqPhase = SEQ_TIPS_B;
      }
      break;

    case SEQ_TIPS_B:
      if (now < seqWaitUntil) break;
      poseTipsB();
#if SEQ_MAIN_BREATH_US > 0
      {
        LimitTier saved = tierMain;
        tierMain = TIER_SOFT;
        int b0 = constrain(softMax[0] - SEQ_MAIN_BREATH_US, softMin[0], softMax[0]);
        int b1 = constrain(softMax[1] - SEQ_MAIN_BREATH_US, softMin[1], softMax[1]);
        setPairTargets(0, b0, b1);
        tierMain = saved;
        wingState = WING_OPEN;
      }
#endif
      seqPhase = SEQ_HOLD_B;
      break;

    case SEQ_HOLD_B:
      if (allArrived(0, 3)) {
        seqWaitUntil = now + SEQ_HOLD_MS;
        seqPhase = SEQ_TIPS_A;
      }
      break;

    case SEQ_TIPS_A:
      if (now < seqWaitUntil) break;
      poseTipsA();
#if SEQ_MAIN_BREATH_US > 0
      poseMainOpen();
#endif
      seqPhase = SEQ_HOLD_A;
      break;

    case SEQ_HOLD_A:
      if (allArrived(0, 3)) {
        seqCycle++;
        if (seqCycle >= SEQ_FLAP_CYCLES) {
          seqWaitUntil = now + SEQ_HOLD_MS;
          seqPhase = SEQ_TIPS_HOME;
        } else {
          seqWaitUntil = now + SEQ_HOLD_MS;
          seqPhase = SEQ_TIPS_B;
        }
      }
      break;

    case SEQ_TIPS_HOME:
      if (now < seqWaitUntil) break;
      poseTipsHome();
      poseMainOpen();
      seqPhase = SEQ_DONE;
      break;

    case SEQ_DONE:
      if (allArrived(0, 3)) {
        abortSequence();
        setStatus("SEQ DONE", C_YELLOW, 1000);
      }
      break;

    default:
      abortSequence();
      break;
  }
}

// ---------- Commands A/B ----------
void cmdToggleWing() {
  if (seqActive) return; // D only interrupts; A ignored mid-seq
  lastRemoteLabel = "A";
  if (wingState == WING_CLOSED) {
    poseMainOpen();
    setStatus("OPEN", C_CYAN);
  } else {
    poseTipsHome();
    poseMainClosed();
    setStatus("CLOSE", C_CYAN);
  }
  runDirty = true;
}

void cmdToggleTips() {
  if (seqActive) return;
  lastRemoteLabel = "B";
  if (wingState != WING_OPEN) {
    setStatus("OPEN FIRST", C_RED);
    runDirty = true;
    return;
  }
  if (tipState == TIP_A) {
    poseTipsB();
    setStatus("TIPS B", C_CYAN);
  } else {
    poseTipsA();
    setStatus("TIPS A", C_CYAN);
  }
  runDirty = true;
}

void applyRemoteEdge(uint8_t ch) {
  // ch: 0=A 1=B 2=C 3=D
  if (ch == 3) { // D highest priority
    gotoHome(true);
    return;
  }
  if (appMode != MODE_RUN) return;
  if (!armed) {
    setStatus("DISARMED", C_RED);
    return;
  }
  if (ch == 0) cmdToggleWing();
  else if (ch == 1) cmdToggleTips();
  else if (ch == 2) startSequence();
}

void pollRemote() {
  const uint8_t pins[4] = { REMOTE_A_PIN, REMOTE_B_PIN, REMOTE_C_PIN, REMOTE_D_PIN };
  unsigned long now = millis();

  for (uint8_t i = 0; i < 4; i++) {
    bool raw = digitalRead(pins[i]);
#if REMOTE_ACTIVE_LOW
    bool pressed = !raw;
#else
    bool pressed = raw;
#endif
    if (pressed != remPrev[i]) {
      remPrev[i] = pressed;
      remChangeMs[i] = now;
    }
    if ((now - remChangeMs[i]) >= REMOTE_DEBOUNCE_MS) {
      if (pressed != remStable[i]) {
        remStable[i] = pressed;
        if (pressed) applyRemoteEdge(i); // rising press edge
      }
    }
  }
}

// ---------- EEPROM ----------
void defaultsCal() {
  for (uint8_t i = 0; i < 4; i++) {
    hardMin[i] = DEFAULT_HARD_MIN;
    hardMax[i] = DEFAULT_HARD_MAX;
    softMin[i] = DEFAULT_SOFT_MIN;
    softMax[i] = DEFAULT_SOFT_MAX;
    center[i] = SERVO_CENTER;
    target[i] = SERVO_CENTER;
    actual[i] = SERVO_CENTER;
  }
  linkMain = LINK_MIRROR;
  linkTips = LINK_MIRROR;
  tierMain = TIER_SOFT;
  tierTips = TIER_SOFT;
  wingState = WING_CLOSED;
  tipState = TIP_A;
}

void saveEEPROM() {
  Persist p;
  p.magic = EEPROM_MAGIC;
  for (uint8_t i = 0; i < 4; i++) {
    p.softMin[i] = softMin[i];
    p.softMax[i] = softMax[i];
    p.hardMin[i] = hardMin[i];
    p.hardMax[i] = hardMax[i];
    p.center[i] = center[i];
    p.pos[i] = target[i];
  }
  p.linkMain = (uint8_t)linkMain;
  p.linkTips = (uint8_t)linkTips;
  p.travelDeg = travelDeg;
  p.tierMain = (uint8_t)tierMain;
  p.tierTips = (uint8_t)tierTips;
  p.wingState = (uint8_t)wingState;
  p.tipState = (uint8_t)tipState;
  EEPROM.put(EEPROM_ADDR, p);
  eepromValid = true;
  setStatus("SAVED", C_YELLOW);
  Serial.println(F("EEPROM saved"));
}

bool loadEEPROM(bool quiet) {
  Persist p;
  EEPROM.get(EEPROM_ADDR, p);
  if (p.magic != EEPROM_MAGIC) {
    eepromValid = false;
    if (!quiet) setStatus("EMPTY - HIT SAVE", C_YELLOW);
    Serial.println(F("EEPROM empty/invalid"));
    return false;
  }
  eepromValid = true;
  for (uint8_t i = 0; i < 4; i++) {
    hardMin[i] = constrain(p.hardMin[i], SERVO_ABS_MIN, SERVO_ABS_MAX);
    hardMax[i] = constrain(p.hardMax[i], SERVO_ABS_MIN, SERVO_ABS_MAX);
    if (hardMin[i] >= hardMax[i]) {
      hardMin[i] = DEFAULT_HARD_MIN;
      hardMax[i] = DEFAULT_HARD_MAX;
    }
    softMin[i] = constrain(p.softMin[i], SERVO_ABS_MIN, SERVO_ABS_MAX);
    softMax[i] = constrain(p.softMax[i], SERVO_ABS_MIN, SERVO_ABS_MAX);
  }
  clampSoftInsideHard(0, 1);
  clampSoftInsideHard(2, 3);
  for (uint8_t i = 0; i < 4; i++) {
    center[i] = constrain(p.center[i], softMin[i], softMax[i]);
    target[i] = constrain(p.pos[i], softMin[i], softMax[i]);
  }
  linkMain = (LinkMode)constrain(p.linkMain, 0, 2);
  linkTips = (LinkMode)constrain(p.linkTips, 0, 2);
  tierMain = (LimitTier)constrain(p.tierMain, 0, 1);
  tierTips = (LimitTier)constrain(p.tierTips, 0, 1);
  if (p.travelDeg == 180 || p.travelDeg == 270) travelDeg = p.travelDeg;
  wingState = (WingState)constrain(p.wingState, 0, 1);
  tipState = (TipState)constrain(p.tipState, 0, 1);

  if (!quiet) setStatus("LOADED", C_YELLOW);
  Serial.println(F("EEPROM loaded"));
  return true;
}

// ---------- Drawing helpers ----------
bool hit(const Btn& b, int x, int y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

bool canPress() {
  if (millis() - lastBtnMs < TOUCH_DEBOUNCE_MS) return false;
  lastBtnMs = millis();
  return true;
}

void drawBtn(const Btn& b, bool selected) {
  uint16_t fill, text, border;
  if (b.color == C_ARM_OFF || b.color == C_ARM_ON || b.color == C_ORANGE) {
    fill = b.color; text = C_WHITE; border = C_WHITE;
  } else if (selected) {
    fill = C_BTN_SEL; text = C_BLACK; border = C_WHITE;
  } else {
    fill = b.color; text = C_BLACK; border = C_WHITE;
  }
  tft.fillRect(b.x, b.y, b.w, b.h, fill);
  tft.drawRect(b.x, b.y, b.w, b.h, border);
  tft.setTextSize(2);
  tft.setTextColor(text);
  int tw = (int)strlen(b.label) * 12;
  tft.setCursor(b.x + max(2, (b.w - tw) / 2), b.y + (b.h - 16) / 2);
  tft.print(b.label);
}

void drawArmBtn() {
  btnArm.label = armed ? "ARMED" : "ARM";
  btnArm.color = armed ? C_ARM_ON : C_ARM_OFF;
  drawBtn(btnArm, false);
}

void drawTierBtns() {
  drawBtn(btnSoft, bankTier() == TIER_SOFT);
  drawBtn(btnHard, bankTier() == TIER_HARD);
}

void drawLinkBtns() {
  LinkMode lm = bankLink();
  drawBtn(btnInd, lm == LINK_IND);
  drawBtn(btnMir, lm == LINK_MIRROR);
  drawBtn(btnCpy, lm == LINK_COPY);
}

void drawActiveBtns() {
  drawBtn(btnAct1, active == ACT_A);
  drawBtn(btnAct2, active == ACT_B);
  drawBtn(btnActB, active == ACT_BOTH);
}

int usToHandleY(int us) {
  int y = map(us, SERVO_ABS_MIN, SERVO_ABS_MAX,
              SLIDER_Y + SLIDER_H - HANDLE_H - 2, SLIDER_Y + 2);
  return constrain(y, SLIDER_Y + 2, SLIDER_Y + SLIDER_H - HANDLE_H - 2);
}

int usToMarkY(int us) {
  return map(us, SERVO_ABS_MIN, SERVO_ABS_MAX,
             SLIDER_Y + SLIDER_H - 3, SLIDER_Y + 2);
}

void drawLimitMarks(int sx, uint8_t idx) {
  int y;
  y = usToMarkY(hardMin[idx]); tft.drawFastHLine(sx + 4, y, SLIDER_W - 8, C_MARK_H); tft.drawFastHLine(sx + 4, y + 1, SLIDER_W - 8, C_MARK_H);
  y = usToMarkY(hardMax[idx]); tft.drawFastHLine(sx + 4, y, SLIDER_W - 8, C_MARK_H); tft.drawFastHLine(sx + 4, y + 1, SLIDER_W - 8, C_MARK_H);
  y = usToMarkY(softMin[idx]); tft.drawFastHLine(sx + 4, y, SLIDER_W - 8, C_MARK_S); tft.drawFastHLine(sx + 4, y + 1, SLIDER_W - 8, C_MARK_S);
  y = usToMarkY(softMax[idx]); tft.drawFastHLine(sx + 4, y, SLIDER_W - 8, C_MARK_S); tft.drawFastHLine(sx + 4, y + 1, SLIDER_W - 8, C_MARK_S);
  y = usToMarkY(center[idx]);
  tft.drawFastHLine(sx + 10, y, SLIDER_W - 20, C_YELLOW);
}

void eraseHandleBand(int sx, int y) {
  if (y < 0) return;
  tft.fillRect(sx + 3, y, SLIDER_W - 6, HANDLE_H, C_TRACK);
}

void paintHandle(int sx, int y, uint16_t color) {
  tft.fillRect(sx + 3, y, SLIDER_W - 6, HANDLE_H, color);
  tft.drawFastHLine(sx + 14, y + HANDLE_H / 2 - 4, SLIDER_W - 28, C_BLACK);
  tft.drawFastHLine(sx + 14, y + HANDLE_H / 2,     SLIDER_W - 28, C_BLACK);
  tft.drawFastHLine(sx + 14, y + HANDLE_H / 2 + 4, SLIDER_W - 28, C_BLACK);
}

void updateOneSlider(int sx, uint8_t idx, uint16_t color, int16_t& lastY) {
  int show = dragging ? target[idx] : actual[idx];
  int y = usToHandleY(show);
  if (y == lastY) return;
  eraseHandleBand(sx, lastY);
  drawLimitMarks(sx, idx);
  paintHandle(sx, y, color);
  lastY = y;
}

void updateSliders(bool force) {
  if (appMode == MODE_RUN) return;
  if (!force && !slidersDirty) return;
  uint8_t i0 = bankBase();

  if (force || handleY1 < 0 || handleY2 < 0) {
    tft.fillRect(SLIDER1_X + 2, SLIDER_Y + 2, SLIDER_W - 4, SLIDER_H - 4, C_TRACK);
    tft.fillRect(SLIDER2_X + 2, SLIDER_Y + 2, SLIDER_W - 4, SLIDER_H - 4, C_TRACK);
    drawLimitMarks(SLIDER1_X, i0);
    drawLimitMarks(SLIDER2_X, i0 + 1);
    handleY1 = handleY2 = -1;
  }

  updateOneSlider(SLIDER1_X, i0, C_CYAN, handleY1);
  updateOneSlider(SLIDER2_X, i0 + 1, C_YELLOW, handleY2);
  slidersDirty = false;
}

void printFixedUs(int x, int y, int us, uint16_t fg) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%4d", us);
  tft.setTextSize(3);
  tft.setTextColor(fg, C_BG);
  tft.setCursor(x, y);
  tft.print(buf);
}

void updateValues(bool force) {
  if (appMode == MODE_RUN) return;
  unsigned long now = millis();
  if (!force && !valuesDirty) return;
  if (!force && (now - lastValueMs < VALUE_REFRESH_MS)) return;
  lastValueMs = now;

  uint8_t i0 = bankBase();
  int show1 = dragging ? target[i0] : actual[i0];
  int show2 = dragging ? target[i0 + 1] : actual[i0 + 1];

  if (force || show1 != lastDrawnUs1) {
    printFixedUs(28, Y_VAL, show1, C_CYAN);
    lastDrawnUs1 = show1;
  }
  if (force || show2 != lastDrawnUs2) {
    printFixedUs(226, Y_VAL, show2, C_YELLOW);
    lastDrawnUs2 = show2;
  }

  LimitTier tier = bankTier();
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  char line[28];
  if (tier == TIER_SOFT)
    snprintf(line, sizeof(line), "SOFT %4d-%4d C%4d", softMin[i0], softMax[i0], center[i0]);
  else
    snprintf(line, sizeof(line), "HARD %4d-%4d C%4d", hardMin[i0], hardMax[i0], center[i0]);
  tft.setCursor(8, Y_VAL + 28);
  tft.print(line);

  if (tier == TIER_SOFT)
    snprintf(line, sizeof(line), "SOFT %4d-%4d C%4d", softMin[i0 + 1], softMax[i0 + 1], center[i0 + 1]);
  else
    snprintf(line, sizeof(line), "HARD %4d-%4d C%4d", hardMin[i0 + 1], hardMax[i0 + 1], center[i0 + 1]);
  tft.setCursor(168, Y_VAL + 28);
  tft.print(line);

  tft.setTextSize(2);
  tft.setTextColor(C_BG, C_BG);
  tft.setCursor(8, Y_VAL + 40);
  tft.print(F("                "));

  if (statusMsg && now < statusUntilMs) {
    tft.setTextColor(statusColor, C_BG);
    tft.setCursor(8, Y_VAL + 40);
    tft.print(statusMsg);
  } else {
    statusMsg = nullptr;
    tft.setTextColor(armed ? C_YELLOW : C_RED, C_BG);
    tft.setCursor(8, Y_VAL + 40);
    tft.print(armed ? F("ARMED") : F("DISARMED"));
  }

  valuesDirty = false;
}

void updateRunUI(bool force) {
  if (appMode != MODE_RUN) return;
  unsigned long now = millis();
  // Refresh when dirty, or while a timed status is showing / just expired
  if (!force && !runDirty && !(statusMsg)) return;
  if (!force && (now - lastValueMs < VALUE_REFRESH_MS)) return;
  lastValueMs = now;

  // Status block
  tft.fillRect(8, 56, 304, 220, C_BG);

  tft.setTextSize(2);
  tft.setTextColor(armed ? C_GREEN : C_RED, C_BG);
  tft.setCursor(16, 64);
  tft.print(armed ? F("ARMED") : F("DISARMED"));

  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(16, 96);
  tft.print(F("WINGS "));
  tft.setTextColor(wingState == WING_OPEN ? C_CYAN : C_DIM, C_BG);
  tft.print(wingState == WING_OPEN ? F("OPEN") : F("CLOSED"));

  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(16, 128);
  tft.print(F("TIPS  "));
  tft.setTextColor(C_YELLOW, C_BG);
  tft.print(tipState == TIP_A ? F("A / MIN") : F("B / MAX"));

  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(16, 160);
  tft.print(F("SEQ   "));
  tft.setTextColor(seqActive ? C_ORANGE : C_DIM, C_BG);
  tft.print(seqActive ? F("RUNNING") : F("IDLE"));

  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(16, 192);
  tft.print(F("REMOTE "));
  tft.setTextColor(C_YELLOW, C_BG);
  tft.print(lastRemoteLabel);

  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  char line[40];
  snprintf(line, sizeof(line), "M %4d %4d   T %4d %4d",
           actual[0], actual[1], actual[2], actual[3]);
  tft.setCursor(16, 228);
  tft.print(line);

  if (statusMsg && now < statusUntilMs) {
    tft.setTextSize(2);
    tft.setTextColor(statusColor, C_BG);
    tft.setCursor(16, 248);
    tft.print(statusMsg);
  } else {
    statusMsg = nullptr;
    tft.setTextSize(1);
    tft.setTextColor(C_DIM, C_BG);
    tft.setCursor(16, 252);
    tft.print(F("Pose: soft MIN=closed/A  MAX=open/B  CTR=tip home"));
  }

  runDirty = false;
}

void setStatus(const char* msg, uint16_t color, uint16_t ms) {
  if (!uiReady) return;
  statusMsg = msg;
  statusColor = color;
  statusUntilMs = millis() + ms;
  valuesDirty = true;
  runDirty = true;
  if (appMode == MODE_RUN) updateRunUI(true);
  else updateValues(true);
}

void drawSetupUI() {
  tft.fillScreen(C_BG);
  uint8_t i0 = bankBase();

  tft.setTextSize(2);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(8, 14);
  if (appMode == MODE_SETUP_MAIN) tft.print(F("MAIN"));
  else tft.print(F("TIPS"));

  tft.setTextSize(2);
  tft.setTextColor(C_CYAN, C_BG);
  tft.setCursor(SLIDER1_X, 72);
  tft.print(i0 == 0 ? F("S1") : F("S3"));
  tft.setTextColor(C_YELLOW, C_BG);
  tft.setCursor(SLIDER2_X, 72);
  tft.print(i0 == 0 ? F("S2") : F("S4"));

  tft.fillRect(SLIDER1_X, SLIDER_Y, SLIDER_W, SLIDER_H, C_TRACK);
  tft.fillRect(SLIDER2_X, SLIDER_Y, SLIDER_W, SLIDER_H, C_TRACK);
  tft.drawRect(SLIDER1_X, SLIDER_Y, SLIDER_W, SLIDER_H, C_WHITE);
  tft.drawRect(SLIDER2_X, SLIDER_Y, SLIDER_W, SLIDER_H, C_WHITE);

  drawArmBtn();
  drawTierBtns();
  drawLinkBtns();
  drawActiveBtns();

  drawBtn(btnN50m, false);
  drawBtn(btnN10m, false);
  drawBtn(btnN10p, false);
  drawBtn(btnN50p, false);
  drawBtn(btnSetMin, false);
  drawBtn(btnSetCtr, false);
  drawBtn(btnSetMax, false);
  drawBtn(btnGoCtr, false);
  drawBtn(btnSweep, false);
  drawBtn(btnSave, false);
  drawBtn(btnLoad, false);
  drawBtn(btnBack, false);

  handleY1 = handleY2 = -1;
  lastDrawnUs1 = lastDrawnUs2 = -1;
  slidersDirty = true;
  valuesDirty = true;
  updateSliders(true);
  updateValues(true);
}

void drawRunUI() {
  tft.fillScreen(C_BG);
  tft.setTextSize(3);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(8, 12);
  tft.print(F("WINGS"));

  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setCursor(140, 22);
  tft.print(F("RUN  A/B/C/D"));

  drawBtn(btnRunA, false);
  drawBtn(btnRunB, false);
  drawBtn(btnRunC, false);
  drawBtn(btnRunD, false);
  drawBtn(btnToMain, false);
  drawBtn(btnToTips, false);

  runDirty = true;
  updateRunUI(true);
}

void drawApp() {
  uiReady = false;
  if (appMode == MODE_RUN) drawRunUI();
  else drawSetupUI();
  uiReady = true;
}

void setAppMode(AppMode m) {
  appMode = m;
  dragging = false;
  active = ACT_BOTH;
  drawApp();
}

// ---------- Setup actions ----------
void toggleArm() {
  armed = !armed;
  if (armed) {
    writeAllImmediate();
    setStatus("ARMED", C_YELLOW);
  } else {
    abortSequence();
    setStatus("DISARMED", C_RED);
  }
  if (appMode != MODE_RUN) drawArmBtn();
  valuesDirty = true;
  runDirty = true;
}

void nudgeSmart(int delta) {
  if (!armed) return;
  uint8_t i0 = bankBase();
  if (active == ACT_A) setTarget(i0, target[i0] + delta);
  else if (active == ACT_B) setTarget(i0 + 1, target[i0 + 1] + delta);
  else if (bankLink() == LINK_IND) {
    setTarget(i0, target[i0] + delta);
    // setTarget may re-link; force independent
    target[i0 + 1] = constrain(target[i0 + 1] + delta, tmin(i0 + 1), tmax(i0 + 1));
  } else {
    setTarget(i0, target[i0] + delta);
  }
}

void doGoCenter() {
  if (!armed) { setStatus("ARM FIRST", C_RED); return; }
  uint8_t i0 = bankBase();
  LimitTier& tier = bankTier();
  LimitTier saved = tier;
  // Center is always within soft; allow move in current tier window
  setTarget(i0, constrain(center[i0], tmin(i0), tmax(i0)));
  if (bankLink() == LINK_IND)
    target[i0 + 1] = constrain(center[i0 + 1], tmin(i0 + 1), tmax(i0 + 1));
  (void)saved;
  setStatus("TO CENTER", C_YELLOW);
}

void doSetCenter() {
  if (!armed) { setStatus("ARM FIRST", C_RED); return; }
  uint8_t i0 = bankBase();
  if (active == ACT_A || active == ACT_BOTH)
    center[i0] = constrain(actual[i0], softMin[i0], softMax[i0]);
  if (active == ACT_B || active == ACT_BOTH)
    center[i0 + 1] = constrain(actual[i0 + 1], softMin[i0 + 1], softMax[i0 + 1]);
  handleY1 = handleY2 = -1;
  slidersDirty = true;
  valuesDirty = true;
  updateSliders(true);
  setStatus("CENTER SET", C_YELLOW);
}

void doSetMin() {
  if (!armed) { setStatus("ARM FIRST", C_RED); return; }
  uint8_t i0 = bankBase();
  LimitTier& tier = bankTier();

  if (tier == TIER_HARD) {
    if (active == ACT_A || active == ACT_BOTH)
      hardMin[i0] = constrain(actual[i0], SERVO_ABS_MIN, hardMax[i0] - 20);
    if (active == ACT_B || active == ACT_BOTH)
      hardMin[i0 + 1] = constrain(actual[i0 + 1], SERVO_ABS_MIN, hardMax[i0 + 1] - 20);
    clampSoftInsideHard(i0, i0 + 1);
    setStatus("HARD MIN", C_YELLOW);
  } else {
    if (active == ACT_A || active == ACT_BOTH)
      softMin[i0] = constrain(actual[i0], hardMin[i0], softMax[i0] - 20);
    if (active == ACT_B || active == ACT_BOTH)
      softMin[i0 + 1] = constrain(actual[i0 + 1], hardMin[i0 + 1], softMax[i0 + 1] - 20);
    center[i0] = constrain(center[i0], softMin[i0], softMax[i0]);
    center[i0 + 1] = constrain(center[i0 + 1], softMin[i0 + 1], softMax[i0 + 1]);
    setStatus("SOFT MIN", C_YELLOW);
  }
  target[i0] = constrain(target[i0], tmin(i0), tmax(i0));
  target[i0 + 1] = constrain(target[i0 + 1], tmin(i0 + 1), tmax(i0 + 1));
  handleY1 = handleY2 = -1;
  slidersDirty = true;
  valuesDirty = true;
  updateSliders(true);
}

void doSetMax() {
  if (!armed) { setStatus("ARM FIRST", C_RED); return; }
  uint8_t i0 = bankBase();
  LimitTier& tier = bankTier();

  if (tier == TIER_HARD) {
    if (active == ACT_A || active == ACT_BOTH)
      hardMax[i0] = constrain(actual[i0], hardMin[i0] + 20, SERVO_ABS_MAX);
    if (active == ACT_B || active == ACT_BOTH)
      hardMax[i0 + 1] = constrain(actual[i0 + 1], hardMin[i0 + 1] + 20, SERVO_ABS_MAX);
    clampSoftInsideHard(i0, i0 + 1);
    setStatus("HARD MAX", C_YELLOW);
  } else {
    if (active == ACT_A || active == ACT_BOTH)
      softMax[i0] = constrain(actual[i0], softMin[i0] + 20, hardMax[i0]);
    if (active == ACT_B || active == ACT_BOTH)
      softMax[i0 + 1] = constrain(actual[i0 + 1], softMin[i0 + 1] + 20, hardMax[i0 + 1]);
    center[i0] = constrain(center[i0], softMin[i0], softMax[i0]);
    center[i0 + 1] = constrain(center[i0 + 1], softMin[i0 + 1], softMax[i0 + 1]);
    setStatus("SOFT MAX", C_YELLOW);
  }
  target[i0] = constrain(target[i0], tmin(i0), tmax(i0));
  target[i0 + 1] = constrain(target[i0 + 1], tmin(i0 + 1), tmax(i0 + 1));
  handleY1 = handleY2 = -1;
  slidersDirty = true;
  valuesDirty = true;
  updateSliders(true);
}

void setLimitTier(LimitTier t) {
  bankTier() = t;
  uint8_t i0 = bankBase();
  target[i0] = constrain(target[i0], tmin(i0), tmax(i0));
  target[i0 + 1] = constrain(target[i0 + 1], tmin(i0 + 1), tmax(i0 + 1));
  drawTierBtns();
  valuesDirty = true;
  slidersDirty = true;
  setStatus(t == TIER_SOFT ? "SOFT MODE" : "HARD MODE", C_YELLOW);
}

void doSweep() {
  if (!armed) { setStatus("ARM FIRST", C_RED); return; }
  setStatus("SWEEP", C_YELLOW, 400);
  uint8_t i0 = bankBase();
  int lo0 = softMin[i0], hi0 = softMax[i0];
  int lo1 = softMin[i0 + 1], hi1 = softMax[i0 + 1];
  LinkMode lm = bankLink();

  for (int pass = 0; pass < 2; pass++) {
    const int steps = 40;
    for (int s = 0; s <= steps; s++) {
      int n = (pass == 0) ? s : (steps - s);
      target[i0] = lo0 + (int)(((long)(hi0 - lo0) * n) / steps);
      if (lm == LINK_COPY)
        target[i0 + 1] = constrain(center[i0 + 1] + (target[i0] - center[i0]), lo1, hi1);
      else if (lm == LINK_MIRROR)
        target[i0 + 1] = constrain(center[i0 + 1] - (target[i0] - center[i0]), lo1, hi1);
      else
        target[i0 + 1] = lo1 + (int)(((long)(hi1 - lo1) * n) / steps);

      driveImmediatePair(i0);
      updateSliders(false);
      updateValues(false);
      delay(16);

      TSPoint p = ts.getPoint();
      pinMode(XM, OUTPUT);
      pinMode(YP, OUTPUT);
      if (p.z > MINPRESSURE && p.z < MAXPRESSURE) {
        setStatus("ABORT", C_RED);
        return;
      }
      // D can still abort via remote during sweep
      pollRemote();
      if (!armed) return;
    }
  }
  setStatus("DONE", C_YELLOW);
}

// ---------- Touch ----------
void restoreTouchPins() {
  pinMode(XM, OUTPUT);
  pinMode(YP, OUTPUT);
}

bool mapTouch(const TSPoint& p, int& x, int& y) {
  int rx = p.x;
  int ry = p.y;
#if TOUCH_SWAP_XY
  int tmp = rx; rx = ry; ry = tmp;
#endif
#if TOUCH_INVERT_X
  x = map(rx, TS_MAXX, TS_MINX, 0, tft.width());
#else
  x = map(rx, TS_MINX, TS_MAXX, 0, tft.width());
#endif
#if TOUCH_INVERT_Y
  y = map(ry, TS_MAXY, TS_MINY, 0, tft.height());
#else
  y = map(ry, TS_MINY, TS_MAXY, 0, tft.height());
#endif
  x = constrain(x, 0, tft.width() - 1);
  y = constrain(y, 0, tft.height() - 1);
  return true;
}

bool inSlider(int x, int y, int sx) {
  return x > sx - 12 && x < sx + SLIDER_W + 12 &&
         y > SLIDER_Y - 4 && y < SLIDER_Y + SLIDER_H + 4;
}

bool handleSlider(int x, int y, int sx, bool first) {
  if (!inSlider(x, y, sx)) return false;
  dragging = true;
  if (!armed) {
    static unsigned long lastArmWarnMs = 0;
    if (millis() - lastArmWarnMs > 500) {
      lastArmWarnMs = millis();
      setStatus("ARM FIRST", C_RED, 700);
    }
    return true;
  }
  uint8_t i0 = bankBase();
  uint8_t idx = first ? i0 : (i0 + 1);
  int us = map(y, SLIDER_Y + SLIDER_H, SLIDER_Y, SERVO_ABS_MIN, SERVO_ABS_MAX);
  us = constrain(us, tmin(idx), tmax(idx));
  if (abs(us - target[idx]) > SLIDER_DEADBAND_US) {
    if (first) setTarget(i0, us);
    else {
      target[idx] = us;
      applyLink(i0, i0 + 1, false);
      valuesDirty = true;
      slidersDirty = true;
    }
    driveImmediatePair(i0);
    updateSliders(false);
  }
  return true;
}

void handleRunTouch(int x, int y) {
  if (!canPress()) return;
  if (hit(btnRunA, x, y)) { if (armed) cmdToggleWing(); else setStatus("DISARMED", C_RED); return; }
  if (hit(btnRunB, x, y)) { if (armed) cmdToggleTips(); else setStatus("DISARMED", C_RED); return; }
  if (hit(btnRunC, x, y)) { if (armed) startSequence(); else setStatus("DISARMED", C_RED); return; }
  if (hit(btnRunD, x, y)) { gotoHome(false); lastRemoteLabel = "D*"; return; }
  if (hit(btnToMain, x, y)) { setAppMode(MODE_SETUP_MAIN); return; }
  if (hit(btnToTips, x, y)) { setAppMode(MODE_SETUP_TIPS); return; }
}

void handleSetupTouch(int x, int y) {
  if (handleSlider(x, y, SLIDER1_X, true)) return;
  if (handleSlider(x, y, SLIDER2_X, false)) return;
  dragging = false;
  if (!canPress()) return;

  if (hit(btnArm, x, y)) { toggleArm(); return; }
  if (hit(btnSoft, x, y)) { setLimitTier(TIER_SOFT); return; }
  if (hit(btnHard, x, y)) { setLimitTier(TIER_HARD); return; }

  if (hit(btnInd, x, y)) { bankLink() = LINK_IND; drawLinkBtns(); return; }
  if (hit(btnMir, x, y)) {
    bankLink() = LINK_MIRROR;
    applyLink(bankBase(), bankBase() + 1, true);
    drawLinkBtns();
    if (armed) driveImmediatePair(bankBase());
    valuesDirty = true; slidersDirty = true;
    return;
  }
  if (hit(btnCpy, x, y)) {
    bankLink() = LINK_COPY;
    applyLink(bankBase(), bankBase() + 1, true);
    drawLinkBtns();
    if (armed) driveImmediatePair(bankBase());
    valuesDirty = true; slidersDirty = true;
    return;
  }

  if (hit(btnAct1, x, y)) { active = ACT_A; drawActiveBtns(); return; }
  if (hit(btnAct2, x, y)) { active = ACT_B; drawActiveBtns(); return; }
  if (hit(btnActB, x, y)) { active = ACT_BOTH; drawActiveBtns(); return; }

  if (hit(btnN50m, x, y)) { nudgeSmart(-NUDGE_COARSE_US); return; }
  if (hit(btnN10m, x, y)) { nudgeSmart(-NUDGE_FINE_US); return; }
  if (hit(btnN10p, x, y)) { nudgeSmart(+NUDGE_FINE_US); return; }
  if (hit(btnN50p, x, y)) { nudgeSmart(+NUDGE_COARSE_US); return; }

  if (hit(btnSetMin, x, y)) { doSetMin(); return; }
  if (hit(btnSetMax, x, y)) { doSetMax(); return; }
  if (hit(btnSetCtr, x, y)) { doSetCenter(); return; }
  if (hit(btnGoCtr, x, y)) { doGoCenter(); return; }
  if (hit(btnSave, x, y)) { saveEEPROM(); return; }
  if (hit(btnLoad, x, y)) {
    if (loadEEPROM(false)) {
      drawLinkBtns();
      drawTierBtns();
      handleY1 = handleY2 = -1;
      if (armed) writeAllImmediate();
      updateSliders(true);
      valuesDirty = true;
    }
    return;
  }
  if (hit(btnSweep, x, y)) { doSweep(); return; }
  if (hit(btnBack, x, y)) { setAppMode(MODE_RUN); return; }
}

void handleTouch(int x, int y) {
  if (appMode == MODE_RUN) handleRunTouch(x, y);
  else handleSetupTouch(x, y);
}

// ---------- Boot / loop ----------
void setup() {
  Serial.begin(115200);
  Serial.println(F("Wings controller starting"));

  defaultsCal();

  pinMode(REMOTE_A_PIN, INPUT_PULLUP);
  pinMode(REMOTE_B_PIN, INPUT_PULLUP);
  pinMode(REMOTE_C_PIN, INPUT_PULLUP);
  pinMode(REMOTE_D_PIN, INPUT_PULLUP);

  uint16_t id = tft.readID();
  if (id == 0xD3D3) id = 0x9486;
  Serial.print(F("TFT ID=0x"));
  Serial.println(id, HEX);
  tft.begin(id);
  tft.setRotation(0);

  loadEEPROM(true);

  for (uint8_t i = 0; i < 4; i++) {
    servos[i].attach(SERVO_PINS[i], SERVO_ABS_MIN, SERVO_ABS_MAX);
  }

  // Boot policy: command home, optionally auto-arm
  poseTipsHome();
  poseMainClosed();
  wingState = WING_CLOSED;
  tipState = TIP_A;

#if BOOT_AUTO_ARM
  armed = true;
  writeAllImmediate();
#else
  armed = false;
  for (uint8_t i = 0; i < 4; i++) actual[i] = target[i];
#endif

  appMode = MODE_RUN;
  drawApp();

  Serial.println(F("Ready — RUN mode. Remote momentary A/B/C/D. D=HOME."));
  Serial.println(F("Calibrate: SETUP MAIN / SETUP TIPS — soft MIN=closed/A MAX=open/B CTR=tip home"));
}

void loop() {
  pollRemote();
  serviceSequence();
  rampTowardTargets();

  TSPoint p = ts.getPoint();
  restoreTouchPins();

  if (p.z > MINPRESSURE && p.z < MAXPRESSURE) {
    int x, y;
    if (mapTouch(p, x, y)) handleTouch(x, y);
  } else {
    dragging = false;
  }

  if (appMode == MODE_RUN) {
    if (runDirty || statusMsg) updateRunUI(false);
  } else {
    if (slidersDirty) updateSliders(false);
    if (valuesDirty || statusMsg) updateValues(false);
  }
}
