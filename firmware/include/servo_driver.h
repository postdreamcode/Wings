#pragma once

/*
  Wings servo driver — the only code permitted to touch a servo pad or an LEDC
  register. Everything above it asks for a position and is refused if the
  request cannot be met safely.

  Why this layer exists, in measured terms rather than theory. All of the
  following was measured on this board:

  - LEDC channels are not phase-aligned to the moment gpio_matrix_out runs, so
    connecting a pad at an arbitrary instant hands the servo whatever remains
    of the pulse in flight. Remainders as short as 18 us were observed, and a
    remainder in the 500-900 us band is a valid command to a position far below
    target. About 4.5% of unaligned connects truncate; roughly 2% land in that
    dangerous band.
  - Handing over only while the frame reads LOW eliminated it: 2000 connects
    with zero bad pulses, shortest first pulse 1489 us. Releasing in the gap:
    1000 releases, zero truncations.
  - A duty write does not take effect until the frame latches. Connecting
    sooner emits the channel's PREVIOUS duty as the servo's first pulse.
  - A pad released with GPIO_OUT latched HIGH and no pull floats at 3.3 V for
    43-45 ms before decaying. Pads here rest at 0 V instead.

  ------------------------------------------------------------------ layout

  All five servo channels share ONE timer, deliberately.

  The chip has four LEDC timers and eight channels, low-speed group only, so
  five servos cannot each have their own timer. Sharing costs nothing: a timer
  sets only the period and resolution, while every channel keeps an independent
  duty register. Two servos on one timer get identical 20 ms frames and
  completely independent pulse widths.

  Putting all five on one timer buys three things:

    - One timer to configure, configured once before any pad can be connected.
      Nothing can later reconfigure a timer that is already driving a servo.
      (Arduino's ledcSetup derives timer = (chan/2)%4 and would scatter these
      across three timers, which is why this module uses the IDF calls and
      picks the timer explicitly.)
    - One common idle gap, so a whole stage of servos can be handed over inside
      the same gap in a single critical section — atomic with respect to the
      frame, and one wait instead of three.
    - A benign failure mode: if the timer stops, all five lose their pulses at
      once, and a servo with no pulses holds position.

  ------------------------------------------------------- the phase reference

  A spare channel sits permanently on the bind pad at SD_REF_US, and is the
  only thing ever read to decide whether a handover is safe. Servo channels are
  never routed anywhere except their own pad, at attach time, in the gap.

  Channels on a shared timer all go HIGH at the counter reset and LOW at their
  own duty, so the reference must be at least as wide as the widest possible
  servo pulse for "reference is LOW" to prove every servo channel is also LOW.
  That is why SD_REF_US is SERVO_ABS_MAX rather than something convenient.

  The reference is supervised continuously. If it ever stops toggling, the read
  that decides "safe to hand over" would be a constant and the whole safety
  property would silently evaporate — so instead every attach is refused until
  the reference is proven alive again.
*/

#include <Arduino.h>
#include "Config.h"

/*
  Per-frame ceiling on how far the emitted pulse may move.

  cosineDurMs is built so the cosine's PEAK rate equals cruiseUsPerSec. At
  100% speed that is the servo's derated capability, about 1481 us/s, which is
  ~30 us per 20 ms frame. 60 us leaves 2x headroom for loop jitter while still
  converting any wild step — a stalled ramp catching up, or a truncated-pulse
  command — into a controlled ramp the operator can STOP.
*/
#define PULSE_MAX_STEP_US 60

#define SD_FRAME_MS 20            // one LEDC frame
// Handovers wait for a falling edge, so the worst case is most of one frame to
// see the pulse start plus its width to see it end: about 10 ms typical, 20 ms
// worst case, paid once per stage rather than once per servo. The budget is two
// frames plus slack so that losing one pulse to preemption costs a retry of the
// wait rather than a refused attach.
#define SD_GAP_TIMEOUT_MS 50
// Shortest HIGH accepted as a real pulse rather than a coupled transient. Any
// genuine pulse on these lines is at least SERVO_ABS_MIN wide.
#define SD_MIN_HIGH_US 1000
#define SD_LIVE_TIMEOUT_MS 80     // longest wait for a duty to go live
#define SD_MAX_GROUP NUM_SERVOS

