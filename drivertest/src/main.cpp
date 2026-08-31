/*
  Bench harness for servo_driver, on ONE servo: wrist hug, channel 0, GPIO5.

  What this is for. servo_driver has been through two code audits but has never
  executed. padtest validated the pad-transition PRIMITIVE; it did not validate
  this module, whose shared timer, group handover, reference supervision and
  refusal paths are all new code. The audits also found a defect that every
  static check passed happily, so hardware is the only remaining authority.

  PHYSICAL EXPECTATION, read before running:

    The first attach moves the horn to SERVO_CENTER (1500 us) from wherever it
    physically is. There is no way to know its position beforehand, so this is
    deliberate and announced rather than discovered. Every later test works in a
    narrow band around centre.

    Only channel 0 / GPIO5 is ever prepared or attached. The other four pads are
    parked by sdBegin and never touched again.

  MEASUREMENT NOTE:

    firstPulseUs times the first HIGH the servo receives after a handover. It
    runs without disabling interrupts, because blocking for a whole frame to
    take a measurement would be worse than the thing being measured. A
    preemption can only make a reading look LONGER (a missed falling edge) or,
    much less likely, clip the start. So a SHORT reading is meaningful and a long
    one is not alarming — which is the right way round, since short is the fault.
*/

#include <Arduino.h>
#include "servo_driver.h"

static const uint8_t CH = CH_WRIST2;      // 0

/*
  Where the horn is parked for every test.

  Operator-set, not assumed. This harness does not read NVS, so it has no access
  to the taught calibration — picking a park position here would be guessing at
  the mechanics, and a wrong guess drives the horn into a stop. Nothing attaches
  until 'p <us>' has been given, so the value is always a deliberate choice.
*/
static int  g_parkUs = 0;
static bool g_parkSet = false;

/*
  Wrist hug's real calibration, read off the Slave board over serial on
  2026-08-31 (fw 0.2.67). This harness cannot read NVS, so these are copied
  rather than loaded, which means they can go stale — override with 'w' if the
  board's stored values have moved since.

  PARK_SUGGESTED is that channel's last commanded position. Attaching there
  produces no motion, because it is already where the firmware believes the horn
  is. Any movement on the first attach is therefore itself a finding.
*/
static const int CAL_HARD_MIN = 900,  CAL_HARD_MAX = 2500;
static const int CAL_SOFT_MIN = 1200, CAL_SOFT_MAX = 2330;
static const int PARK_SUGGESTED = 1550;

static int g_softMin = CAL_SOFT_MIN, g_softMax = CAL_SOFT_MAX;

static bool parkReady() {
  if (g_parkSet) return true;
  Serial.println(F("REFUSED: no park position set. Use 'p <us>' first."));
  Serial.println(F("  This is the position the horn moves to on the first attach."));
  return false;
}

// A first pulse is expected to equal the prepared width. Anything below this is
// a truncation, which is the fault this driver exists to prevent.
static const int TRUNCATION_FLOOR_US = 1400;

// Kept so the summary can distinguish "sdBegin failed" from "sdBegin succeeded
// and supervision later marked the reference dead" — very different faults.
static bool g_beginOk = false, g_refOkAtBoot = false;

static uint32_t g_cycles = 0, g_short = 0, g_refusals = 0, g_detachFail = 0;
static uint32_t g_minFirst = 0xFFFFFFFF, g_maxFirst = 0;

// ------------------------------------------------------------------ helpers

static void pump(uint32_t ms) {
  const uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < ms) {
    sdService();
    delay(1);
  }
}

// Width of the next HIGH on a pin. Returns 0 if no pulse arrives in time.
static uint32_t firstPulseUs(int pin, uint32_t timeoutUs) {
  const uint32_t t0 = micros();
  while (!gpio_get_level((gpio_num_t)pin)) {
    if ((uint32_t)(micros() - t0) > timeoutUs) return 0;
  }
  const uint32_t rise = micros();
  while (gpio_get_level((gpio_num_t)pin)) {
    if ((uint32_t)(micros() - rise) > timeoutUs) return 0xFFFFFFFF;  // never fell
  }
  return (uint32_t)(micros() - rise);
}

