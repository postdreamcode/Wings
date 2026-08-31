/*
  Wings servo-pad transition primitive — hardware validation.
  NO SERVO CONNECTED. GPIO 1-5 (the servo pins) are never touched.

  This is the candidate implementation of the new driver's only two pad
  operations, written the way it will appear in the firmware, plus a harness
  that tries to break it on real silicon.

    BIND_PIN   GPIO21 — the firmware's PIN_LEDC_BIND. A channel is bound here
                        so its frame can be read before a servo pad is touched.
    CTRL_PIN   GPIO10 — bare pad, nothing external on it. The control case.
    WRIST_HUG_PIN GPIO5 — the real wrist hug servo pin, so the servo's input
                        impedance and cable are the load. Driven ONLY with the
                        servo's V+ lead physically unplugged, which is the only
                        guarantee against motion: USB can backfeed the 6 V rail.

  Established by earlier probes on this board:
    - gpio_get_level() reads a live LEDC-driven pad accurately.
    - gpio_matrix_out() enables the pad output driver.
    - The three LEDC timers are not phase-aligned and their phase differs from
      one configuration to the next, so phase must be measured, never assumed.
    - Connecting or releasing at an arbitrary phase truncates the pulse to as
      little as 18 us, which commands a full-speed slam toward the minimum.

  Tests:
    1  padConnect at randomised phase — first pulse must always be complete
    2  padRelease at randomised phase — pulse in progress must never truncate
    3  refusal paths — a wrong or absent phase source must refuse, not guess
    4  does re-binding a channel to the bind pad glitch a servo pad already
       connected to that same channel? The old holdLedcUs did this on every
       attach, so the driver needs the answer.
*/

#include <Arduino.h>
#include "driver/gpio.h"
#include "rom/gpio.h"
#include "soc/gpio_sig_map.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

static const int BIND_PIN = 21;

// GPIO10 is a bare pad (control). GPIO5 is PIN_WRIST2, the wrist hug servo,
// which runs on LEDC channel 5 in the firmware — the same channel used here.
// GPIO5 is only ever driven with the servo's V+ lead physically unplugged.
static const int CTRL_PIN = 10;
static const int WRIST_HUG_PIN = 5;
static int g_servoPin = CTRL_PIN;

static const uint8_t CHAN = 5;

static const int LEDC_BITS = 12;
static const int LEDC_HZ = 50;
static const uint32_t FRAME_US = 20000;
static const int NOMINAL_US = 1500;

// A pulse this short reads as a command toward the servo's minimum.
static const uint32_t SLAM_US = 900;

// Bounded waits, so an unexpected condition refuses instead of hanging.
static const uint32_t GAP_WAIT_LIMIT_US = 2 * FRAME_US + 2000;
static const uint32_t VERIFY_LIMIT_US = 3 * FRAME_US;
static const uint32_t VERIFY_TOL_US = 120;

static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_radios = false;

enum PadFail : uint8_t {
  PAD_OK = 0,
  PAD_NO_PHASE_SOURCE,
  PAD_WRONG_PULSE,
  PAD_NO_GAP,
};
static PadFail g_lastFail = PAD_OK;

static uint32_t usToDuty(int us) {
  const uint32_t maxd = (1u << LEDC_BITS) - 1u;
  if (us < 0) us = 0;
  if (us > (int)FRAME_US) us = (int)FRAME_US;
  return (uint32_t)(((uint32_t)us * maxd) / FRAME_US);
}

static void pinCfg(int pin, gpio_mode_t mode, bool pullUp, bool pullDown) {
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << pin;
  c.mode = mode;
  c.pull_up_en = pullUp ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
  c.pull_down_en = pullDown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
  c.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&c);
}

static inline int lvl(int pin) {
  return gpio_get_level((gpio_num_t)pin);
}

static void matrixToLedc(int pin, uint8_t chan) {
  gpio_matrix_out(pin, LEDC_LS_SIG_OUT0_IDX + (chan % 8), false, false);
}

static void matrixToGpio(int pin) {
  gpio_matrix_out(pin, SIG_GPIO_OUT_IDX, false, false);
}

