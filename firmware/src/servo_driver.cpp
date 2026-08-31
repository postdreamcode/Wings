#include "servo_driver.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "rom/gpio.h"
#include "soc/gpio_sig_map.h"

// One LEDC channel per job, carried over because the harness and the 12 V
// units are known to work on these. Which timer they use is chosen here rather
// than inherited from Arduino's (chan/2)%4 formula — see servo_driver.h.
static const uint8_t LEDC_CH[NUM_SERVOS] = {5, 6, 2, 3, 4};

static const ledc_mode_t  SD_MODE  = LEDC_LOW_SPEED_MODE;
static const ledc_timer_t SD_TIMER = LEDC_TIMER_0;
static const int LEDC_BITS = 12;
static const int LEDC_HZ = 50;
static const uint32_t FRAME_US = 20000;

struct SdChan {
  int hardMin, hardMax;
  int softMin, softMax;
  int commanded;
  int written;
  int preparedUs;
  bool prepared;
  bool attached;
};

static SdChan g_ch[NUM_SERVOS];
static uint8_t g_mask = 0;
static uint32_t g_lastWriteMs = 0;
static bool g_begun = false;

// Channels the driver could not prove safe to release, and so deliberately left
// routed and pulsing rather than truncate a pulse. Sticky: only a successful
// release clears a bit.
static uint8_t g_faultMask = 0;

// Continuous supervision of the phase reference. Every sdService tick samples
// the reference pad; if a whole supervision window passes without seeing both
// levels then the reference is not toggling, the gap test would be meaningless,
// and attaches are refused until it recovers.
static bool g_refOk = false;
static bool g_refSawHigh = false;
static bool g_refSawLow = false;
static uint16_t g_refTicks = 0;
static const uint16_t SD_REF_WINDOW_TICKS = 50;   // ~1 s at one tick per frame

// Guards a pad handover. The window is a couple of register writes long; an
// interrupt landing between them is what leaves a pad half-owned.
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

const char* sdResultName(SdResult r) {
  switch (r) {
    case SD_OK:           return "OK";
    case SD_ALREADY:      return "ALREADY";
    case SD_BAD_CH:       return "BAD_CH";
    case SD_NOT_PREPARED: return "NOT_PREPARED";
    case SD_NOT_LIVE:     return "NOT_LIVE";
    case SD_NO_GAP:       return "NO_GAP";
    case SD_NO_REF:       return "NO_REF";
  }
  return "?";
}

static uint32_t usToDuty(int us) {
  const uint32_t maxd = (1u << LEDC_BITS) - 1u;
  if (us < 0) us = 0;
  if (us > (int)FRAME_US) us = (int)FRAME_US;
  return (uint32_t)(((uint32_t)us * maxd) / FRAME_US);
}

// Intersection of the absolute, hard and soft windows. A pulse outside this is
// never emitted, whatever is asked for.
static int clampUs(uint8_t ch, int us) {
  const SdChan& c = g_ch[ch];
  int lo = SERVO_ABS_MIN;
  int hi = SERVO_ABS_MAX;
  if (c.hardMin > lo) lo = c.hardMin;
  if (c.hardMax < hi) hi = c.hardMax;
  if (c.softMin > lo) lo = c.softMin;
  if (c.softMax < hi) hi = c.softMax;
  if (lo > hi) {
    // Limits contradict each other. Fall back to the hard window, then the
    // absolute one, rather than inventing a position from a bad config.
    lo = (c.hardMin > SERVO_ABS_MIN) ? c.hardMin : SERVO_ABS_MIN;
    hi = (c.hardMax < SERVO_ABS_MAX) ? c.hardMax : SERVO_ABS_MAX;
    if (lo > hi) { lo = SERVO_ABS_MIN; hi = SERVO_ABS_MAX; }
  }
  return constrain(us, lo, hi);
}

static int servoPin(uint8_t ch) { return SERVO_PINS[ch]; }
static int ledcSig(uint8_t chan) { return LEDC_LS_SIG_OUT0_IDX + (chan % 8); }

// Writes go to the duty register; the hardware copies them to duty_rd when the
// frame latches. So liveDuty is what the servo is actually being sent, which
// makes it a real check rather than an echo of the write.
static void setDuty(uint8_t chan, uint32_t duty) {
  ledc_set_duty(SD_MODE, (ledc_channel_t)chan, duty);
  ledc_update_duty(SD_MODE, (ledc_channel_t)chan);
}