// Spare LEDC channel used only as the phase reference. Servo channels are
// {5,6,2,3,4}, so 0, 1 and 7 are free.
#define SD_REF_CHANNEL 7

// Must be >= the widest servo pulse, or a LOW reference would not prove that
// every servo channel is also LOW.
#define SD_REF_US SERVO_ABS_MAX

enum SdResult : uint8_t {
  SD_OK = 0,
  SD_ALREADY,        // already attached; nothing done
  SD_BAD_CH,
  SD_NOT_PREPARED,   // sdPrepare was never called for this channel
  SD_NOT_LIVE,       // hardware is not yet emitting the prepared duty
  SD_NO_GAP,         // no pulse edge seen: refused rather than connect blind
  SD_NO_REF,         // phase reference is not proven alive; nothing may attach
};

const char* sdResultName(SdResult r);

// False if the peripheral could not be set up or the phase reference is not
// toggling. Nothing may attach in that state, so a false return is worth
// surfacing rather than discarding.
bool sdBegin();
bool sdRefOk();                    // phase reference proven alive

// Operating limits, pushed in by the layer that owns calibration. No pulse
// outside them is ever emitted, whatever is commanded.
void sdSetLimits(uint8_t ch, int hardMin, int hardMax, int softMin, int softMax);

/*
  Attaching is two steps so a stage does not pay the duty-latch wait per servo:
  sdPrepare writes the duty without blocking, sdDutyLive asks the hardware
  whether it is actually emitting it yet, and sdAttach performs the handover.
  Prepare every channel in a stage, then attach them together.

  The prepared value IS the position the servo wakes up holding, and attaching
  adopts it as the commanded position. A prior sdCommand on a detached channel
  does not survive: prepare last, and prepare the position you actually want.
  sdPrepare refuses on an attached channel — use sdCommand to move a live servo.
*/
SdResult sdPrepare(uint8_t ch, int us);
bool sdPrepared(uint8_t ch);
int  sdPreparedUs(uint8_t ch);     // compare against this, not sdPrepared alone
bool sdDutyLive(uint8_t ch);       // hardware is emitting the prepared duty
SdResult sdAttach(uint8_t ch);

// All-or-nothing handover of several pads inside one idle gap. Refuses the
// whole group if any channel is not ready, because a partly attached stage
// mid-move is worse than a refused one.
SdResult sdAttachGroup(const uint8_t* chs, uint8_t n);

// Prepare, wait for the duty to go live, then attach. Blocks up to
// SD_LIVE_TIMEOUT_MS. For callers not yet restructured around sdPrepare.
SdResult sdAttachNow(uint8_t ch, int us);

/*
  Release, timed so no pulse is cut short. Returns false when the driver could
  not prove the frame was idle, in which case the pad is deliberately LEFT
  ROUTED and the channel is flagged in sdFaultMask.

  That is the safe answer, not a cop-out: a routed pad keeps repeating the last
  commanded pulse and the servo holds position, whereas parking it mid-pulse
  produces the truncated pulse that makes these servos slam to minimum at full
  torque. A caller that gets false must surface it — the pad is still live and
  only the supply e-stop will stop it.
*/
bool sdDetach(uint8_t ch);
bool sdDetachGroup(const uint8_t* chs, uint8_t n);   // one gap, one handover
bool sdDetachAll();

bool sdAttached(uint8_t ch);
uint8_t sdAttachMask();
uint8_t sdFaultMask();             // channels left routed because release was unsafe

/*
  Request a position. The driver clamps it to the configured limits and moves
  the emitted pulse toward it no faster than PULSE_MAX_STEP_US per frame.
  sdWritten is what the servo has actually been given; while sdSettled is false
  the driver is still catching up and a ramp above should wait rather than run
  ahead of it.
*/
void sdCommand(uint8_t ch, int us);
int  sdCommanded(uint8_t ch);
int  sdWritten(uint8_t ch);
bool sdSettled(uint8_t ch);
bool sdAllSettled();

// Emits pending pulses on a 20 ms cadence and supervises the phase reference.
// Must be called every loop.
void sdService();