// ------------------------------------------------------- the primitive

// ledcSetup configures the LEDC *timer*, which must be running before any
// channel on it produces edges. ledcAttachPin then enables the channel itself.
static uint8_t g_timerReady = 0;
static void ledcTimerReady(uint8_t chan) {
  if (g_timerReady & (1u << chan)) return;
  ledcSetup(chan, LEDC_HZ, LEDC_BITS);
  g_timerReady |= (1u << chan);
}

// Bind a channel to the bind pad so its frame can be observed. Never call this
// for a channel whose servo pad is currently connected — test 4 checks why.
static void phaseSourceBind(uint8_t chan, int us) {
  ledcTimerReady(chan);
  ledcWrite(chan, usToDuty(us));
  ledcAttachPin(BIND_PIN, chan);
  ledcWrite(chan, usToDuty(us));
  pinCfg(BIND_PIN, GPIO_MODE_INPUT_OUTPUT, false, false);
  matrixToLedc(BIND_PIN, chan);
}

// Confirm the bind pad really carries the pulse we think it does. Without this
// a mis-bound channel would let padConnect read the wrong frame and connect
// mid-pulse with nothing to indicate anything was wrong.
static bool phaseSourceValid(int expectUs) {
  const uint32_t t0 = micros();
  while (lvl(BIND_PIN)) {
    if ((uint32_t)(micros() - t0) > VERIFY_LIMIT_US) { g_lastFail = PAD_NO_PHASE_SOURCE; return false; }
  }
  while (!lvl(BIND_PIN)) {
    if ((uint32_t)(micros() - t0) > VERIFY_LIMIT_US) { g_lastFail = PAD_NO_PHASE_SOURCE; return false; }
  }
  const uint32_t rise = micros();
  while (lvl(BIND_PIN)) {
    if ((uint32_t)(micros() - t0) > VERIFY_LIMIT_US) { g_lastFail = PAD_NO_PHASE_SOURCE; return false; }
  }
  const uint32_t w = micros() - rise;

  const uint32_t lo = ((uint32_t)expectUs > VERIFY_TOL_US) ? (uint32_t)expectUs - VERIFY_TOL_US : 0;
  const uint32_t hi = (uint32_t)expectUs + VERIFY_TOL_US;
  if (w < lo || w > hi) {
    g_lastFail = PAD_WRONG_PULSE;
    return false;
  }
  return true;
}

// Wait until a falling edge has just been seen, so the whole idle gap lies
// ahead. Waiting merely for "level is low" would leave an unknown amount of
// gap remaining; a falling edge guarantees the full 17.5-19 ms.
static bool waitFreshGap(int pin) {
  const uint32_t t0 = micros();
  while (!lvl(pin)) {
    if ((uint32_t)(micros() - t0) > GAP_WAIT_LIMIT_US) { g_lastFail = PAD_NO_GAP; return false; }
  }
  while (lvl(pin)) {
    if ((uint32_t)(micros() - t0) > GAP_WAIT_LIMIT_US) { g_lastFail = PAD_NO_GAP; return false; }
  }
  return true;
}

// Connect a servo pad to a running channel. Refuses rather than risk handing
// the servo a partial pulse. GPIO_OUT is pre-set LOW so the few microseconds
// before LEDC takes over match what the frame is already doing — the opposite
// of the old latch-HIGH workaround, which fabricated a blip in the gap.
// verifyNow  re-check the phase source on this connect, rather than trusting a
//            check already done when the channel was bound
// freshGap   wait for a falling edge so the whole idle gap lies ahead, rather
//            than connecting as soon as the level reads LOW
static bool padConnect(int pin, uint8_t chan, int expectUs,
                       bool verifyNow, bool freshGap) {
  g_lastFail = PAD_OK;
  if (verifyNow && !phaseSourceValid(expectUs)) return false;

  if (freshGap) {
    if (!waitFreshGap(BIND_PIN)) return false;
  } else {
    const uint32_t t0 = micros();
    while (lvl(BIND_PIN)) {
      if ((uint32_t)(micros() - t0) > GAP_WAIT_LIMIT_US) { g_lastFail = PAD_NO_GAP; return false; }
    }
  }

  portENTER_CRITICAL(&g_mux);
  gpio_set_level((gpio_num_t)pin, 0);
  pinCfg(pin, GPIO_MODE_INPUT_OUTPUT, false, false);
  matrixToLedc(pin, chan);
  portEXIT_CRITICAL(&g_mux);
  return true;
}