static uint32_t liveDuty(uint8_t chan) {
  return ledc_get_duty(SD_MODE, (ledc_channel_t)chan);
}

/*
  Resting state for a released pad: input, internal pull-down enabled, output
  register pre-set LOW.

  The previous design left GPIO_OUT latched HIGH with no pull, which measured as
  a line floating at 3.3 V for 43-45 ms before decaying. Holding it at 0 V means
  a detached servo sees no pulses at all, which is what makes it hold position,
  and a floating high-impedance node cannot pick up a coupled transient that
  looks like a pulse.

  This is only safe together with gap-aligned connects: on a line resting LOW an
  unaligned connect produces a genuinely short pulse, whereas the old
  floating-HIGH line accidentally masked one. The two belong together.

  The pull-down is left enabled while the pad is driven. It costs tens of
  microamps against a push-pull output, and leaving it alone avoids touching pad
  configuration after the matrix has been pointed at the channel.
*/
static void padPark(uint8_t ch) {
  const int pin = servoPin(ch);
  portENTER_CRITICAL(&g_mux);
  gpio_set_level((gpio_num_t)pin, 0);
  gpio_matrix_out(pin, SIG_GPIO_OUT_IDX, false, false);
  portEXIT_CRITICAL(&g_mux);

  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << pin;
  c.mode = GPIO_MODE_INPUT;
  c.pull_up_en = GPIO_PULLUP_DISABLE;
  c.pull_down_en = GPIO_PULLDOWN_ENABLE;
  c.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&c);
}

// Binds a channel to the timer with an explicit timer_sel, which is the whole
// reason for using the IDF call instead of ledcSetup. gpio_num is the bind pad
// and never a servo pad: a servo pin is only ever routed at attach time, in the
// idle gap, by this module.
static bool channelConfig(uint8_t chan, uint32_t duty) {
  ledc_channel_config_t c = {};
  c.gpio_num = PIN_LEDC_BIND;
  c.speed_mode = SD_MODE;
  c.channel = (ledc_channel_t)chan;
  c.intr_type = LEDC_INTR_DISABLE;
  c.timer_sel = SD_TIMER;
  c.duty = duty;
  c.hpoint = 0;
  return ledc_channel_config(&c) == ESP_OK;
}

// The reference pad must be readable, so it is configured INPUT_OUTPUT.
// ledc_channel_config leaves a pad output-only, and gpio_get_level on an
// output-only pad reads a constant — which would make the gap test lie.
static void refPadInit() {
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << PIN_LEDC_BIND;
  c.mode = GPIO_MODE_INPUT_OUTPUT;
  c.pull_up_en = GPIO_PULLUP_DISABLE;
  c.pull_down_en = GPIO_PULLDOWN_DISABLE;
  c.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&c);
  gpio_matrix_out(PIN_LEDC_BIND, ledcSig(SD_REF_CHANNEL), false, false);
}

// Proves the reference actually toggles, rather than assuming it. Boot-time
// only: costs a couple of frames.
static bool refProveAlive() {
  if (liveDuty(SD_REF_CHANNEL) != usToDuty(SD_REF_US)) return false;
  bool sawHigh = false, sawLow = false;
  const uint32_t t0 = micros();
  while ((uint32_t)(micros() - t0) < 3u * FRAME_US) {
    if (gpio_get_level((gpio_num_t)PIN_LEDC_BIND)) sawHigh = true;
    else sawLow = true;
    if (sawHigh && sawLow) return true;
  }
  return false;
}