static void expect(const char* what, SdResult got, SdResult want) {
  const bool ok = (got == want);
  if (!ok) g_refusals++;
  Serial.printf("  [%s] %-34s got=%-12s want=%s\n",
                ok ? "PASS" : "FAIL", what, sdResultName(got), sdResultName(want));
}

// ------------------------------------------------------------- phase A: refusals
/*
  Every one of these must REFUSE. A driver that connects when it should not is
  the whole hazard, so these matter more than the happy path. None of them move
  the servo: nothing here reaches a successful handover.
*/
// Any unexpected success here means a pad went live when it should not have.
// Release it at once and say so loudly rather than carrying on.
static void bailIfAttached(const char* where) {
  if (!sdAttached(CH)) return;
  Serial.printf("  *** CRITICAL: pad CONNECTED during %s ***\n", where);
  if (!sdDetach(CH)) {
    Serial.printf("  *** and detach REFUSED, faultMask=0x%02X, servo still driven ***\n",
                  sdFaultMask());
  } else {
    Serial.println(F("  released"));
  }
  g_refusals++;
}

static void phaseRefusals() {
  Serial.println(F("\n--- A: refusal paths (no motion expected) ---"));
  // Gated even though nothing here should move: if the driver is wrong, the pad
  // goes live at the prepared position, so that position must be the operator's
  // choice and not a default this harness invented.
  if (!parkReady()) return;

  sdDetachAll();

  expect("attach without prepare", sdAttach(CH), SD_NOT_PREPARED);
  expect("attach bad channel", sdAttach(NUM_SERVOS), SD_BAD_CH);

  const uint8_t group[2] = { CH, (uint8_t)NUM_SERVOS };
  expect("group of zero", sdAttachGroup(group, 0), SD_BAD_CH);
  // Group validation returns on the first fault it finds, and ch0 is unprepared
  // here, so this tests "a member is not ready" rather than the bad index.
  expect("group with unready member", sdAttachGroup(group, 2), SD_NOT_PREPARED);

  // Prepared but the duty has not latched yet: attaching now would emit the
  // channel's PREVIOUS duty as the servo's first pulse.
  sdDetachAll();
  const SdResult p = sdPrepare(CH, g_parkUs);
  Serial.printf("  prepare -> %s, preparedUs=%d\n", sdResultName(p), sdPreparedUs(CH));
  if (!sdDutyLive(CH)) {
    expect("attach before duty is live", sdAttach(CH), SD_NOT_LIVE);
    // The duty can latch in the gap between the check above and the attach, in
    // which case succeeding is correct rather than a fault — but the pad is now
    // live, so it still has to come back off.
    bailIfAttached("attach-before-live");
  } else {
    Serial.println(F("  (duty already live, latch race not observable this run)"));
  }

  // Now that ch0 is prepared and live, a bad index is the ONLY remaining fault —
  // which is the case that actually proves index validation, rather than being
  // masked by an unready member.
  const uint32_t tw2 = millis();
  while (!sdDutyLive(CH) && (uint32_t)(millis() - tw2) < SD_LIVE_TIMEOUT_MS) sdService();
  Serial.printf("  ch0 prepared and live=%d\n", sdDutyLive(CH) ? 1 : 0);
  expect("bad index, everything else ready", sdAttachGroup(group, 2), SD_BAD_CH);

  bailIfAttached("phase A");
  Serial.println(F("--- A done ---"));
}

