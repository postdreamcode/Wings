/*
  Bench harness for servo_driver, on the two wrist servos:
    ch0 WRIST_HUG   GPIO5
    ch1 WRIST_RAISE GPIO4

  What this is for. servo_driver went through two code audits before it ever
  executed, and its first run on hardware immediately exposed two defects that
  both audits had passed — either of which alone made every attach impossible.
  So hardware is the authority here, not review.

  PHYSICAL EXPECTATION, read before running:

    Each channel's park position defaults to the value the wing firmware last
    commanded for it, so attaching there should produce NO motion. Movement on
    the first attach is therefore itself a finding, not an expected side effect.
    Nothing attaches until park is set explicitly per channel.

    Only ch0 and ch1 are ever prepared or attached. The other three pads are
    parked by sdBegin and never touched.

  MEASUREMENT NOTE:

    Pulse timing runs without disabling interrupts, because blocking for a whole
    frame to take a measurement would be worse than the thing being measured. A
    preemption can only make a reading look LONGER (a missed falling edge) or, far
    less likely, clip its start. So a SHORT reading is meaningful while a long one
    is not alarming — the right way round, since short is the fault.
*/

#include <Arduino.h>
#include "servo_driver.h"

/*
  Real calibration, read off the Slave board over serial on 2026-08-31 (fw
  0.2.67). This harness cannot read NVS, so these are copied rather than loaded
  and can go stale — override with 'w'.

  park is that channel's last commanded position. Attaching there is a no-op.
*/
struct BenchCh {
  uint8_t ch;
  int hardMin, hardMax;
  int softMin, softMax;
  int parkSuggested;
  int parkUs;
  bool parkSet;
};

static BenchCh g_bc[2] = {
  { CH_WRIST2, 900, 2500, 1200, 2330, 1550, 0, false },  // WRIST_HUG,   GPIO5
  { CH_WRIST1, 500, 2500,  530, 2500, 1680, 0, false },  // WRIST_RAISE, GPIO4
};
static const uint8_t NBC = 2;

// A first pulse is expected to equal the prepared width. Anything below this is
// a truncation, which is the fault this driver exists to prevent.
static const int TRUNCATION_FLOOR_US = 1400;

static bool g_beginOk = false, g_refOkAtBoot = false;
static uint32_t g_cycles = 0, g_short = 0, g_refusals = 0, g_detachFail = 0;
static uint32_t g_minFirst = 0xFFFFFFFF, g_maxFirst = 0;
static uint32_t g_groupCycles = 0, g_skewViolations = 0, g_maxSkewUs = 0;

static BenchCh& bc(uint8_t i) { return g_bc[i]; }

static bool parkReady(uint8_t i) {
  if (g_bc[i].parkSet) return true;
  Serial.printf("REFUSED: no park position for ch%u (%s). Use 'p %u %d'.\n",
                g_bc[i].ch, SERVO_NAMES[g_bc[i].ch], g_bc[i].ch, g_bc[i].parkSuggested);
  return false;
}

// ------------------------------------------------------------------ helpers

static void pump(uint32_t ms) {
  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < ms) {
    sdService();
    delay(1);
  }
}

static bool waitLive(uint8_t ch) {
  const uint32_t t0 = millis();
  while (!sdDutyLive(ch)) {
    if ((uint32_t)(millis() - t0) > SD_LIVE_TIMEOUT_MS) return false;
    sdService();
  }
  return true;
}

// Width of the next HIGH on a pin. 0 = never rose, ALL ONES = never fell.
static uint32_t firstPulseUs(int pin, uint32_t timeoutUs) {
  const uint32_t t0 = micros();
  while (!gpio_get_level((gpio_num_t)pin)) {
    if ((uint32_t)(micros() - t0) > timeoutUs) return 0;
  }
  const uint32_t rise = micros();
  while (gpio_get_level((gpio_num_t)pin)) {
    if ((uint32_t)(micros() - rise) > timeoutUs) return 0xFFFFFFFF;
  }
  return (uint32_t)(micros() - rise);
}