// Release a servo pad. The pad is live, so its own level gives the phase
// directly. Releasing anywhere in the gap is safe: the servo simply stops
// receiving pulses and holds, which is what a detach means.
static bool padRelease(int pin) {
  g_lastFail = PAD_OK;
  const uint32_t t0 = micros();
  while (lvl(pin)) {
    if ((uint32_t)(micros() - t0) > GAP_WAIT_LIMIT_US) { g_lastFail = PAD_NO_GAP; return false; }
  }

  portENTER_CRITICAL(&g_mux);
  gpio_set_level((gpio_num_t)pin, 0);
  matrixToGpio(pin);
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
  portEXIT_CRITICAL(&g_mux);
  return true;
}

// ------------------------------------------------------- harness support

// Park the stand-in servo pad: not driven, pulled down so its resting level is
// known. The firmware leaves it floating; the pull-down here exists only so
// edges can be timed.
static void servoPadPark() {
  matrixToGpio(g_servoPin);
  pinCfg(g_servoPin, GPIO_MODE_INPUT, false, true);
  gpio_set_direction((gpio_num_t)g_servoPin, GPIO_MODE_INPUT);
}

static bool firstHighRun(uint32_t connectUs, uint32_t* w, bool* mid) {
  const uint32_t t0 = micros();
  if (lvl(g_servoPin)) {
    *mid = true;
    while (lvl(g_servoPin)) {
      if ((uint32_t)(micros() - t0) > 3u * FRAME_US) return false;
    }
    *w = micros() - connectUs;
    return true;
  }
  *mid = false;
  while (!lvl(g_servoPin)) {
    if ((uint32_t)(micros() - t0) > 3u * FRAME_US) return false;
  }
  const uint32_t r = micros();
  while (lvl(g_servoPin)) {
    if ((uint32_t)(micros() - t0) > 4u * FRAME_US) return false;
  }
  *w = micros() - r;
  return true;
}

static void radioPoke(uint32_t i) {
  if (!g_radios || (i % 8) != 0) return;
  const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t blob[32];
  memset(blob, 0xA5, sizeof(blob));
  esp_now_send(bcast, blob, sizeof(blob));
}

// ------------------------------------------------------- test 1: connect

static void test1(uint32_t trials, bool radios, bool verifyNow, bool freshGap,
                  const char* label) {
  uint32_t n = 0, mid = 0, slam = 0, clean = 0, refused = 0, unmeasured = 0;
  uint32_t minUs = 0xFFFFFFFF, maxUs = 0, worstCost = 0;
  uint64_t costSum = 0;

  for (uint32_t i = 0; i < trials; i++) {
    servoPadPark();
    delayMicroseconds((uint32_t)random(0, (long)FRAME_US));

    const uint32_t c0 = micros();
    if (!padConnect(g_servoPin, CHAN, NOMINAL_US, verifyNow, freshGap)) { refused++; continue; }
    const uint32_t connectUs = micros();
    const uint32_t cost = connectUs - c0;
    costSum += cost;
    if (cost > worstCost) worstCost = cost;

    uint32_t w = 0;
    bool m = false;
    if (!firstHighRun(connectUs, &w, &m)) { unmeasured++; servoPadPark(); continue; }

    n++;
    if (m) mid++;
    if (w < minUs) minUs = w;
    if (w > maxUs) maxUs = w;
    if (w < SLAM_US) slam++;
    else if (w >= 1350 && w <= 1650) clean++;

    servoPadPark();
    if (radios) radioPoke(i);
    if ((i % 50) == 0) delay(1);
  }

  Serial.printf("    %-26s radios %-3s n=%lu mid=%lu SLAM=%lu clean=%lu min=%luus max=%luus refused=%lu unmeas=%lu cost avg=%luus max=%luus\n",
                label, radios ? "ON" : "OFF",
                (unsigned long)n, (unsigned long)mid, (unsigned long)slam,
                (unsigned long)clean,
                (minUs == 0xFFFFFFFF) ? 0UL : (unsigned long)minUs,
                (unsigned long)maxUs, (unsigned long)refused,
                (unsigned long)unmeasured,
                (unsigned long)(n ? (uint32_t)(costSum / n) : 0),
                (unsigned long)worstCost);
}