/*
  Returns at the START of the idle gap, having watched the pin go HIGH for a
  plausible pulse width and then LOW.

  "Currently LOW" is not good enough, for two independent reasons. A line stuck
  LOW — dead channel, unrouted pad, input buffer off — would satisfy it
  instantly, and the safety property would evaporate while every test still
  passed. And a sample taken in the last microseconds of a gap, followed by an
  interrupt of a few hundred microseconds before the matrix write, walks the
  handover into the next pulse and straight into the slam band.

  Requiring the falling edge fixes both: it proves the line is pulsing right
  now, and it leaves the full idle gap ahead of the handover, which is orders of
  magnitude more margin than any plausible preemption.

  The minimum HIGH dwell stops a coupled transient from impersonating a pulse
  and handing back a falling edge that means nothing. A real pulse here is at
  least SERVO_ABS_MIN wide, so anything far below that is not the waveform.
  Short highs are discarded and the search continues until the timeout.
*/
static bool waitForGapStartOn(int pin, uint32_t timeoutMs) {
  const uint32_t t0 = micros();
  const uint32_t limit = timeoutMs * 1000u;
  for (;;) {
    while (!gpio_get_level((gpio_num_t)pin)) {
      if ((uint32_t)(micros() - t0) > limit) return false;
    }
    const uint32_t tRise = micros();
    while (gpio_get_level((gpio_num_t)pin)) {
      if ((uint32_t)(micros() - t0) > limit) return false;
    }
    if ((uint32_t)(micros() - tRise) >= SD_MIN_HIGH_US) return true;
  }
}

// Attach times its handover off the reference, which is the one signal under
// continuous supervision and is wider than any servo pulse.
static bool waitForGapStart(uint32_t timeoutMs) {
  return waitForGapStartOn(PIN_LEDC_BIND, timeoutMs);
}

bool sdBegin() {
  /*
    Re-entry must not silence a channel whose pad is still live: writing duty 0
    to a connected pad is a 0 us frame, which is the original slam. Release
    properly first, through the gap-aligned path.

    The reference is marked bad before the teardown so nothing can attach into a
    half-configured peripheral. If the release cannot be done safely, the whole
    re-init is abandoned rather than forced: tearing down the timer under a live
    pad is the slam this refuses to cause.
  */
  if (g_begun) {
    g_refOk = false;
    if (!sdDetachAll()) return false;
  }

  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    SdChan& c = g_ch[i];
    c.hardMin = DEFAULT_HARD_MIN;
    c.hardMax = DEFAULT_HARD_MAX;
    c.softMin = DEFAULT_SOFT_MIN;
    c.softMax = DEFAULT_SOFT_MAX;
    c.commanded = SERVO_CENTER;
    c.written = SERVO_CENTER;
    c.preparedUs = SERVO_CENTER;
    c.prepared = false;
    c.attached = false;
  }
  g_mask = 0;
  g_faultMask = 0;

  // Pads first: nothing may be driven while the peripheral is being set up.
  for (uint8_t i = 0; i < NUM_SERVOS; i++) padPark(i);

  ledc_timer_config_t t = {};
  t.speed_mode = SD_MODE;
  t.timer_num = SD_TIMER;
  t.duty_resolution = (ledc_timer_bit_t)LEDC_BITS;
  t.freq_hz = LEDC_HZ;
  t.clk_cfg = LEDC_AUTO_CLK;
  bool cfgOk = (ledc_timer_config(&t) == ESP_OK);

  // Every channel on the one timer, servo channels silent at duty 0. A channel
  // that failed to bind would sit on the reset-default timer, which is not this
  // one, so a bad return here has to block attaching rather than be discarded.
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    if (!channelConfig(LEDC_CH[i], 0)) cfgOk = false;
  }

  // Reference last so it is the signal left on the bind pad.
  if (!channelConfig(SD_REF_CHANNEL, usToDuty(SD_REF_US))) cfgOk = false;
  refPadInit();

  g_refOk = cfgOk && refProveAlive();
  g_refSawHigh = false;
  g_refSawLow = false;
  g_refTicks = 0;

  g_lastWriteMs = millis();
  g_begun = true;
  return g_refOk;
}

bool sdRefOk() { return g_refOk; }

void sdSetLimits(uint8_t ch, int hardMin, int hardMax, int softMin, int softMax) {
  if (ch >= NUM_SERVOS) return;
  SdChan& c = g_ch[ch];
  c.hardMin = hardMin;
  c.hardMax = hardMax;
  c.softMin = softMin;
  c.softMax = softMax;
  // A tightened limit must take effect on what is already in flight, not only
  // on the next request. The slew ceiling still governs how the emitted pulse
  // gets there. A pending prepared position is re-clamped too, otherwise a
  // channel prepared before the change would attach outside the new window.
  c.commanded = clampUs(ch, c.commanded);
  // The duty rewrite below must never reach a live pad. markAttached clears
  // prepared, so this is already unreachable while attached; the explicit test
  // keeps it that way if either invariant is ever edited.
  if (c.prepared && !c.attached) {
    const int reclamped = clampUs(ch, c.preparedUs);
    if (reclamped != c.preparedUs) {
      c.preparedUs = reclamped;
      setDuty(LEDC_CH[ch], usToDuty(c.preparedUs));
    }
  }
}