/*
  Times the first pulse on TWO pads at once, and the skew between their rising
  edges.

  The skew is the point. Both channels sit on one shared LEDC timer, so both pads
  must go HIGH on the same counter reset. That is the premise the whole
  single-timer design rests on: it is what makes one idle gap common to every
  channel, and therefore what makes a group handover inside one critical section
  meaningful. If these two pads do not rise together, the premise is wrong and
  group attach is not safe no matter how clean the individual widths look.
*/
struct PairMeas { uint32_t aUs, bUs, skewUs; bool ok; };

static PairMeas firstPulsePair(int pinA, int pinB, uint32_t timeoutUs) {
  PairMeas m = { 0, 0, 0, false };
  const uint32_t t0 = micros();

  while (!gpio_get_level((gpio_num_t)pinA) && !gpio_get_level((gpio_num_t)pinB)) {
    if ((uint32_t)(micros() - t0) > timeoutUs) return m;
  }
  const uint32_t rise = micros();

  // Whichever came up first, see how long the other takes to follow.
  const bool aUp = gpio_get_level((gpio_num_t)pinA);
  const int lag = aUp ? pinB : pinA;
  if (!gpio_get_level((gpio_num_t)lag)) {
    while (!gpio_get_level((gpio_num_t)lag)) {
      if ((uint32_t)(micros() - rise) > timeoutUs) return m;
    }
    m.skewUs = (uint32_t)(micros() - rise);
  }

  uint32_t fa = 0, fb = 0;
  for (;;) {
    if (!fa && !gpio_get_level((gpio_num_t)pinA)) fa = micros();
    if (!fb && !gpio_get_level((gpio_num_t)pinB)) fb = micros();
    if (fa && fb) break;
    if ((uint32_t)(micros() - rise) > timeoutUs) return m;
  }
  m.aUs = fa - rise;
  m.bUs = fb - rise;
  m.ok = true;
  return m;
}

static void expect(const char* what, SdResult got, SdResult want) {
  const bool ok = (got == want);
  if (!ok) g_refusals++;
  Serial.printf("  [%s] %-34s got=%-12s want=%s\n",
                ok ? "PASS" : "FAIL", what, sdResultName(got), sdResultName(want));
}

// Any unexpected success means a pad went live when it should not have. Release
// it at once and say so, rather than carrying on with a servo being driven.
static void bailIfAttached(const char* where) {
  bool any = false;
  for (uint8_t i = 0; i < NBC; i++) {
    if (!sdAttached(g_bc[i].ch)) continue;
    any = true;
    Serial.printf("  *** CRITICAL: ch%u pad CONNECTED during %s ***\n", g_bc[i].ch, where);
    if (!sdDetach(g_bc[i].ch)) {
      Serial.printf("  *** detach REFUSED, faultMask=0x%02X, servo still driven ***\n",
                    sdFaultMask());
    }
  }
  if (any) g_refusals++;
}

static void applyLimits() {
  for (uint8_t i = 0; i < NBC; i++) {
    sdSetLimits(g_bc[i].ch, g_bc[i].hardMin, g_bc[i].hardMax,
                g_bc[i].softMin, g_bc[i].softMax);
  }
}

// ------------------------------------------------------------- A: refusals

static void phaseRefusals() {
  Serial.println(F("\n--- A: refusal paths (no motion expected) ---"));
  // Gated even though nothing here should move: if the driver is wrong, a pad
  // goes live at the prepared position, so that position must be the operator's
  // choice rather than one this harness invented.
  if (!parkReady(0)) return;
  const uint8_t CH = g_bc[0].ch;

  sdDetachAll();

  expect("attach without prepare", sdAttach(CH), SD_NOT_PREPARED);
  expect("attach bad channel", sdAttach(NUM_SERVOS), SD_BAD_CH);

  const uint8_t group[2] = { CH, (uint8_t)NUM_SERVOS };
  expect("group of zero", sdAttachGroup(group, 0), SD_BAD_CH);
  // Validation returns on the first fault it finds and ch0 is unprepared here,
  // so this tests "a member is not ready", not the bad index.
  expect("group with unready member", sdAttachGroup(group, 2), SD_NOT_PREPARED);

  sdDetachAll();
  const SdResult p = sdPrepare(CH, g_bc[0].parkUs);
  Serial.printf("  prepare -> %s, preparedUs=%d\n", sdResultName(p), sdPreparedUs(CH));
  if (!sdDutyLive(CH)) {
    expect("attach before duty is live", sdAttach(CH), SD_NOT_LIVE);
    // The duty can latch between the check above and the attach, in which case
    // succeeding is correct — but the pad is live now and must come back off.
    bailIfAttached("attach-before-live");
  } else {
    Serial.println(F("  (duty already live, latch race not observable this run)"));
  }

  // With ch0 prepared and live, a bad index is the ONLY remaining fault, which
  // is what actually proves index validation rather than being masked.
  waitLive(CH);
  Serial.printf("  ch0 prepared and live=%d\n", sdDutyLive(CH) ? 1 : 0);
  expect("bad index, everything else ready", sdAttachGroup(group, 2), SD_BAD_CH);

  bailIfAttached("phase A");
  Serial.println(F("--- A done ---"));
}