// ------------------------------------------------------- test 2: release

static void test2(uint32_t trials, bool radios) {
  uint32_t n = 0, inHigh = 0, trunc = 0, clean = 0, refused = 0;
  uint32_t minUs = 0xFFFFFFFF, maxUs = 0, worstCost = 0;
  uint64_t costSum = 0;

  for (uint32_t i = 0; i < trials; i++) {
    servoPadPark();
    if (!padConnect(g_servoPin, CHAN, NOMINAL_US, false, false)) { refused++; continue; }
    delay(3);

    const uint32_t t0 = micros();
    while (lvl(g_servoPin)) { if ((uint32_t)(micros() - t0) > 2u * FRAME_US) break; }
    while (!lvl(g_servoPin)) { if ((uint32_t)(micros() - t0) > 2u * FRAME_US) break; }
    const uint32_t rise = micros();

    const uint32_t offset = (uint32_t)random(0, (long)FRAME_US);
    while ((uint32_t)(micros() - rise) < offset) { }
    const bool wasHigh = lvl(g_servoPin) != 0;

    const uint32_t c0 = micros();
    if (!padRelease(g_servoPin)) { refused++; servoPadPark(); continue; }
    const uint32_t tRel = micros();
    const uint32_t cost = tRel - c0;
    costSum += cost;
    if (cost > worstCost) worstCost = cost;

    pinCfg(g_servoPin, GPIO_MODE_INPUT, false, true);
    uint32_t tLow = micros();
    while (lvl(g_servoPin)) {
      tLow = micros();
      if ((uint32_t)(tLow - tRel) > 4000) break;
    }
    tLow = micros();

    n++;
    if (wasHigh) {
      inHigh++;
      const uint32_t eff = tLow - rise;
      if (eff < minUs) minUs = eff;
      if (eff > maxUs) maxUs = eff;
      if (eff < SLAM_US) trunc++;
      else clean++;
    }

    servoPadPark();
    if (radios) radioPoke(i);
    if ((i % 50) == 0) delay(1);
  }

  Serial.printf("    radios %-3s n=%lu inHigh=%lu TRUNC=%lu clean=%lu min=%luus max=%luus refused=%lu cost avg=%luus max=%luus\n",
                radios ? "ON" : "OFF",
                (unsigned long)n, (unsigned long)inHigh, (unsigned long)trunc,
                (unsigned long)clean,
                (minUs == 0xFFFFFFFF) ? 0UL : (unsigned long)minUs,
                (unsigned long)maxUs, (unsigned long)refused,
                (unsigned long)(n ? (uint32_t)(costSum / n) : 0),
                (unsigned long)worstCost);
}

// ------------------------------------------------------- test 3: refusal

static const char* failName(PadFail f) {
  switch (f) {
    case PAD_OK: return "OK";
    case PAD_NO_PHASE_SOURCE: return "NO_PHASE_SOURCE";
    case PAD_WRONG_PULSE: return "WRONG_PULSE";
    case PAD_NO_GAP: return "NO_GAP";
  }
  return "?";
}