/*
  Refuses on an attached channel, and says so rather than failing silently.
  sdPrepare writes the duty directly, so on a live pad it would step the servo
  straight to the new position with no slew ceiling and no regard for the frame —
  the two things this module exists to enforce. An attached channel is moved with
  sdCommand and nothing else.
*/
SdResult sdPrepare(uint8_t ch, int us) {
  if (ch >= NUM_SERVOS || !g_begun) return SD_BAD_CH;
  SdChan& c = g_ch[ch];
  if (c.attached) return SD_ALREADY;
  c.preparedUs = clampUs(ch, us);
  setDuty(LEDC_CH[ch], usToDuty(c.preparedUs));
  c.prepared = true;
  return SD_OK;
}

bool sdPrepared(uint8_t ch) {
  return ch < NUM_SERVOS && g_ch[ch].prepared;
}

// The position a prepared channel would attach at. sdPrepared alone cannot tell
// "prepared at what I asked for" from "prepared earlier at something else", or
// from a value since re-clamped by sdSetLimits, so a caller that cares must
// compare against this rather than trust the flag.
int sdPreparedUs(uint8_t ch) {
  return ch < NUM_SERVOS ? g_ch[ch].preparedUs : 0;
}

bool sdDutyLive(uint8_t ch) {
  if (ch >= NUM_SERVOS || !g_ch[ch].prepared) return false;
  return liveDuty(LEDC_CH[ch]) == usToDuty(g_ch[ch].preparedUs);
}

// Everything a channel must satisfy before its pad may be touched.
static SdResult attachReady(uint8_t ch) {
  if (ch >= NUM_SERVOS || !g_begun) return SD_BAD_CH;
  if (g_ch[ch].attached) return SD_ALREADY;
  if (!g_ch[ch].prepared) return SD_NOT_PREPARED;
  if (!sdDutyLive(ch)) return SD_NOT_LIVE;
  return SD_OK;
}

// Assumes the caller has verified readiness and is inside the idle gap.
static void handOver(uint8_t ch) {
  const int pin = servoPin(ch);
  // Pre-set LOW so the instant the output driver enables, the pad is at the
  // idle level rather than fabricating an edge of its own.
  gpio_set_level((gpio_num_t)pin, 0);
  gpio_matrix_out(pin, ledcSig(LEDC_CH[ch]), false, false);
}

static void markAttached(uint8_t ch) {
  SdChan& c = g_ch[ch];
  c.attached = true;
  c.prepared = false;
  c.commanded = c.preparedUs;
  c.written = c.preparedUs;
  g_mask |= (uint8_t)(1u << ch);
}

SdResult sdAttach(uint8_t ch) {
  if (!g_refOk) return SD_NO_REF;
  SdResult r = attachReady(ch);
  if (r != SD_OK) return r;
  if (!waitForGapStart(SD_GAP_TIMEOUT_MS)) return SD_NO_GAP;

  // Re-check after the wait. Waiting for the gap takes about 10 ms, and a
  // check made before it is stale by the time the pad is touched.
  r = attachReady(ch);
  if (r != SD_OK) return r;

  // Bookkeeping inside the same critical section as the handover. Both are only
  // register and integer writes, and keeping them together means no observer can
  // ever see a routed pad that state claims is detached — which detach would
  // then decline to park and sdService would decline to slew.
  portENTER_CRITICAL(&g_mux);
  handOver(ch);
  markAttached(ch);
  portEXIT_CRITICAL(&g_mux);
  return SD_OK;
}

SdResult sdAttachGroup(const uint8_t* chs, uint8_t n) {
  if (!g_refOk) return SD_NO_REF;
  if (chs == nullptr || n == 0 || n > SD_MAX_GROUP) return SD_BAD_CH;

  // All-or-nothing: validate the whole group before touching any pad, so a
  // stage never ends up half attached.
  for (uint8_t k = 0; k < n; k++) {
    const SdResult r = attachReady(chs[k]);
    if (r != SD_OK) return r;
  }

  if (!waitForGapStart(SD_GAP_TIMEOUT_MS)) return SD_NO_GAP;

  // Re-validate after the wait: about 10 ms passed, and anything checked before
  // it is stale by the time a pad is touched.
  for (uint8_t k = 0; k < n; k++) {
    const SdResult r = attachReady(chs[k]);
    if (r != SD_OK) return r;
  }

  // One gap, one critical section, every pad in the group. This is what the
  // shared timer is for: the gap is common, so the handover is atomic with
  // respect to the frame.
  portENTER_CRITICAL(&g_mux);
  for (uint8_t k = 0; k < n; k++) {
    handOver(chs[k]);
    markAttached(chs[k]);
  }
  portEXIT_CRITICAL(&g_mux);
  return SD_OK;
}