/*
  Kills the phase reference by pointing GPIO21 at plain GPIO instead of the
  reference channel, then confirms the driver refuses rather than reading a dead
  line as a clean gap. A pad forced LOW is exactly what the original naive
  "is it LOW right now" check would have accepted.
*/
static void phaseDeadReference() {
  Serial.println(F("\n--- A2: dead phase reference (no motion expected) ---"));
  if (!parkReady(0)) return;
  const uint8_t CH = g_bc[0].ch;
  sdDetachAll();

  gpio_matrix_out(PIN_LEDC_BIND, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_level((gpio_num_t)PIN_LEDC_BIND, 0);
  Serial.println(F("  reference unrouted, pad forced LOW"));

  sdPrepare(CH, g_bc[0].parkUs);
  pump(SD_FRAME_MS * 2);
  const SdResult r = sdAttach(CH);
  const bool refused = (r == SD_NO_GAP || r == SD_NO_REF);
  if (!refused) g_refusals++;
  Serial.printf("  [%s] attach on dead reference -> %s (want NO_GAP or NO_REF)\n",
                refused ? "PASS" : "FAIL", sdResultName(r));
  bailIfAttached("dead-reference attach");

  // Supervision marks the reference dead once a full SD_REF_WINDOW_MS has passed
  // with no HIGH. Allowing a little over one window is enough now that staleness
  // replaced the tumbling window; under the old scheme even 1.5 s could pass or
  // fail depending on where the window boundary happened to fall.
  const uint32_t died = millis();
  pump(1400);
  Serial.printf("  [%s] after %lu ms of service: sdRefOk()=%d (want 0)\n",
                sdRefOk() ? "FAIL" : "PASS",
                (unsigned long)(millis() - died), sdRefOk() ? 1 : 0);
  if (sdRefOk()) g_refusals++;

  Serial.println(F("  restoring reference via sdBegin()"));
  const bool ok = sdBegin();
  applyLimits();
  pump(SD_FRAME_MS * 3);
  Serial.printf("  [%s] sdBegin()=%d sdRefOk()=%d (want 1/1)\n",
                (ok && sdRefOk()) ? "PASS" : "FAIL", ok ? 1 : 0, sdRefOk() ? 1 : 0);

  Serial.println(F("--- A2 done ---"));
}

// ---------------------------------------------- B: single-channel attach cycles

static void phaseCycles(uint32_t n) {
  if (!parkReady(0)) return;
  const uint8_t CH = g_bc[0].ch;
  Serial.printf("\n--- B: %lu single-channel cycles, ch%u at %d us ---\n",
                (unsigned long)n, CH, g_bc[0].parkUs);

  sdDetachAll();

  for (uint32_t i = 0; i < n; i++) {
    if (sdPrepare(CH, g_bc[0].parkUs) != SD_OK) {
      Serial.printf("  cycle %lu: prepare refused, aborting\n", (unsigned long)i);
      break;
    }
    if (!waitLive(CH)) {
      Serial.printf("  cycle %lu: duty never went live\n", (unsigned long)i);
      break;
    }

    const SdResult a = sdAttach(CH);
    if (a != SD_OK) {
      g_refusals++;
      Serial.printf("  cycle %lu: attach refused (%s)\n", (unsigned long)i, sdResultName(a));
      pump(SD_FRAME_MS);
      continue;
    }

    const uint32_t w = firstPulseUs(SERVO_PINS[CH], 3u * 20000u);
    g_cycles++;

    // Only testable while a pad is genuinely live: re-preparing an attached
    // channel writes a duty outside the slew ceiling, so it must be refused and
    // the caller pushed toward sdCommand instead.
    if (i == 0) expect("prepare while attached", sdPrepare(CH, g_bc[0].parkUs), SD_ALREADY);

    if (w != 0 && w != 0xFFFFFFFF) {
      if (w < g_minFirst) g_minFirst = w;
      if (w > g_maxFirst) g_maxFirst = w;
      if ((int)w < TRUNCATION_FLOOR_US) {
        g_short++;
        Serial.printf("  *** cycle %lu: FIRST PULSE %lu us (floor %d) ***\n",
                      (unsigned long)i, (unsigned long)w, TRUNCATION_FLOOR_US);
      }
    }

    pump(60);

    if (!sdDetach(CH)) {
      g_detachFail++;
      Serial.printf("  *** cycle %lu: DETACH REFUSED, pad LEFT ROUTED, faultMask=0x%02X ***\n",
                    (unsigned long)i, sdFaultMask());
      Serial.println(F("  *** stopping: a servo is still being driven ***"));
      break;
    }
    pump(40);

    if ((i % 100) == 99) {
      Serial.printf("  ... %lu cycles, %lu short, first-pulse %lu..%lu us\n",
                    (unsigned long)(i + 1), (unsigned long)g_short,
                    (unsigned long)g_minFirst, (unsigned long)g_maxFirst);
    }
  }
  Serial.println(F("--- B done ---"));
}

// --------------------------------------------------- G: group attach of two pads
/*
  The test the single shared timer exists to justify. Both pads are handed over
  inside ONE idle gap in ONE critical section, so this checks three things at
  once: neither first pulse is truncated, both pads rise on the same counter
  reset (skew ~0), and the group releases together without fault.
*/
static void phaseGroup(uint32_t n) {
  if (!parkReady(0) || !parkReady(1)) return;

  const uint8_t chs[2] = { g_bc[0].ch, g_bc[1].ch };
  const int pinA = SERVO_PINS[chs[0]], pinB = SERVO_PINS[chs[1]];

  Serial.printf("\n--- G: %lu GROUP cycles, ch%u@%dus (GPIO%d) + ch%u@%dus (GPIO%d) ---\n",
                (unsigned long)n, chs[0], g_bc[0].parkUs, pinA,
                chs[1], g_bc[1].parkUs, pinB);
  Serial.println(F("  both parked at their last commanded values, so neither should move"));

  sdDetachAll();

  for (uint32_t i = 0; i < n; i++) {
    bool ready = true;
    for (uint8_t k = 0; k < 2; k++) {
      if (sdPrepare(chs[k], g_bc[k].parkUs) != SD_OK) ready = false;
    }
    for (uint8_t k = 0; k < 2 && ready; k++) {
      if (!waitLive(chs[k])) ready = false;
    }
    if (!ready) {
      Serial.printf("  cycle %lu: could not ready the group, aborting\n", (unsigned long)i);
      break;
    }

    const SdResult a = sdAttachGroup(chs, 2);
    if (a != SD_OK) {
      g_refusals++;
      Serial.printf("  cycle %lu: group attach refused (%s)\n",
                    (unsigned long)i, sdResultName(a));
      pump(SD_FRAME_MS);
      continue;
    }

    const PairMeas m = firstPulsePair(pinA, pinB, 3u * 20000u);
    g_groupCycles++;
    if (m.ok) {
      const uint32_t lo = (m.aUs < m.bUs) ? m.aUs : m.bUs;
      const uint32_t hi = (m.aUs > m.bUs) ? m.aUs : m.bUs;
      if (lo < g_minFirst) g_minFirst = lo;
      if (hi > g_maxFirst) g_maxFirst = hi;
      if (m.skewUs > g_maxSkewUs) g_maxSkewUs = m.skewUs;

      if ((int)m.aUs < TRUNCATION_FLOOR_US || (int)m.bUs < TRUNCATION_FLOOR_US) {
        g_short++;
        Serial.printf("  *** cycle %lu: TRUNCATED first pulse a=%lu b=%lu us ***\n",
                      (unsigned long)i, (unsigned long)m.aUs, (unsigned long)m.bUs);
      }
      // Shared timer means the rising edges are the same event. Anything past a
      // few us of measurement noise contradicts the design premise.
      if (m.skewUs > 50) {
        g_skewViolations++;
        Serial.printf("  *** cycle %lu: RISE SKEW %lu us — pads not phase aligned ***\n",
                      (unsigned long)i, (unsigned long)m.skewUs);
      }
    } else {
      Serial.printf("  cycle %lu: pair measurement failed\n", (unsigned long)i);
    }

    pump(60);

    if (!sdDetachGroup(chs, 2)) {
      g_detachFail++;
      Serial.printf("  *** cycle %lu: GROUP DETACH REFUSED, pads LEFT ROUTED, faultMask=0x%02X ***\n",
                    (unsigned long)i, sdFaultMask());
      Serial.println(F("  *** stopping: servos are still being driven ***"));
      break;
    }
    pump(40);

    if ((i % 50) == 49) {
      Serial.printf("  ... %lu group cycles, %lu short, %lu skew, pulses %lu..%lu us, maxSkew %lu us\n",
                    (unsigned long)(i + 1), (unsigned long)g_short,
                    (unsigned long)g_skewViolations,
                    (unsigned long)g_minFirst, (unsigned long)g_maxFirst,
                    (unsigned long)g_maxSkewUs);
    }
  }
  Serial.println(F("--- G done ---"));
}

// ----------------------------------------------------------- C: slew ceiling
/*
  Confirms the per-frame ceiling governs the emitted pulse: commands a jump far
  larger than one frame's worth and checks the driver walks there instead of
  stepping. THIS MOVES BOTH HORNS, by design, within their soft windows.
*/
static void phaseSlew() {
  Serial.println(F("\n--- C: slew ceiling (HORNS WILL MOVE) ---"));
  if (!parkReady(0) || !parkReady(1)) return;

  const uint8_t chs[2] = { g_bc[0].ch, g_bc[1].ch };
  sdDetachAll();
  for (uint8_t k = 0; k < 2; k++) { sdPrepare(chs[k], g_bc[k].parkUs); waitLive(chs[k]); }
  const SdResult a = sdAttachGroup(chs, 2);
  if (a != SD_OK) {
    Serial.printf("  group attach failed (%s), skipping\n", sdResultName(a));
    return;
  }
  pump(100);

  int prev[2], worst[2] = { 0, 0 }, steps[2] = { 0, 0 };
  for (uint8_t k = 0; k < 2; k++) {
    prev[k] = sdWritten(chs[k]);
    sdCommand(chs[k], g_bc[k].parkUs + 250);
    Serial.printf("  ch%u commanding %d -> %d\n", chs[k], prev[k], g_bc[k].parkUs + 250);
  }

  const uint32_t t0 = millis();
  while (!sdAllSettled() && (uint32_t)(millis() - t0) < 3000) {
    sdService();
    for (uint8_t k = 0; k < 2; k++) {
      const int now = sdWritten(chs[k]);
      if (now == prev[k]) continue;
      const int step = abs(now - prev[k]);
      if (step > worst[k]) worst[k] = step;
      prev[k] = now;
      steps[k]++;
    }
    delay(1);
  }

  for (uint8_t k = 0; k < 2; k++) {
    Serial.printf("  [%s] ch%u largest step %d us (ceiling %d), %d steps, written=%d\n",
                  (worst[k] <= PULSE_MAX_STEP_US && worst[k] > 0) ? "PASS" : "FAIL",
                  chs[k], worst[k], PULSE_MAX_STEP_US, steps[k], sdWritten(chs[k]));
    if (worst[k] > PULSE_MAX_STEP_US) g_refusals++;
  }

  Serial.println(F("  returning to park"));
  for (uint8_t k = 0; k < 2; k++) sdCommand(chs[k], g_bc[k].parkUs);
  const uint32_t t1 = millis();
  while (!sdAllSettled() && (uint32_t)(millis() - t1) < 3000) { sdService(); delay(1); }
  pump(200);

  if (!sdDetachGroup(chs, 2)) {
    g_detachFail++;
    Serial.printf("  *** GROUP DETACH REFUSED, faultMask=0x%02X ***\n", sdFaultMask());
  }
  Serial.println(F("--- C done ---"));
}

// -------------------------------------------------------------- D: limits

static void phaseLimits() {
  Serial.println(F("\n--- D: limit clamping (no motion) ---"));
  sdDetachAll();
  applyLimits();

  for (uint8_t i = 0; i < NBC; i++) {
    const uint8_t ch = g_bc[i].ch;
    sdCommand(ch, SERVO_ABS_MAX + 500);
    Serial.printf("  [%s] ch%u command %d -> %d (want softMax %d)\n",
                  sdCommanded(ch) == g_bc[i].softMax ? "PASS" : "FAIL",
                  ch, SERVO_ABS_MAX + 500, sdCommanded(ch), g_bc[i].softMax);
    sdCommand(ch, 0);
    Serial.printf("  [%s] ch%u command %d -> %d (want softMin %d)\n",
                  sdCommanded(ch) == g_bc[i].softMin ? "PASS" : "FAIL",
                  ch, 0, sdCommanded(ch), g_bc[i].softMin);
  }

  // A prepared position must be re-clamped when the window tightens under it, or
  // the channel would attach outside the new limits. One of the audit findings.
  const uint8_t ch0 = g_bc[0].ch;
  sdPrepare(ch0, g_bc[0].softMax);
  sdSetLimits(ch0, g_bc[0].hardMin, g_bc[0].hardMax, 1400, 1600);
  Serial.printf("  [%s] prepared %d, window tightened to 1400..1600 -> preparedUs %d\n",
                sdPreparedUs(ch0) <= 1600 ? "PASS" : "FAIL",
                g_bc[0].softMax, sdPreparedUs(ch0));

  applyLimits();
  Serial.println(F("--- D done ---"));
}

// ------------------------------------------------------------------ console

static void report() {
  Serial.println(F("\n================ SUMMARY ================"));
  Serial.printf("single cycles      : %lu\n", (unsigned long)g_cycles);
  Serial.printf("group cycles       : %lu\n", (unsigned long)g_groupCycles);
  Serial.printf("short first pulses : %lu   <-- must be 0\n", (unsigned long)g_short);
  Serial.printf("rise-skew failures : %lu   <-- must be 0\n", (unsigned long)g_skewViolations);
  Serial.printf("unexpected results : %lu   <-- must be 0\n", (unsigned long)g_refusals);
  Serial.printf("refused detaches   : %lu   <-- must be 0\n", (unsigned long)g_detachFail);
  if (g_cycles || g_groupCycles) {
    Serial.printf("first pulse range  : %lu..%lu us\n",
                  (unsigned long)g_minFirst, (unsigned long)g_maxFirst);
    Serial.printf("worst rise skew    : %lu us\n", (unsigned long)g_maxSkewUs);
  }
  Serial.printf("at boot: sdBegin=%d refOk=%d\n", g_beginOk ? 1 : 0, g_refOkAtBoot ? 1 : 0);
  Serial.printf("now    : attachMask=0x%02X faultMask=0x%02X refOk=%d\n",
                sdAttachMask(), sdFaultMask(), sdRefOk() ? 1 : 0);
  if (g_refOkAtBoot && !sdRefOk()) {
    Serial.println(F("reference was alive at boot and has since been marked dead"));
  }
  Serial.println(F("========================================="));
}

static void help() {
  Serial.println(F("\nservo_driver bench — wrist pair"));
  Serial.println(F("  p <ch> <us>  set park           REQUIRED per channel before motion"));
  Serial.println(F("  w <ch> <lo> <hi>  soft window   override the copied calibration"));
  Serial.println(F("  d        limit clamping         no motion"));
  Serial.println(F("  a        refusal paths          no motion expected"));
  Serial.println(F("  r        dead-reference refusal no motion expected"));
  Serial.println(F("  b <n>    n single-ch cycles     ch0 only"));
  Serial.println(F("  g <n>    n GROUP cycles         ch0+ch1 in one gap"));
  Serial.println(F("  c        slew ceiling           MOVES both horns +250us"));
  Serial.println(F("  s        summary"));
  Serial.println(F("  z        detach all + zero counters"));
  for (uint8_t i = 0; i < NBC; i++) {
    Serial.printf("  ch%u %-12s park=%s%d soft=%d..%d GPIO%d\n",
                  g_bc[i].ch, SERVO_NAMES[g_bc[i].ch],
                  g_bc[i].parkSet ? "" : "UNSET, suggest ",
                  g_bc[i].parkSet ? g_bc[i].parkUs : g_bc[i].parkSuggested,
                  g_bc[i].softMin, g_bc[i].softMax, SERVO_PINS[g_bc[i].ch]);
  }
}

static int findBc(int ch) {
  for (uint8_t i = 0; i < NBC; i++) if (g_bc[i].ch == ch) return i;
  return -1;
}

static void setPark(const String& arg) {
  int ch = -1, us = 0;
  if (sscanf(arg.c_str(), "%d %d", &ch, &us) != 2) {
    Serial.println(F("usage: p <ch> <us>"));
    return;
  }
  const int i = findBc(ch);
  if (i < 0) { Serial.printf("REFUSED: ch%d is not under test\n", ch); return; }
  if (us < g_bc[i].hardMin || us > g_bc[i].hardMax) {
    Serial.printf("REFUSED: %d outside ch%d hard %d..%d\n",
                  us, ch, g_bc[i].hardMin, g_bc[i].hardMax);
    return;
  }
  g_bc[i].parkUs = us;
  g_bc[i].parkSet = true;
  Serial.printf("ch%d park = %d us. The horn WILL move here on the next attach.\n", ch, us);
  if (us != g_bc[i].parkSuggested) {
    Serial.printf("NOTE: %d is the stored last-commanded value; parking elsewhere makes\n"
                  "      the first attach a real move rather than a no-op.\n",
                  g_bc[i].parkSuggested);
  }
}

static void setWindow(const String& arg) {
  int ch = -1, lo = 0, hi = 0;
  if (sscanf(arg.c_str(), "%d %d %d", &ch, &lo, &hi) != 3) {
    Serial.println(F("usage: w <ch> <softMin> <softMax>"));
    return;
  }
  const int i = findBc(ch);
  if (i < 0) { Serial.printf("REFUSED: ch%d is not under test\n", ch); return; }
  if (lo < g_bc[i].hardMin || hi > g_bc[i].hardMax || lo >= hi - 20) {
    Serial.printf("REFUSED: %d..%d not sane inside hard %d..%d\n",
                  lo, hi, g_bc[i].hardMin, g_bc[i].hardMax);
    return;
  }
  g_bc[i].softMin = lo;
  g_bc[i].softMax = hi;
  applyLimits();
  Serial.printf("ch%d soft window = %d..%d\n", ch, lo, hi);
}

void setup() {
  Serial.begin(115200);
  delay(2500);
  Serial.println(F("\n\n=== servo_driver bench: wrist pair ==="));

  const bool ok = sdBegin();
  g_beginOk = ok;
  g_refOkAtBoot = sdRefOk();
  Serial.printf("sdBegin()=%d sdRefOk()=%d  (reference GPIO%d)\n",
                ok ? 1 : 0, sdRefOk() ? 1 : 0, PIN_LEDC_BIND);
  if (!ok) Serial.println(F("*** driver refused to start: nothing can attach ***"));

  applyLimits();
  Serial.println(F("Limits copied from the board, NOT read from NVS."));
  Serial.println(F("All five pads parked. Only ch0 and ch1 are ever attached."));
  Serial.println(F("Nothing moves until park is set for the channels involved."));
  help();
}

void loop() {
  sdService();

  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (!line.length()) return;

  const char c = line[0];
  const String rest = line.substring(1);

  switch (c) {
    case 'p': setPark(rest); break;
    case 'w': setWindow(rest); break;
    case 'a': phaseRefusals(); break;
    case 'r': phaseDeadReference(); break;
    case 'd': phaseLimits(); break;
    case 'c': phaseSlew(); break;
    case 'b': { long n = rest.toInt(); if (n < 1) n = 100; phaseCycles((uint32_t)n); report(); break; }
    case 'g': { long n = rest.toInt(); if (n < 1) n = 50;  phaseGroup((uint32_t)n);  report(); break; }
    case 's': report(); break;
    case 'z':
      if (!sdDetachAll()) {
        Serial.printf("*** detachAll REFUSED, faultMask=0x%02X ***\n", sdFaultMask());
      }
      g_cycles = g_short = g_refusals = g_detachFail = 0;
      g_groupCycles = g_skewViolations = g_maxSkewUs = 0;
      g_minFirst = 0xFFFFFFFF; g_maxFirst = 0;
      Serial.println(F("detached, counters zeroed"));
      break;
    default: help(); break;
  }
}