static void test3() {
  Serial.println(F("  3 refusal paths — a bad phase source must refuse, not connect"));

  servoPadPark();
  phaseSourceBind(CHAN, NOMINAL_US);
  delay(40);
  bool ok = padConnect(g_servoPin, CHAN, NOMINAL_US, true, true);
  Serial.printf("    healthy source                 -> %-9s (%-15s) %s\n",
                ok ? "connected" : "refused", failName(g_lastFail),
                ok ? "expected" : "UNEXPECTED");
  if (ok) padRelease(g_servoPin);
  servoPadPark();

  ledcWrite(CHAN, usToDuty(2400));
  delay(40);
  ok = padConnect(g_servoPin, CHAN, NOMINAL_US, true, true);
  Serial.printf("    channel 2400us, expected 1500  -> %-9s (%-15s) %s\n",
                ok ? "connected" : "refused", failName(g_lastFail),
                (!ok && g_lastFail == PAD_WRONG_PULSE) ? "expected" : "UNEXPECTED");
  if (ok) padRelease(g_servoPin);
  servoPadPark();

  ledcWrite(CHAN, usToDuty(NOMINAL_US));
  matrixToGpio(BIND_PIN);
  gpio_set_level((gpio_num_t)BIND_PIN, 0);
  delay(40);
  ok = padConnect(g_servoPin, CHAN, NOMINAL_US, true, true);
  Serial.printf("    bind pad unrouted              -> %-9s (%-15s) %s\n",
                ok ? "connected" : "refused", failName(g_lastFail),
                (!ok && g_lastFail == PAD_NO_PHASE_SOURCE) ? "expected" : "UNEXPECTED");
  if (ok) padRelease(g_servoPin);
  servoPadPark();

  phaseSourceBind(CHAN, NOMINAL_US);
  ledcWrite(CHAN, 0);
  delay(40);
  ok = padConnect(g_servoPin, CHAN, NOMINAL_US, true, true);
  Serial.printf("    channel stopped, duty 0        -> %-9s (%-15s) %s\n",
                ok ? "connected" : "refused", failName(g_lastFail),
                (!ok && g_lastFail == PAD_NO_PHASE_SOURCE) ? "expected" : "UNEXPECTED");
  if (ok) padRelease(g_servoPin);

  phaseSourceBind(CHAN, NOMINAL_US);
  delay(40);
  servoPadPark();
}

// ------------------------------------------------------- test 4: re-bind

// The old holdLedcUs called ledcAttachPin(PIN_LEDC_BIND, chan) on every attach.
// If that disturbs a servo pad already connected to the same channel, the
// driver must never re-bind a live channel.
static void test4(uint32_t trials) {
  uint32_t n = 0, bad = 0;
  uint32_t minUs = 0xFFFFFFFF, maxUs = 0;

  for (uint32_t i = 0; i < trials; i++) {
    servoPadPark();
    phaseSourceBind(CHAN, NOMINAL_US);
    delay(30);
    if (!padConnect(g_servoPin, CHAN, NOMINAL_US, false, false)) continue;
    delay(3);

    delayMicroseconds((uint32_t)random(0, (long)FRAME_US));
    ledcAttachPin(BIND_PIN, CHAN);
    pinCfg(BIND_PIN, GPIO_MODE_INPUT_OUTPUT, false, false);
    matrixToLedc(BIND_PIN, CHAN);

    for (int k = 0; k < 3; k++) {
      const uint32_t t0 = micros();
      while (lvl(g_servoPin)) { if ((uint32_t)(micros() - t0) > 2u * FRAME_US) break; }
      while (!lvl(g_servoPin)) { if ((uint32_t)(micros() - t0) > 2u * FRAME_US) break; }
      const uint32_t r = micros();
      while (lvl(g_servoPin)) { if ((uint32_t)(micros() - t0) > 3u * FRAME_US) break; }
      const uint32_t w = micros() - r;
      n++;
      if (w < minUs) minUs = w;
      if (w > maxUs) maxUs = w;
      if (w < SLAM_US || w > 2100) bad++;
    }

    padRelease(g_servoPin);
    servoPadPark();
    if ((i % 20) == 0) delay(1);
  }

  Serial.printf("    pulses=%lu BAD=%lu min=%luus max=%luus  -> re-binding a live channel is %s\n",
                (unsigned long)n, (unsigned long)bad,
                (minUs == 0xFFFFFFFF) ? 0UL : (unsigned long)minUs,
                (unsigned long)maxUs,
                (n == 0) ? "UNTESTED (no pulses captured)"
                         : (bad ? "UNSAFE" : "safe in this test"));
}