/*
  Kills the phase reference by pointing GPIO21 at plain GPIO instead of the
  reference channel, then confirms the driver refuses rather than treating a
  dead line as a clean gap. This is the exact failure the second audit caught:
  the old "is it LOW right now" test passed instantly on a dead line.
*/
static void phaseDeadReference() {
  Serial.println(F("\n--- A2: dead phase reference (no motion expected) ---"));
  if (!parkReady()) return;
  sdDetachAll();

  gpio_matrix_out(PIN_LEDC_BIND, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_level((gpio_num_t)PIN_LEDC_BIND, 0);
  Serial.println(F("  reference unrouted, pad forced LOW"));

  sdPrepare(CH, g_parkUs);
  pump(SD_FRAME_MS * 2);
  const SdResult r = sdAttach(CH);
  const bool refused = (r == SD_NO_GAP || r == SD_NO_REF);
  if (!refused) g_refusals++;
  Serial.printf("  [%s] attach on dead reference -> %s (want NO_GAP or NO_REF)\n",
                refused ? "PASS" : "FAIL", sdResultName(r));
  bailIfAttached("dead-reference attach");

  // Supervision should also latch the reference bad on its own.
  pump(1500);
  Serial.printf("  after 1.5 s of service: sdRefOk()=%d (want 0)\n", sdRefOk() ? 1 : 0);

  Serial.println(F("  restoring reference via sdBegin()"));
  const bool ok = sdBegin();
  pump(SD_FRAME_MS * 3);
  Serial.printf("  [%s] sdBegin()=%d sdRefOk()=%d (want 1/1)\n",
                (ok && sdRefOk()) ? "PASS" : "FAIL", ok ? 1 : 0, sdRefOk() ? 1 : 0);

  Serial.println(F("--- A2 done ---"));
}

// -------------------------------------------------- phase B: attach/detach volume
/*
  The measurement that caught the original fault: attach, time the servo's very
  first pulse, release, repeat. Same position every cycle, so after the initial
  move to centre the horn should not move again. Any motion here IS the bug.
*/
static void phaseCycles(uint32_t n) {
  if (!parkReady()) return;
  Serial.printf("\n--- B: %lu attach/detach cycles at %d us ---\n",
                (unsigned long)n, g_parkUs);
  Serial.println(F("  horn moves to the park position on cycle 1, then should sit still"));

  sdDetachAll();

  for (uint32_t i = 0; i < n; i++) {
    const SdResult p = sdPrepare(CH, g_parkUs);
    if (p != SD_OK) {
      Serial.printf("  cycle %lu: prepare refused (%s), aborting\n",
                    (unsigned long)i, sdResultName(p));
      break;
    }

    // Let the duty latch before handing the pad over.
    const uint32_t tw = millis();
    while (!sdDutyLive(CH) && (uint32_t)(millis() - tw) < SD_LIVE_TIMEOUT_MS) sdService();

    const SdResult a = sdAttach(CH);
    if (a != SD_OK) {
      g_refusals++;
      Serial.printf("  cycle %lu: attach refused (%s)\n",
                    (unsigned long)i, sdResultName(a));
      pump(SD_FRAME_MS);
      continue;
    }

    const uint32_t w = firstPulseUs(SERVO_PINS[CH], 3u * 20000u);
    g_cycles++;
    if (w != 0 && w != 0xFFFFFFFF) {
      if (w < g_minFirst) g_minFirst = w;
      if (w > g_maxFirst) g_maxFirst = w;
      if ((int)w < TRUNCATION_FLOOR_US) {
        g_short++;
        Serial.printf("  *** cycle %lu: FIRST PULSE %lu us (floor %d) ***\n",
                      (unsigned long)i, (unsigned long)w, TRUNCATION_FLOOR_US);
      }
    } else {
      Serial.printf("  cycle %lu: no measurable pulse (%s)\n", (unsigned long)i,
                    w == 0 ? "never rose" : "never fell");
    }

    pump(60);   // a few frames of genuine hold

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

// --------------------------------------------------------- phase C: slew + limits
/*
  Confirms the per-frame ceiling actually governs the emitted pulse. Commands a
  jump far larger than one frame's worth and checks the driver walks there
  instead of stepping. THIS MOVES THE HORN, by design, within soft limits.
*/
static void phaseSlew() {
  Serial.println(F("\n--- C: slew ceiling (HORN WILL MOVE) ---"));
  if (!parkReady()) return;

  sdDetachAll();
  sdPrepare(CH, g_parkUs);
  const uint32_t tw = millis();
  while (!sdDutyLive(CH) && (uint32_t)(millis() - tw) < SD_LIVE_TIMEOUT_MS) sdService();
  if (sdAttach(CH) != SD_OK) {
    Serial.println(F("  attach failed, skipping"));
    return;
  }
  pump(100);

  // Deliberately modest, and clamped by the driver anyway. The point is to prove
  // the ceiling governs the step, not to explore the travel.
  const int goal = g_parkUs + 250;
  Serial.printf("  commanding %d -> %d in one call\n", sdWritten(CH), goal);
  sdCommand(CH, goal);

  int prev = sdWritten(CH);
  int worst = 0, frames = 0;
  const uint32_t t0 = millis();
  while (!sdSettled(CH) && (uint32_t)(millis() - t0) < 3000) {
    sdService();
    const int now = sdWritten(CH);
    if (now != prev) {
      const int step = abs(now - prev);
      if (step > worst) worst = step;
      prev = now;
      frames++;
    }
    delay(1);
  }
  Serial.printf("  [%s] largest single step %d us (ceiling %d), %d steps\n",
                (worst <= PULSE_MAX_STEP_US) ? "PASS" : "FAIL",
                worst, PULSE_MAX_STEP_US, frames);
  Serial.printf("  settled=%d written=%d commanded=%d\n",
                sdSettled(CH) ? 1 : 0, sdWritten(CH), sdCommanded(CH));

  Serial.println(F("  returning to centre"));
  sdCommand(CH, g_parkUs);
  const uint32_t t1 = millis();
  while (!sdSettled(CH) && (uint32_t)(millis() - t1) < 3000) { sdService(); delay(1); }
  pump(200);

  if (!sdDetach(CH)) {
    g_detachFail++;
    Serial.printf("  *** DETACH REFUSED, pad LEFT ROUTED, faultMask=0x%02X ***\n",
                  sdFaultMask());
  }
  Serial.println(F("--- C done ---"));
}

/*
  Limits are checked on the COMMANDED value, so this needs no motion: a request
  outside the window must be clamped before it can ever be emitted.
*/
static void phaseLimits() {
  Serial.println(F("\n--- D: limit clamping (no motion) ---"));
  sdDetachAll();

  sdSetLimits(CH, CAL_HARD_MIN, CAL_HARD_MAX, g_softMin, g_softMax);

  sdCommand(CH, SERVO_ABS_MAX + 500);
  Serial.printf("  [%s] command %d -> commanded %d (want %d)\n",
                sdCommanded(CH) == g_softMax ? "PASS" : "FAIL",
                SERVO_ABS_MAX + 500, sdCommanded(CH), g_softMax);

  sdCommand(CH, 0);
  Serial.printf("  [%s] command %d -> commanded %d (want %d)\n",
                sdCommanded(CH) == g_softMin ? "PASS" : "FAIL",
                0, sdCommanded(CH), g_softMin);

  // A prepared position must be re-clamped when the window tightens under it,
  // or the channel would attach outside the new limits. This is one of the
  // defects the first audit found.
  sdPrepare(CH, g_softMax);
  sdSetLimits(CH, CAL_HARD_MIN, CAL_HARD_MAX, 1400, 1600);
  Serial.printf("  [%s] prepared %d, window tightened to 1400..1600 -> preparedUs %d\n",
                sdPreparedUs(CH) <= 1600 ? "PASS" : "FAIL", g_softMax, sdPreparedUs(CH));

  sdSetLimits(CH, CAL_HARD_MIN, CAL_HARD_MAX, g_softMin, g_softMax);
  Serial.println(F("--- D done ---"));
}

// ------------------------------------------------------------------ report

static void report() {
  Serial.println(F("\n================ SUMMARY ================"));
  Serial.printf("cycles completed   : %lu\n", (unsigned long)g_cycles);
  Serial.printf("short first pulses : %lu   <-- must be 0\n", (unsigned long)g_short);
  Serial.printf("unexpected results : %lu   <-- must be 0\n", (unsigned long)g_refusals);
  Serial.printf("refused detaches   : %lu   <-- must be 0\n", (unsigned long)g_detachFail);
  if (g_cycles) {
    Serial.printf("first pulse range  : %lu..%lu us (prepared %d)\n",
                  (unsigned long)g_minFirst, (unsigned long)g_maxFirst, g_parkUs);
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
  Serial.println(F("\nservo_driver bench — wrist hug only (ch0 / GPIO5)"));
  Serial.println(F("  p <us>   set park position        REQUIRED before anything else"));
  Serial.println(F("  w <lo> <hi>  soft window          override the copied calibration"));
  Serial.println(F("  d        limit clamping           no motion"));
  Serial.println(F("  a        refusal paths            no motion expected"));
  Serial.println(F("  r        dead-reference refusal   no motion expected"));
  Serial.println(F("  b <n>    n attach/detach cycles   moves to park on cycle 1"));
  Serial.println(F("  c        slew ceiling             MOVES the horn +250us"));
  Serial.println(F("  s        summary"));
  Serial.println(F("  z        detach all + zero counters"));
  Serial.println(F("  ?        this help"));
  if (g_parkSet) Serial.printf("park = %d us\n", g_parkUs);
  else Serial.println(F("park = NOT SET, motion paths refused"));
}

static void setPark(const String& arg) {
  const long us = arg.toInt();
  if (us < SERVO_ABS_MIN || us > SERVO_ABS_MAX) {
    Serial.printf("REFUSED: %ld us outside %d..%d\n",
                  us, SERVO_ABS_MIN, SERVO_ABS_MAX);
    return;
  }
  g_parkUs = (int)us;
  g_parkSet = true;
  Serial.printf("park = %d us. The horn WILL move here on the next attach.\n", g_parkUs);
  Serial.printf("Driver clamps to the active soft window %d..%d.\n", g_softMin, g_softMax);
  if (g_parkUs != PARK_SUGGESTED) {
    Serial.printf("NOTE: %d is the stored last-commanded position; parking elsewhere\n"
                  "      means the first attach is a real move, not a no-op.\n",
                  PARK_SUGGESTED);
  }
}

// Override the soft window if the board's stored calibration has changed.
static void setWindow(const String& arg) {
  int lo = 0, hi = 0;
  if (sscanf(arg.c_str(), "%d %d", &lo, &hi) != 2) {
    Serial.printf("usage: w <softMin> <softMax>   (current %d..%d)\n", g_softMin, g_softMax);
    return;
  }
  if (lo < CAL_HARD_MIN || hi > CAL_HARD_MAX || lo >= hi - 20) {
    Serial.printf("REFUSED: %d..%d not sane inside hard %d..%d\n",
                  lo, hi, CAL_HARD_MIN, CAL_HARD_MAX);
    return;
  }
  g_softMin = lo;
  g_softMax = hi;
  sdSetLimits(CH, CAL_HARD_MIN, CAL_HARD_MAX, g_softMin, g_softMax);
  Serial.printf("soft window = %d..%d\n", g_softMin, g_softMax);
}

void setup() {
  Serial.begin(115200);
  delay(2500);
  Serial.println(F("\n\n=== servo_driver bench ==="));
  Serial.printf("channel %u (%s) on GPIO%u, reference GPIO%u\n",
                CH, SERVO_NAMES[CH], SERVO_PINS[CH], PIN_LEDC_BIND);

  const bool ok = sdBegin();
  g_beginOk = ok;
  g_refOkAtBoot = sdRefOk();
  Serial.printf("sdBegin()=%d sdRefOk()=%d\n", ok ? 1 : 0, sdRefOk() ? 1 : 0);
  if (!ok) {
    Serial.println(F("*** driver refused to start: nothing can attach ***"));
  }
  sdSetLimits(CH, CAL_HARD_MIN, CAL_HARD_MAX, g_softMin, g_softMax);
  Serial.printf("limits: hard %d..%d soft %d..%d (copied from the board, not read from NVS)\n",
                CAL_HARD_MIN, CAL_HARD_MAX, g_softMin, g_softMax);
  Serial.println(F("All five pads are parked. Only ch0 is ever attached."));
  Serial.printf("Nothing moves until you set a park position. Suggested: p %d\n",
                PARK_SUGGESTED);
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
    case 'b': {
      long n = rest.toInt();
      if (n < 1) n = 100;
      phaseCycles((uint32_t)n);
      report();
      break;
    }
    case 's': report(); break;
    case 'z':
      if (!sdDetachAll()) {
        Serial.printf("*** detachAll REFUSED, faultMask=0x%02X ***\n", sdFaultMask());
      }
      g_cycles = g_short = g_refusals = g_detachFail = 0;
      g_minFirst = 0xFFFFFFFF; g_maxFirst = 0;
      Serial.println(F("detached, counters zeroed"));
      break;
    default: help(); break;
  }
}