SdResult sdAttachNow(uint8_t ch, int us) {
  if (ch >= NUM_SERVOS || !g_begun) return SD_BAD_CH;
  if (g_ch[ch].attached) return SD_ALREADY;
  const SdResult p = sdPrepare(ch, us);
  if (p != SD_OK) return p;

  // Bounded rather than a bare wait-until-true: nothing in the motion path may
  // spin forever, however unlikely the hardware fault.
  const uint32_t t0 = millis();
  while (!sdDutyLive(ch)) {
    if ((uint32_t)(millis() - t0) > SD_LIVE_TIMEOUT_MS) return SD_NOT_LIVE;
    delay(1);
  }
  return sdAttach(ch);
}

/*
  Hands the pad back to plain GPIO and parks it. The channel is deliberately
  left running at its current duty: it drives nothing once unrouted, and on the
  next attach an unchanged duty is already live, so the readback check passes
  immediately.

  Silencing the channel before the release would be actively wrong — setting
  duty 0 while the pad is still routed truncates the pulse in flight, which is
  the exact fault this module exists to prevent.
*/
static void releasePad(uint8_t ch) {
  // Park the pad before clearing the flags. The other order lets a concurrent
  // attach see "not attached" while the pad is still live and route it a second
  // time; this order makes it see ALREADY and decline, which is the safe answer.
  padPark(ch);
  g_ch[ch].attached = false;
  g_ch[ch].prepared = false;
  g_mask &= (uint8_t)~(1u << ch);
  g_faultMask &= (uint8_t)~(1u << ch);
}

/*
  Finds a gap to release in, preferring the reference and falling back to the
  pads being released.

  Release is timed off the reference because it is wider than any servo pulse and
  is the one signal under continuous supervision. But if the reference read is
  broken while the timer is still running, the servos are still being driven, and
  parking blind would cut a pulse in flight. So the pads themselves are tried
  next: reading a routed pad is direct and, unlike the old "currently LOW" test,
  a stuck read cannot fake a pass now that an edge is required.

  Returns false only when neither line shows a pulse, and the caller must then
  NOT park. See sdDetach.
*/
static bool findReleaseGap(const uint8_t* chs, uint8_t n) {
  if (waitForGapStart(SD_GAP_TIMEOUT_MS)) return true;
  for (uint8_t k = 0; k < n; k++) {
    if (chs[k] < NUM_SERVOS && g_ch[chs[k]].attached) {
      if (waitForGapStartOn(servoPin(chs[k]), SD_GAP_TIMEOUT_MS)) return true;
    }
  }
  return false;
}

/*
  Refuses to release when no gap can be found, and flags the channel instead.

  This reverses an earlier judgement of mine that was wrong. Parking anyway is
  not the safer end state. A pad left routed keeps receiving the last commanded
  pulse, so the servo holds position; parking it mid-pulse truncates that pulse,
  which is precisely the input that makes these servos slam to their minimum at
  full torque. On 150 kg servos an unowned hold beats a truncated pulse, so when
  the driver cannot prove the frame is idle it does nothing and says so.

  The physical e-stop on the supply remains the hard kill for this case.
*/
bool sdDetach(uint8_t ch) {
  if (ch >= NUM_SERVOS) return false;
  if (!g_ch[ch].attached) {
    // Prepared but never attached: drop the reservation so a later attach
    // cannot succeed on a duty nobody asked for any more.
    g_ch[ch].prepared = false;
    return true;
  }
  if (!findReleaseGap(&ch, 1)) {
    g_faultMask |= (uint8_t)(1u << ch);
    return false;
  }
  releasePad(ch);
  return true;
}