// ------------------------------------------------------- test 5: floating

/*
  The audit's objection to probe H: the pull-down used to make the falling edge
  timeable is also what turns a mid-pulse disconnect into a short pulse. The
  firmware releases to INPUT with no pull, leaving the pad floating.

  So: release mid-pulse exactly as the firmware does, then watch the pad and
  record when, if ever, it reads LOW. A bare pad with nothing on it bounds the
  no-load end of the question. It cannot answer what a servo input plus cable
  does — only a servo on the line can.

    latchHigh  true  = firmware behaviour, GPIO_OUT left at 1
               false = GPIO_OUT pre-set to 0 before handing over
    pull       the pull applied as part of the release
*/
static void test5(uint32_t trials, bool latchHigh, bool pullDown, const char* label) {
  const uint32_t WATCH_US = 40000;
  uint32_t n = 0, wentLow = 0, heldHigh = 0;
  uint32_t minLow = 0xFFFFFFFF, maxLow = 0;
  uint64_t lowSum = 0;

  for (uint32_t i = 0; i < trials; i++) {
    servoPadPark();
    if (!padConnect(g_servoPin, CHAN, NOMINAL_US, false, false)) continue;
    delay(3);

    const uint32_t t0 = micros();
    while (lvl(g_servoPin)) { if ((uint32_t)(micros() - t0) > 2u * FRAME_US) break; }
    while (!lvl(g_servoPin)) { if ((uint32_t)(micros() - t0) > 2u * FRAME_US) break; }
    const uint32_t rise = micros();

    // Land solidly inside the HIGH phase, which is where truncation would occur.
    const uint32_t offset = 200 + (uint32_t)random(0, 1000);
    while ((uint32_t)(micros() - rise) < offset) { }
    if (!lvl(g_servoPin)) continue;

    uint32_t tRel;
    portENTER_CRITICAL(&g_mux);
    gpio_set_level((gpio_num_t)g_servoPin, latchHigh ? 1 : 0);
    tRel = micros();
    matrixToGpio(g_servoPin);
    pinCfg(g_servoPin, GPIO_MODE_INPUT, false, pullDown);
    portEXIT_CRITICAL(&g_mux);

    bool low = false;
    uint32_t tLow = 0;
    while ((uint32_t)(micros() - tRel) < WATCH_US) {
      if (!lvl(g_servoPin)) { tLow = micros(); low = true; break; }
    }

    n++;
    if (low) {
      const uint32_t d = tLow - tRel;
      wentLow++;
      lowSum += d;
      if (d < minLow) minLow = d;
      if (d > maxLow) maxLow = d;
    } else {
      heldHigh++;
    }

    servoPadPark();
    if ((i % 20) == 0) delay(1);
  }

  Serial.printf("    %-28s n=%lu wentLow=%lu heldHigh(40ms)=%lu  time-to-low min=%luus avg=%luus max=%luus\n",
                label, (unsigned long)n, (unsigned long)wentLow,
                (unsigned long)heldHigh,
                (minLow == 0xFFFFFFFF) ? 0UL : (unsigned long)minLow,
                (unsigned long)(wentLow ? (uint32_t)(lowSum / wentLow) : 0),
                (unsigned long)maxLow);
}

// ------------------------------------------------------- test 6: decay

