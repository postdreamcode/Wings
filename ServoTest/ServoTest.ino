/*
  Wings — Manual Servo Test Bench
  Mega 2560 + 3.5" TFT shield + two ANNIMOS 150kg (DS51150-12V / RDS51150SG)

  Limits:
    HARD = mechanical stops (SF/HD toggle → HD, then MIN/MAX)
    SOFT = working envelope inside HARD (default travel / sweep)
    SETC = store center at current position
    GOC  = move to stored center
*/

#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <TouchScreen.h>
#include <Servo.h>
#include <EEPROM.h>
#include "Config.h"

MCUFRIEND_kbv tft;
TouchScreen ts = TouchScreen(XP, YP, XM, YM, TS_OHMS);

Servo servo1;
Servo servo2;

// High-contrast palette (RGB565)
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
#define C_MARK_H   0x8410   // hard-limit marks (dim)
#define C_MARK_S   0xFFFF   // soft-limit marks (bright)
#define C_DIM      0xBDF7

enum LinkMode : uint8_t { LINK_IND = 0, LINK_MIRROR = 1, LINK_COPY = 2 };
enum ActiveTarget : uint8_t { ACT_S1 = 0, ACT_S2 = 1, ACT_BOTH = 2 };
enum LimitTier : uint8_t { TIER_SOFT = 0, TIER_HARD = 1 };

struct Persist {
  uint16_t magic;
  int16_t  softMin1, softMax1, softMin2, softMax2;
  int16_t  hardMin1, hardMax1, hardMin2, hardMax2;
  int16_t  center1, center2;
  int16_t  pos1, pos2;
  uint8_t  link;
  uint8_t  travelDeg;
  uint8_t  tier;
};

struct Btn {
  int16_t x, y, w, h;
  const char* label;
  uint16_t color;
};

// ---------- State ----------
bool armed = false;
LinkMode linkMode = LINK_IND;
ActiveTarget active = ACT_BOTH;
LimitTier limitTier = TIER_SOFT;

int hardMin1 = DEFAULT_HARD_MIN, hardMax1 = DEFAULT_HARD_MAX;
int hardMin2 = DEFAULT_HARD_MIN, hardMax2 = DEFAULT_HARD_MAX;
int softMin1 = DEFAULT_SOFT_MIN, softMax1 = DEFAULT_SOFT_MAX;
int softMin2 = DEFAULT_SOFT_MIN, softMax2 = DEFAULT_SOFT_MAX;
int center1 = SERVO_CENTER;
int center2 = SERVO_CENTER;

int target1 = SERVO_CENTER;
int target2 = SERVO_CENTER;
int actual1 = SERVO_CENTER;
int actual2 = SERVO_CENTER;

uint8_t travelDeg = SERVO_TRAVEL_DEG;

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

int16_t handleY1 = -1;
int16_t handleY2 = -1;
int lastDrawnUs1 = -1;
int lastDrawnUs2 = -1;
int16_t touchX = -1, touchY = -1;
int16_t debugDotX = -1, debugDotY = -1;

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

Btn btnGoCtr = {8,   Y_UTIL, 74, 34, "GOC", C_BTN};
Btn btnSweep = {86,  Y_UTIL, 74, 34, "SWP", C_BTN};
Btn btnSave  = {164, Y_UTIL, 74, 34, "SAVE", C_BTN};
Btn btnLoad  = {242, Y_UTIL, 70, 34, "LOAD", C_BTN};

// ---------- Prototypes ----------
void updateSliders(bool force);
void updateValues(bool force);
void drawArmBtn();
void drawLinkBtns();
void drawActiveBtns();
void drawTierBtns();
void setStatus(const char* msg, uint16_t color, uint16_t ms = 900);
void clampSoftInsideHard();

// ---------- Travel helpers ----------
int travelMin1() { return (limitTier == TIER_HARD) ? hardMin1 : softMin1; }
int travelMax1() { return (limitTier == TIER_HARD) ? hardMax1 : softMax1; }
int travelMin2() { return (limitTier == TIER_HARD) ? hardMin2 : softMin2; }
int travelMax2() { return (limitTier == TIER_HARD) ? hardMax2 : softMax2; }