bool sdDetachGroup(const uint8_t* chs, uint8_t n) {
  if (chs == nullptr || n == 0 || n > SD_MAX_GROUP) return false;
  bool any = false;
  for (uint8_t k = 0; k < n; k++) {
    if (chs[k] < NUM_SERVOS && g_ch[chs[k]].attached) any = true;
  }
  if (!any) {
    for (uint8_t k = 0; k < n; k++) {
      if (chs[k] < NUM_SERVOS) g_ch[chs[k]].prepared = false;
    }
    return true;
  }

  // Shared timer, shared gap: wait once, then release the whole group.
  if (!findReleaseGap(chs, n)) {
    for (uint8_t k = 0; k < n; k++) {
      if (chs[k] < NUM_SERVOS && g_ch[chs[k]].attached) {
        g_faultMask |= (uint8_t)(1u << chs[k]);
      }
    }
    return false;
  }
  for (uint8_t k = 0; k < n; k++) {
    if (chs[k] < NUM_SERVOS && g_ch[chs[k]].attached) releasePad(chs[k]);
  }
  return true;
}

bool sdDetachAll() {
  uint8_t all[NUM_SERVOS];
  for (uint8_t i = 0; i < NUM_SERVOS; i++) all[i] = i;
  return sdDetachGroup(all, NUM_SERVOS);
}

uint8_t sdFaultMask() { return g_faultMask; }

bool sdAttached(uint8_t ch) {
  return ch < NUM_SERVOS && g_ch[ch].attached;
}

uint8_t sdAttachMask() { return g_mask; }

void sdCommand(uint8_t ch, int us) {
  if (ch >= NUM_SERVOS) return;
  g_ch[ch].commanded = clampUs(ch, us);
}

int sdCommanded(uint8_t ch) {
  return (ch < NUM_SERVOS) ? g_ch[ch].commanded : 0;
}

int sdWritten(uint8_t ch) {
  return (ch < NUM_SERVOS) ? g_ch[ch].written : 0;
}

// A detached channel is settled by definition: nothing is being emitted, so
// there is nothing to catch up. Reporting false forever would stall any caller
// that waits on this before releasing or advancing a ramp.
bool sdSettled(uint8_t ch) {
  if (ch >= NUM_SERVOS) return true;
  if (!g_ch[ch].attached) return true;
  return g_ch[ch].written == g_ch[ch].commanded;
}

bool sdAllSettled() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    if (g_ch[i].attached && !sdSettled(i)) return false;
  }
  return true;
}

// One sample per tick, accumulated. A reference that has stopped toggling makes
// the gap test meaningless, so it is treated as a loss of the safety property
// rather than a cosmetic fault.
static void superviseRef() {
  if (gpio_get_level((gpio_num_t)PIN_LEDC_BIND)) g_refSawHigh = true;
  else g_refSawLow = true;

  if (++g_refTicks < SD_REF_WINDOW_TICKS) return;
  g_refOk = g_refSawHigh && g_refSawLow &&
            (liveDuty(SD_REF_CHANNEL) == usToDuty(SD_REF_US));
  g_refSawHigh = false;
  g_refSawLow = false;
  g_refTicks = 0;
}

/*
  The only place a servo pulse changes. One write per frame, because the
  hardware latches once per frame anyway, and never further than
  PULSE_MAX_STEP_US.

  The step ceiling is what makes a wrong command survivable. A truncated-pulse
  command, or a ramp catching up after a stalled loop, arrives here as a large
  jump and leaves as a controlled ramp an operator can interrupt.
*/
void sdService() {
  if (!g_begun) return;
  const uint32_t now = millis();
  if ((uint32_t)(now - g_lastWriteMs) < SD_FRAME_MS) return;
  g_lastWriteMs = now;

  superviseRef();

  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    SdChan& c = g_ch[i];
    if (!c.attached) continue;

    // Clamp the goal, then slew toward it. Clamping after adding the step would
    // let a tightened limit yank the pulse into range in one frame, which is
    // exactly the large uncontrolled move the ceiling exists to prevent.
    const int goal = clampUs(i, c.commanded);
    int step = goal - c.written;
    if (step == 0) continue;
    if (step > PULSE_MAX_STEP_US) step = PULSE_MAX_STEP_US;
    else if (step < -PULSE_MAX_STEP_US) step = -PULSE_MAX_STEP_US;

    c.written += step;
    setDuty(LEDC_CH[i], usToDuty(c.written));
  }
}