/*
  After a mid-pulse release the firmware leaves the pad floating at HIGH. Test 5
  showed it holds HIGH for at least 40 ms. The question that matters is how long
  it holds, because that floating HIGH is what makes a mid-pulse connect benign:
  the servo just sees a longer over-range pulse. If the node decays to LOW during
  a stage gap, the next connect lands on a LOW line and a mid-pulse hand-off
  becomes a real short pulse.

  Watches for seconds rather than milliseconds, and reports where the level sits
  at intervals so a slow decay is visible even if it never crosses the logic
  threshold.
*/
static void test6(uint32_t trials, uint32_t watchMs) {
  uint32_t n = 0, everLow = 0;
  uint32_t minLowMs = 0xFFFFFFFF, maxLowMs = 0;
  uint64_t lowSum = 0;

  // Level at 50, 250, 500, 1000, 2000 ms after release, counted as HIGH.
  const uint32_t marks[5] = {50, 250, 500, 1000, 2000};
  uint32_t highAt[5] = {0, 0, 0, 0, 0};
  uint32_t sampled[5] = {0, 0, 0, 0, 0};

  for (uint32_t i = 0; i < trials; i++) {
    servoPadPark();
    if (!padConnect(g_servoPin, CHAN, NOMINAL_US, false, false)) continue;
    delay(3);

    const uint32_t t0 = micros();
    while (lvl(g_servoPin)) { if ((uint32_t)(micros() - t0) > 2u * FRAME_US) break; }
    while (!lvl(g_servoPin)) { if ((uint32_t)(micros() - t0) > 2u * FRAME_US) break; }
    const uint32_t rise = micros();

    const uint32_t offset = 200 + (uint32_t)random(0, 1000);
    while ((uint32_t)(micros() - rise) < offset) { }
    if (!lvl(g_servoPin)) continue;

    // Exactly what releaseOnePin does today.
    gpio_set_level((gpio_num_t)g_servoPin, 1);
    const uint32_t tRel = millis();
    matrixToGpio(g_servoPin);
    pinCfg(g_servoPin, GPIO_MODE_INPUT, false, false);

    bool low = false;
    uint32_t lowMs = 0;
    int mi = 0;
    while ((uint32_t)(millis() - tRel) < watchMs) {
      const uint32_t el = millis() - tRel;
      const int v = lvl(g_servoPin);
      while (mi < 5 && el >= marks[mi]) {
        if (marks[mi] <= watchMs) {
          sampled[mi]++;
          if (v) highAt[mi]++;
        }
        mi++;
      }
      if (!v && !low) {
        low = true;
        lowMs = el;
        break;
      }
    }

    n++;
    if (low) {
      everLow++;
      lowSum += lowMs;
      if (lowMs < minLowMs) minLowMs = lowMs;
      if (lowMs > maxLowMs) maxLowMs = lowMs;
    }

    servoPadPark();
    delay(1);
  }

  Serial.printf("    GPIO%d n=%lu wentLow=%lu  time-to-low min=%lums avg=%lums max=%lums\n",
                g_servoPin, (unsigned long)n, (unsigned long)everLow,
                (minLowMs == 0xFFFFFFFF) ? 0UL : (unsigned long)minLowMs,
                (unsigned long)(everLow ? (uint32_t)(lowSum / everLow) : 0),
                (unsigned long)maxLowMs);
  Serial.print(F("    still HIGH at: "));
  for (int k = 0; k < 5; k++) {
    if (!sampled[k]) continue;
    Serial.printf("%lums=%lu/%lu  ", (unsigned long)marks[k],
                  (unsigned long)highAt[k], (unsigned long)sampled[k]);
  }
  Serial.println();
}

// ------------------------------------------------------- characterisation

// Drives nothing. Applies each internal pull in turn and reports what the pad
// settles to, which reveals whatever is on the wire. This is the scan that
// found GPIO21's external pull-up.
static void characterize(int pin, const char* what) {
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << pin;
  c.mode = GPIO_MODE_INPUT;
  c.intr_type = GPIO_INTR_DISABLE;

  c.pull_up_en = GPIO_PULLUP_ENABLE;
  c.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_config(&c);
  delay(5);
  const int up = gpio_get_level((gpio_num_t)pin);

  c.pull_up_en = GPIO_PULLUP_DISABLE;
  c.pull_down_en = GPIO_PULLDOWN_ENABLE;
  gpio_config(&c);
  delay(5);
  const int dn = gpio_get_level((gpio_num_t)pin);

  c.pull_up_en = GPIO_PULLUP_DISABLE;
  c.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_config(&c);
  delay(5);
  const int fl = gpio_get_level((gpio_num_t)pin);

  const char* verdict;
  if (up == 1 && dn == 0) verdict = "follows internal pulls: negligible load on the wire";
  else if (up == 0 && dn == 0) verdict = "held LOW externally: something pulls this line down";
  else if (up == 1 && dn == 1) verdict = "held HIGH externally: something pulls this line up";
  else verdict = "inconsistent";

  Serial.printf("    GPIO%-2d %-14s pullup=%d pulldown=%d floating=%d  %s\n",
                pin, what, up, dn, fl, verdict);
}