int usToDeg(int us) {
  long span = SERVO_ABS_MAX - SERVO_ABS_MIN;
  return (int)(((long)(us - SERVO_ABS_MIN) * travelDeg) / span);
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

bool hit(const Btn& b, int x, int y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

bool canPress() {
  if (millis() - lastBtnMs < TOUCH_DEBOUNCE_MS) return false;
  lastBtnMs = millis();
  return true;
}

void clampSoftInsideHard() {
  softMin1 = constrain(softMin1, hardMin1, hardMax1 - 20);
  softMax1 = constrain(softMax1, softMin1 + 20, hardMax1);
  softMin2 = constrain(softMin2, hardMin2, hardMax2 - 20);
  softMax2 = constrain(softMax2, softMin2 + 20, hardMax2);
  center1 = constrain(center1, softMin1, softMax1);
  center2 = constrain(center2, softMin2, softMax2);
}

void applyLinkFrom1() {
  int d = target1 - center1;
  if (linkMode == LINK_COPY)
    target2 = constrain(center2 + d, travelMin2(), travelMax2());
  else if (linkMode == LINK_MIRROR)
    target2 = constrain(center2 - d, travelMin2(), travelMax2());
}

void applyLinkFrom2() {
  int d = target2 - center2;
  if (linkMode == LINK_COPY)
    target1 = constrain(center1 + d, travelMin1(), travelMax1());
  else if (linkMode == LINK_MIRROR)
    target1 = constrain(center1 - d, travelMin1(), travelMax1());
}

void setTarget1(int us) {
  target1 = constrain(us, travelMin1(), travelMax1());
  if (linkMode != LINK_IND) applyLinkFrom1();
  valuesDirty = true;
  slidersDirty = true;
}

void setTarget2(int us) {
  target2 = constrain(us, travelMin2(), travelMax2());
  if (linkMode != LINK_IND) applyLinkFrom2();
  valuesDirty = true;
  slidersDirty = true;
}

void driveImmediate() {
  actual1 = target1;
  actual2 = target2;
  if (!armed) return;
  servo1.writeMicroseconds(actual1);
  servo2.writeMicroseconds(actual2);
}

void nudgeSmart(int delta) {
  if (!armed) return;
  if (active == ACT_S1) setTarget1(target1 + delta);
  else if (active == ACT_S2) setTarget2(target2 + delta);
  else if (linkMode == LINK_IND) {
    setTarget1(target1 + delta);
    setTarget2(target2 + delta);
  } else {
    setTarget1(target1 + delta);
  }
}

void writeServosImmediate() {
  driveImmediate();
  slidersDirty = true;
  valuesDirty = true;
}

void rampTowardTargets() {
  if (!armed || dragging) return;
  unsigned long now = millis();
  if (now - lastRampMs < RAMP_INTERVAL_MS) return;
  lastRampMs = now;

  bool moved = false;
  if (actual1 != target1) {
    actual1 += constrain(target1 - actual1, -RAMP_STEP_US, RAMP_STEP_US);
    servo1.writeMicroseconds(actual1);
    moved = true;
  }
  if (actual2 != target2) {
    actual2 += constrain(target2 - actual2, -RAMP_STEP_US, RAMP_STEP_US);
    servo2.writeMicroseconds(actual2);
    moved = true;
  }
  if (moved) {
    slidersDirty = true;
    valuesDirty = true;
  }
}

// ---------- EEPROM ----------
void saveEEPROM() {
  Persist p;
  p.magic = EEPROM_MAGIC;
  p.softMin1 = softMin1; p.softMax1 = softMax1;
  p.softMin2 = softMin2; p.softMax2 = softMax2;
  p.hardMin1 = hardMin1; p.hardMax1 = hardMax1;
  p.hardMin2 = hardMin2; p.hardMax2 = hardMax2;
  p.center1 = center1;   p.center2 = center2;
  p.pos1 = target1;      p.pos2 = target2;
  p.link = (uint8_t)linkMode;
  p.travelDeg = travelDeg;
  p.tier = (uint8_t)limitTier;
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

  hardMin1 = constrain(p.hardMin1, SERVO_ABS_MIN, SERVO_ABS_MAX);
  hardMax1 = constrain(p.hardMax1, SERVO_ABS_MIN, SERVO_ABS_MAX);
  hardMin2 = constrain(p.hardMin2, SERVO_ABS_MIN, SERVO_ABS_MAX);
  hardMax2 = constrain(p.hardMax2, SERVO_ABS_MIN, SERVO_ABS_MAX);
  if (hardMin1 >= hardMax1) { hardMin1 = DEFAULT_HARD_MIN; hardMax1 = DEFAULT_HARD_MAX; }
  if (hardMin2 >= hardMax2) { hardMin2 = DEFAULT_HARD_MIN; hardMax2 = DEFAULT_HARD_MAX; }

  softMin1 = constrain(p.softMin1, SERVO_ABS_MIN, SERVO_ABS_MAX);
  softMax1 = constrain(p.softMax1, SERVO_ABS_MIN, SERVO_ABS_MAX);
  softMin2 = constrain(p.softMin2, SERVO_ABS_MIN, SERVO_ABS_MAX);
  softMax2 = constrain(p.softMax2, SERVO_ABS_MIN, SERVO_ABS_MAX);
  clampSoftInsideHard();

  center1 = constrain(p.center1, softMin1, softMax1);
  center2 = constrain(p.center2, softMin2, softMax2);

  limitTier = (LimitTier)constrain(p.tier, 0, 1);
  target1 = constrain(p.pos1, travelMin1(), travelMax1());
  target2 = constrain(p.pos2, travelMin2(), travelMax2());
  linkMode = (LinkMode)constrain(p.link, 0, 2);
  if (p.travelDeg == 180 || p.travelDeg == 270) travelDeg = p.travelDeg;

  if (!quiet) setStatus("LOADED", C_YELLOW);
  Serial.println(F("EEPROM loaded"));
  return true;
}

// ---------- Drawing ----------
void drawBtn(const Btn& b, bool selected) {
  uint16_t fill, text, border;
  if (b.color == C_ARM_OFF || b.color == C_ARM_ON) {
    fill = b.color; text = C_WHITE; border = C_WHITE;
  } else if (selected) {
    fill = C_BTN_SEL; text = C_BLACK; border = C_WHITE;
  } else {
    fill = C_BTN; text = C_BLACK; border = C_WHITE;
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
  drawBtn(btnSoft, limitTier == TIER_SOFT);
  drawBtn(btnHard, limitTier == TIER_HARD);
}

void drawLinkBtns() {
  drawBtn(btnInd, linkMode == LINK_IND);
  drawBtn(btnMir, linkMode == LINK_MIRROR);
  drawBtn(btnCpy, linkMode == LINK_COPY);
}

void drawActiveBtns() {
  drawBtn(btnAct1, active == ACT_S1);
  drawBtn(btnAct2, active == ACT_S2);
  drawBtn(btnActB, active == ACT_BOTH);
}

void drawLimitMarks(int sx, int hmin, int hmax, int smin, int smax) {
  int y;
  y = usToMarkY(hmin); tft.drawFastHLine(sx + 4, y, SLIDER_W - 8, C_MARK_H); tft.drawFastHLine(sx + 4, y + 1, SLIDER_W - 8, C_MARK_H);
  y = usToMarkY(hmax); tft.drawFastHLine(sx + 4, y, SLIDER_W - 8, C_MARK_H); tft.drawFastHLine(sx + 4, y + 1, SLIDER_W - 8, C_MARK_H);
  y = usToMarkY(smin); tft.drawFastHLine(sx + 4, y, SLIDER_W - 8, C_MARK_S); tft.drawFastHLine(sx + 4, y + 1, SLIDER_W - 8, C_MARK_S);
  y = usToMarkY(smax); tft.drawFastHLine(sx + 4, y, SLIDER_W - 8, C_MARK_S); tft.drawFastHLine(sx + 4, y + 1, SLIDER_W - 8, C_MARK_S);
  // center tick
  y = usToMarkY(sx == SLIDER1_X ? center1 : center2);
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

void updateOneSlider(int sx, int us, int hmin, int hmax, int smin, int smax, uint16_t color, int16_t& lastY) {
  int y = usToHandleY(us);
  if (y == lastY) return;
  eraseHandleBand(sx, lastY);
  drawLimitMarks(sx, hmin, hmax, smin, smax);
  paintHandle(sx, y, color);
  lastY = y;
}

void updateSliders(bool force) {
  if (!force && !slidersDirty) return;

  if (force || handleY1 < 0 || handleY2 < 0) {
    tft.fillRect(SLIDER1_X + 2, SLIDER_Y + 2, SLIDER_W - 4, SLIDER_H - 4, C_TRACK);
    tft.fillRect(SLIDER2_X + 2, SLIDER_Y + 2, SLIDER_W - 4, SLIDER_H - 4, C_TRACK);
    drawLimitMarks(SLIDER1_X, hardMin1, hardMax1, softMin1, softMax1);
    drawLimitMarks(SLIDER2_X, hardMin2, hardMax2, softMin2, softMax2);
    handleY1 = handleY2 = -1;
  }

  int show1 = dragging ? target1 : actual1;
  int show2 = dragging ? target2 : actual2;
  updateOneSlider(SLIDER1_X, show1, hardMin1, hardMax1, softMin1, softMax1, C_CYAN, handleY1);
  updateOneSlider(SLIDER2_X, show2, hardMin2, hardMax2, softMin2, softMax2, C_YELLOW, handleY2);
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
  unsigned long now = millis();
  if (!force && !valuesDirty) return;
  if (!force && (now - lastValueMs < VALUE_REFRESH_MS)) return;
  lastValueMs = now;

  int show1 = dragging ? target1 : actual1;
  int show2 = dragging ? target2 : actual2;

  if (force || show1 != lastDrawnUs1) {
    printFixedUs(28, Y_VAL, show1, C_CYAN);
    lastDrawnUs1 = show1;
  }
  if (force || show2 != lastDrawnUs2) {
    printFixedUs(226, Y_VAL, show2, C_YELLOW);
    lastDrawnUs2 = show2;
  }

  // Tier + limits + center
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  char line[28];
  if (limitTier == TIER_SOFT) {
    snprintf(line, sizeof(line), "SOFT %4d-%4d C%4d", softMin1, softMax1, center1);
  } else {
    snprintf(line, sizeof(line), "HARD %4d-%4d C%4d", hardMin1, hardMax1, center1);
  }
  tft.setCursor(8, Y_VAL + 28);
  tft.print(line);

  if (limitTier == TIER_SOFT) {
    snprintf(line, sizeof(line), "SOFT %4d-%4d C%4d", softMin2, softMax2, center2);
  } else {
    snprintf(line, sizeof(line), "HARD %4d-%4d C%4d", hardMin2, hardMax2, center2);
  }
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

void setStatus(const char* msg, uint16_t color, uint16_t ms) {
  if (!uiReady) return;
  statusMsg = msg;
  statusColor = color;
  statusUntilMs = millis() + ms;
  valuesDirty = true;
  updateValues(true);
}

void drawStaticUI() {
  tft.fillScreen(C_BG);

  tft.setTextSize(3);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(8, 12);
  tft.print(F("WINGS"));

  tft.setTextSize(2);
  tft.setTextColor(C_CYAN, C_BG);
  tft.setCursor(SLIDER1_X, 72);
  tft.print(F("S1"));
  tft.setTextColor(C_YELLOW, C_BG);
  tft.setCursor(SLIDER2_X, 72);
  tft.print(F("S2"));

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
}

// ---------- Actions ----------
void doGoCenter() {
  if (!armed) { setStatus("ARM FIRST", C_RED); return; }
  setTarget1(constrain(center1, travelMin1(), travelMax1()));
  if (linkMode == LINK_IND)
    setTarget2(constrain(center2, travelMin2(), travelMax2()));
  setStatus("TO CENTER", C_YELLOW);
}

void doSetCenter() {
  if (!armed) { setStatus("ARM FIRST", C_RED); return; }
  if (active == ACT_S1 || active == ACT_BOTH)
    center1 = constrain(actual1, softMin1, softMax1);
  if (active == ACT_S2 || active == ACT_BOTH)
    center2 = constrain(actual2, softMin2, softMax2);
  handleY1 = handleY2 = -1;
  slidersDirty = true;
  valuesDirty = true;
  updateSliders(true);
  setStatus("CENTER SET", C_YELLOW);
  Serial.print(F("center1=")); Serial.print(center1);
  Serial.print(F(" center2=")); Serial.println(center2);
}

void doSetMin() {
  if (!armed) { setStatus("ARM FIRST", C_RED); return; }

  if (limitTier == TIER_HARD) {
    if (active == ACT_S1 || active == ACT_BOTH)
      hardMin1 = constrain(actual1, SERVO_ABS_MIN, hardMax1 - 20);
    if (active == ACT_S2 || active == ACT_BOTH)
      hardMin2 = constrain(actual2, SERVO_ABS_MIN, hardMax2 - 20);
    clampSoftInsideHard();
    setStatus("HARD MIN", C_YELLOW);
  } else {
    if (active == ACT_S1 || active == ACT_BOTH)
      softMin1 = constrain(actual1, hardMin1, softMax1 - 20);
    if (active == ACT_S2 || active == ACT_BOTH)
      softMin2 = constrain(actual2, hardMin2, softMax2 - 20);
    center1 = constrain(center1, softMin1, softMax1);
    center2 = constrain(center2, softMin2, softMax2);
    setStatus("SOFT MIN", C_YELLOW);
  }

  target1 = constrain(target1, travelMin1(), travelMax1());
  target2 = constrain(target2, travelMin2(), travelMax2());
  handleY1 = handleY2 = -1;
  slidersDirty = true;
  valuesDirty = true;
  updateSliders(true);
}

void doSetMax() {
  if (!armed) { setStatus("ARM FIRST", C_RED); return; }

  if (limitTier == TIER_HARD) {
    if (active == ACT_S1 || active == ACT_BOTH)
      hardMax1 = constrain(actual1, hardMin1 + 20, SERVO_ABS_MAX);
    if (active == ACT_S2 || active == ACT_BOTH)
      hardMax2 = constrain(actual2, hardMin2 + 20, SERVO_ABS_MAX);
    clampSoftInsideHard();
    setStatus("HARD MAX", C_YELLOW);
  } else {
    if (active == ACT_S1 || active == ACT_BOTH)
      softMax1 = constrain(actual1, softMin1 + 20, hardMax1);
    if (active == ACT_S2 || active == ACT_BOTH)
      softMax2 = constrain(actual2, softMin2 + 20, hardMax2);
    center1 = constrain(center1, softMin1, softMax1);
    center2 = constrain(center2, softMin2, softMax2);
    setStatus("SOFT MAX", C_YELLOW);
  }

  target1 = constrain(target1, travelMin1(), travelMax1());
  target2 = constrain(target2, travelMin2(), travelMax2());
  handleY1 = handleY2 = -1;
  slidersDirty = true;
  valuesDirty = true;
  updateSliders(true);
}

void setLimitTier(LimitTier t) {
  limitTier = t;
  // Keep command legal for the new travel window
  target1 = constrain(target1, travelMin1(), travelMax1());
  target2 = constrain(target2, travelMin2(), travelMax2());
  drawTierBtns();
  valuesDirty = true;
  slidersDirty = true;
  setStatus(t == TIER_SOFT ? "SOFT MODE" : "HARD MODE", C_YELLOW);
}

void doSweep() {
  if (!armed) { setStatus("ARM FIRST", C_RED); return; }
  setStatus("SWEEP", C_YELLOW, 400);

  // Sweep always uses SOFT limits (safe working range)
  int lo1 = softMin1, hi1 = softMax1;
  int lo2 = softMin2, hi2 = softMax2;

  for (int pass = 0; pass < 2; pass++) {
    const int steps = 40;
    for (int i = 0; i <= steps; i++) {
      int n = (pass == 0) ? i : (steps - i);
      target1 = lo1 + (int)(((long)(hi1 - lo1) * n) / steps);
      if (linkMode == LINK_COPY)
        target2 = constrain(center2 + (target1 - center1), lo2, hi2);
      else if (linkMode == LINK_MIRROR)
        target2 = constrain(center2 - (target1 - center1), lo2, hi2);
      else
        target2 = lo2 + (int)(((long)(hi2 - lo2) * n) / steps);

      writeServosImmediate();
      updateSliders(false);
      updateValues(false);
      delay(16);

      TSPoint p = ts.getPoint();
      restoreTouchPins();
      if (p.z > MINPRESSURE && p.z < MAXPRESSURE) {
        setStatus("ABORT", C_RED);
        return;
      }
    }
  }
  setStatus("DONE", C_YELLOW);
}

void toggleArm() {
  armed = !armed;
  if (armed) {
    actual1 = target1;
    actual2 = target2;
    servo1.writeMicroseconds(actual1);
    servo2.writeMicroseconds(actual2);
    setStatus("ARMED", C_YELLOW);
  } else {
    setStatus("DISARMED", C_RED);
  }
  drawArmBtn();
  valuesDirty = true;
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

#if TOUCH_DEBUG
void drawTouchDebug(int x, int y) {
  static unsigned long lastDbgMs = 0;
  if (millis() - lastDbgMs < 50) return;
  lastDbgMs = millis();
  char buf[18];
  snprintf(buf, sizeof(buf), "%3d,%3d", x, y);
  tft.setTextSize(2);
  tft.setTextColor(C_YELLOW, C_BG);
  tft.setCursor(110, 14);
  tft.print(buf);
  debugDotX = x;
  debugDotY = y;
}
#endif

void logHit(const char* name, int x, int y) {
  Serial.print(F("hit "));
  Serial.print(name);
  Serial.print(F(" @ "));
  Serial.print(x);
  Serial.print(',');
  Serial.println(y);
}

bool inSlider(int x, int y, int sx) {
  return x > sx - 12 && x < sx + SLIDER_W + 12 &&
         y > SLIDER_Y - 4 && y < SLIDER_Y + SLIDER_H + 4;
}

bool handleSlider(int x, int y, int sx, bool isServo1) {
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

  int us = map(y, SLIDER_Y + SLIDER_H, SLIDER_Y, SERVO_ABS_MIN, SERVO_ABS_MAX);
  if (isServo1) {
    us = constrain(us, travelMin1(), travelMax1());
    if (abs(us - target1) > SLIDER_DEADBAND_US) {
      setTarget1(us);
      driveImmediate();
      updateSliders(false);
    }
  } else {
    us = constrain(us, travelMin2(), travelMax2());
    if (abs(us - target2) > SLIDER_DEADBAND_US) {
      setTarget2(us);
      driveImmediate();
      updateSliders(false);
    }
  }
  return true;
}

void handleTouch(int x, int y) {
  touchX = x;
  touchY = y;
#if TOUCH_DEBUG
  drawTouchDebug(x, y);
#endif

  if (handleSlider(x, y, SLIDER1_X, true)) return;
  if (handleSlider(x, y, SLIDER2_X, false)) return;

  dragging = false;
  if (!canPress()) return;

  if (hit(btnArm, x, y)) { logHit("ARM", x, y); toggleArm(); return; }
  if (hit(btnSoft, x, y)) { logHit("SF", x, y); setLimitTier(TIER_SOFT); return; }
  if (hit(btnHard, x, y)) { logHit("HD", x, y); setLimitTier(TIER_HARD); return; }

  if (hit(btnInd, x, y)) { logHit("IND", x, y); linkMode = LINK_IND; drawLinkBtns(); return; }
  if (hit(btnMir, x, y)) {
    logHit("MIR", x, y);
    linkMode = LINK_MIRROR;
    applyLinkFrom1();
    drawLinkBtns();
    if (armed) driveImmediate();
    valuesDirty = true;
    slidersDirty = true;
    return;
  }
  if (hit(btnCpy, x, y)) {
    logHit("CPY", x, y);
    linkMode = LINK_COPY;
    applyLinkFrom1();
    drawLinkBtns();
    if (armed) driveImmediate();
    valuesDirty = true;
    slidersDirty = true;
    return;
  }

  if (hit(btnAct1, x, y)) { logHit("S1", x, y); active = ACT_S1; drawActiveBtns(); return; }
  if (hit(btnAct2, x, y)) { logHit("S2", x, y); active = ACT_S2; drawActiveBtns(); return; }
  if (hit(btnActB, x, y)) { logHit("BOTH", x, y); active = ACT_BOTH; drawActiveBtns(); return; }

  if (hit(btnN50m, x, y)) { logHit("-50", x, y); nudgeSmart(-NUDGE_COARSE_US); return; }
  if (hit(btnN10m, x, y)) { logHit("-10", x, y); nudgeSmart(-NUDGE_FINE_US); return; }
  if (hit(btnN10p, x, y)) { logHit("+10", x, y); nudgeSmart(+NUDGE_FINE_US); return; }
  if (hit(btnN50p, x, y)) { logHit("+50", x, y); nudgeSmart(+NUDGE_COARSE_US); return; }

  if (hit(btnSetMin, x, y)) { logHit("MIN", x, y); doSetMin(); return; }
  if (hit(btnSetMax, x, y)) { logHit("MAX", x, y); doSetMax(); return; }
  if (hit(btnSetCtr, x, y)) { logHit("SETC", x, y); doSetCenter(); return; }
  if (hit(btnGoCtr, x, y)) { logHit("GOC", x, y); doGoCenter(); return; }

  if (hit(btnSave, x, y)) { logHit("SAVE", x, y); saveEEPROM(); return; }
  if (hit(btnLoad, x, y)) {
    logHit("LOAD", x, y);
    if (loadEEPROM(false)) {
      drawLinkBtns();
      drawTierBtns();
      handleY1 = handleY2 = -1;
      if (armed) writeServosImmediate();
      updateSliders(true);
      valuesDirty = true;
    }
    return;
  }
  if (hit(btnSweep, x, y)) { logHit("SWP", x, y); doSweep(); return; }
}

void pollSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  switch (c) {
    case 'a': case 'A': toggleArm(); break;
    case 'c': case 'C': doGoCenter(); break;
    case 's': case 'S': saveEEPROM(); break;
    case 'l': case 'L':
      if (loadEEPROM(false)) {
        drawLinkBtns();
        drawTierBtns();
        handleY1 = handleY2 = -1;
        if (armed) writeServosImmediate();
        updateSliders(true);
        valuesDirty = true;
      }
      break;
    case 'w': if (armed) nudgeSmart(+NUDGE_FINE_US); break;
    case 'x': if (armed) nudgeSmart(-NUDGE_FINE_US); break;
    case 'h': case 'H': setLimitTier(TIER_HARD); break;
    case 'f': case 'F': setLimitTier(TIER_SOFT); break; // "soft" / Fine working range
    case '?':
      Serial.print(F("armed=")); Serial.print(armed);
      Serial.print(F(" tier=")); Serial.print(limitTier == TIER_SOFT ? "SOFT" : "HARD");
      Serial.print(F(" c1=")); Serial.print(center1);
      Serial.print(F(" c2=")); Serial.print(center2);
      Serial.print(F(" soft1=")); Serial.print(softMin1); Serial.print('-'); Serial.print(softMax1);
      Serial.print(F(" hard1=")); Serial.print(hardMin1); Serial.print('-'); Serial.println(hardMax1);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println(F("Wings ServoTest starting"));

  uint16_t id = tft.readID();
  if (id == 0xD3D3) id = 0x9486;
  Serial.print(F("TFT ID=0x"));
  Serial.println(id, HEX);
  tft.begin(id);
  tft.setRotation(0);

  loadEEPROM(true);

  servo1.attach(SERVO1_PIN, SERVO_ABS_MIN, SERVO_ABS_MAX);
  servo2.attach(SERVO2_PIN, SERVO_ABS_MIN, SERVO_ABS_MAX);
  actual1 = target1;
  actual2 = target2;
  servo1.writeMicroseconds(actual1);
  servo2.writeMicroseconds(actual2);

  drawStaticUI();
  uiReady = true;
  updateSliders(true);
  updateValues(true);

  Serial.println(F("Ready — DISARMED. SF=soft HD=hard  SETC/GOC  MIN/MAX  SAVE"));
}

void loop() {
  pollSerial();
  rampTowardTargets();

  TSPoint p = ts.getPoint();
  restoreTouchPins();

  if (p.z > MINPRESSURE && p.z < MAXPRESSURE) {
    int x, y;
    if (mapTouch(p, x, y)) handleTouch(x, y);
  } else {
    dragging = false;
  }

  if (slidersDirty) updateSliders(false);
  if (valuesDirty || statusMsg) updateValues(false);
}