// ------------------------------------------------------- driver

static void radiosUp() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(50);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return;
  const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, bcast, 6);
  p.channel = 1;
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
  g_radios = true;
}

static void runAll(uint32_t trials) {
  Serial.println();
  Serial.println(F("######## Wings pad transition primitive — validation ########"));
  Serial.printf("bind=GPIO%d servo=GPIO%d chan=%d %dHz/%dbit nominal=%dus trials=%lu\n",
                BIND_PIN, g_servoPin, CHAN, LEDC_HZ, LEDC_BITS, NOMINAL_US,
                (unsigned long)trials);
  Serial.println(F("NO SERVO CONNECTED. GPIO 1-5 are never touched."));

  phaseSourceBind(CHAN, NOMINAL_US);
  delay(60);
  servoPadPark();

  // Without this the whole run degenerates into refusals with no clue why.
  uint32_t sn = 0, shi = 0;
  const uint32_t st0 = micros();
  while ((uint32_t)(micros() - st0) < 10u * FRAME_US) {
    sn++;
    if (lvl(BIND_PIN)) shi++;
  }
  const uint32_t sdX100 = sn ? (uint32_t)(((uint64_t)shi * 10000ull) / sn) : 0;
  Serial.printf("  sanity: GPIO%d high=%lu.%02lu%% (expect 7.50%%)  GPIO%d parked reads %d (expect 0)\n",
                BIND_PIN, (unsigned long)(sdX100 / 100), (unsigned long)(sdX100 % 100),
                g_servoPin, lvl(g_servoPin));
  if (sdX100 < 600 || sdX100 > 900) {
    Serial.println(F("  ABORT: bind pad is not carrying the frame; nothing below is meaningful"));
    return;
  }

  Serial.println(F("  0 read-only characterisation — nothing is driven here"));
  characterize(CTRL_PIN, "bare pad");
  characterize(WRIST_HUG_PIN, "wrist hug");

  Serial.println(F("  5 mid-pulse release: does the line fall, and how fast?"));
  Serial.println(F("    A fall inside 500-900us after the rise is a slam command."));
  Serial.println(F("    No fall at all means the servo sees an over-range pulse and discards it."));

  // GPIO5 is not driven again: USB backfeed proved able to move the servo even
  // with V+ unplugged, so the bench has no verified-dead rail.
  g_servoPin = CTRL_PIN;
  test5(200, true, false, "firmware: latch HIGH, no pull");
  test5(200, false, true, "latch LOW + pull-down (new)");
  servoPadPark();

  Serial.println(F("  6 how long does the released floating line stay HIGH?"));
  Serial.println(F("    That floating HIGH is what makes a mid-pulse connect benign today."));
  test6(25, 2500);
  servoPadPark();

  if (trials > 600) {
    Serial.println(F("  1 padConnect at randomised phase — SLAM must be 0"));
    test1(trials, false, false, false, "hoisted verify, level-LOW");
    Serial.println(F("  2 padRelease at randomised phase — TRUNC must be 0"));
    test2(trials, false);
    test3();
  }

  Serial.println();
  Serial.println(F("######## done ######## send 's' short, 'l' long"));
}

void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 4000) delay(10);
  delay(600);

  randomSeed(esp_random());
  servoPadPark();
  radiosUp();
  runAll(500);
}

void loop() {
  if (Serial.available()) {
    int c = -1;
    while (Serial.available()) {
      const int ch = Serial.read();
      if (ch != '\r' && ch != '\n') c = ch;
    }
    runAll((c == 'l' || c == 'L') ? 3000 : 500);
  }
  delay(20);
}
