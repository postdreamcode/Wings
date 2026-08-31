# Wings — Handoff

**State as of 2026-08-31, commit `436c37d` on `main`, pushed to `github.com/postdreamcode/Wings` (private).**

The motion stack has been rewritten around a new `servo_driver` module. The driver
is hardware-validated in isolation. The rewired firmware builds clean but **has
not been run on hardware**. That bench run is the next task.

---

## 1. What this project is

A wearable wing rig. An ESP32-S3 Mini drives five high-torque RC servos:

| idx | name | pin | role |
|---|---|---|---|
| 0 | `CH_WRIST2` | GPIO5 | wrist hug |
| 1 | `CH_WRIST1` | GPIO4 | wrist raise |
| 2 | `CH_ELBOW2` | GPIO3 | elbow hug |
| 3 | `CH_ELBOW1` | GPIO2 | elbow raise |
| 4 | `CH_SHOULDER` | GPIO1 | shoulder |

Three are 150 kg·cm, two are 35 kg·cm. Commands arrive from a 433 MHz RF fob, a
BLE GATT app, voice, a serial console, and ESP-NOW between the two wings. Servos
are attached only while moving and released afterward, so the internal brake
holds position with no pulses.

**This machine can injure the person wearing it.** Unintended motion is not a
cosmetic bug. Every design decision below trades convenience for the property
that a wrong command produces a *controlled* move, or no move, but never a
full-torque slam.

Build: `cd firmware; pio run -e esp32-s3-mini` (PowerShell — Windows host, use
`;` not `&&`).

---

## 2. The fault this all exists to fix

Reported symptoms, in Ken's words:

1. **Extra extreme motion *after* a move finishes.** Random, any servo, more
   likely at higher speed.
2. **Occasionally a jump at the *start* of a move that self-corrects quickly.**

Reproduced over the RF fob as well as BLE, so it was never a transport problem.

### What was measured on this board

These are measurements from the `padtest/` and `drivertest/` harnesses, not
theory:

- **LEDC channels are not phase-aligned to the moment `gpio_matrix_out` runs.**
  Connecting a servo pad at an arbitrary instant hands the servo whatever remains
  of the pulse in flight. Remainders as short as **18 µs** were observed. About
  **4.5%** of unaligned connects truncate; roughly **2%** land in the 500–900 µs
  band — and a 500–900 µs pulse is not noise, it is a *valid command* to a
  position far below target. That is symptom 2.
- **Handing over only while the frame reads LOW eliminated it.** 2000 connects,
  zero bad pulses, shortest first pulse 1489 µs.
- **A duty write does not take effect until the frame latches.** Connecting
  sooner emits the channel's *previous* duty as the servo's first pulse.
- **A pad released with `GPIO_OUT` latched HIGH and no pull floats at 3.3 V for
  43–45 ms** before decaying through the logic threshold. The old firmware's
  connects happened inside that window, so they were benign *by accident*.
- **The settle defect.** `armGroupHold()` stamped the release deadline when the
  *attach* completed, and the ramp duration almost always exceeds the 250 ms
  hold, so the dwell was already expired when the move finished. The release then
  fired one service iteration after the final duty write — a few milliseconds,
  less than one 20 ms frame. **The servo could lose the pad before ever receiving
  a pulse at the target position.** That is the best explanation for symptom 1.

A historical note that matters: an earlier fix latched released pads HIGH to stop
an occasional slam during arming, and it worked. It worked because a floating-HIGH
line masks a truncated connect. The driver removes the underlying cause, so the
workaround is gone by construction — pads now rest at 0 V. **The two changes
belong together.** Resting a pad LOW without gap-aligned connects would be worse
than either.

---

## 3. The driver

`firmware/include/servo_driver.h` + `firmware/src/servo_driver.cpp`.

Both files are heavily commented with the reasoning and the measurements. Read
the header first — it is the specification.

### The one rule

> **`servo_driver.cpp` is the only code permitted to touch a servo pad, an LEDC
> register, or the GPIO matrix.**

Everything above it asks for a position and is *refused* if the request cannot be
met safely. `servos.cpp` no longer contains `ledcWrite`, `ledcSetup`,
`gpio_matrix_out`, a duty conversion, or an LEDC channel map. Do not reintroduce
any of them.

### Single shared timer

All five servo channels plus a phase reference sit on **one** timer
(`LEDC_TIMER_0`, low-speed mode, 50 Hz, 12-bit). This is deliberate.

The chip has four LEDC timers and eight low-speed channels, so five servos cannot
each own a timer. Sharing costs nothing: a timer sets only period and resolution
while every channel keeps an independent duty register. Two servos on one timer
get identical 20 ms frames and completely independent pulse widths.

Sharing buys three things:

- **One timer to configure, configured once** before any pad can connect. Nothing
  can later reconfigure a timer that is already driving a servo.
- **One common idle gap**, so a whole stage of servos can be handed over inside
  the same gap in a single critical section — atomic with respect to the frame,
  and one wait instead of three.
- **A benign failure mode**: if the timer stops, all five lose their pulses at
  once, and a servo with no pulses holds position.

Arduino's `ledcSetup` derives `timer = (chan/2)%4` and would scatter these across
three timers. That is why the module calls the IDF functions directly and names
the timer explicitly. (The pre-rewrite firmware was worse than scattered:
`ledcSetup` does not bind a channel to a timer at all, and `ledcAttachPin` had
been dropped, so channels sat on reset defaults.)

### The phase reference

A spare LEDC channel (`SD_REF_CHANNEL` = 7) sits permanently on
`PIN_LEDC_BIND` (**GPIO21**) at `SD_REF_US`. **This is the only signal ever read
to decide whether a handover is safe.** Servo channels are never routed anywhere
except their own pad, at attach time, in the gap.

Two constraints that are easy to break:

- `SD_REF_US` is `SERVO_ABS_MAX` (2500 µs), not something convenient. Channels on
  a shared timer all go HIGH at the counter reset and LOW at their own duty, so
  the reference must be **at least as wide as the widest possible servo pulse**
  for "reference is LOW" to prove every servo channel is also LOW.
- The reference pad is configured `GPIO_MODE_INPUT_OUTPUT`.
  `ledc_channel_config` leaves a pad output-only, and `gpio_get_level` on an
  output-only pad reads a constant — which would make the gap test lie while every
  test still passed.

The reference is **supervised continuously**. If it stops toggling, the read that
decides "safe to hand over" becomes a constant and the entire safety property
silently evaporates — so every attach is refused (`SD_NO_REF`) until it is proven
alive again.

### The handover primitive

Attach, in order:

1. `sdPrepare(ch, us)` — clamp, write the duty. Non-blocking. Refuses on an
   attached channel (`SD_ALREADY`): writing a duty directly to a live pad would
   step the servo with no slew ceiling and no regard for the frame.
2. `sdDutyLive(ch)` — ask the **hardware** whether it is actually emitting that
   duty yet. Reads `ledc_get_duty`, which returns `duty_rd`, the latched value —
   so it is a real check, not an echo of the write.
3. `sdAttach(ch)` / `sdAttachGroup(chs, n)` — validate, **wait for a falling edge
   on the reference**, re-validate, then hand over inside a critical section.

`waitForGapStartOn` requires a **rising then falling edge** with a minimum HIGH
dwell (`SD_MIN_HIGH_US` = 1000). "Currently LOW" is not sufficient, for two
independent reasons:

- A line stuck LOW (dead channel, unrouted pad, input buffer off) satisfies it
  instantly. The safety property would evaporate while every test passed. This is
  not hypothetical — it was verified on the bench that a deliberately-killed
  reference reads as a clean gap to a naive level check.
- A sample taken in the last microseconds of a gap, followed by a few hundred
  microseconds of interrupt before the matrix write, walks the handover into the
  next pulse and straight into the slam band.

Requiring the falling edge proves the line is pulsing *now* and leaves the full
idle gap ahead of the handover.

The handover itself pre-sets `GPIO_OUT` LOW so the instant the output driver
enables, the pad is at the idle level rather than fabricating an edge. Attach
bookkeeping happens **inside the same critical section** as the matrix write, so
no observer can see a routed pad that state claims is detached.

### Release, and the refusal that matters

`sdDetach` finds a gap — preferring the reference, falling back to the pads being
released — and parks the pad (input, pull-down enabled, `GPIO_OUT` pre-set LOW).

**If no gap can be found it returns `false`, leaves the pad ROUTED, and sets a bit
in `sdFaultMask()`.**

This reverses an earlier judgement that was wrong. Parking anyway is *not* the
safer end state. A routed pad keeps repeating the last commanded pulse, so the
servo holds position. Parking mid-pulse truncates that pulse, which is precisely
the input that makes these servos slam to minimum at full torque. On a 150 kg
servo an unowned hold beats a truncated pulse.

**A caller that gets `false` must surface it.** The pad is still driving and only
the supply e-stop will stop it. `servos.cpp` prints loudly, keeps `g_armed` true,
and reports `DISARM INCOMPLETE` / `STOP INCOMPLETE`.

### The slew ceiling

`PULSE_MAX_STEP_US` = 60. `sdService()` is the **only** place a servo pulse
changes — one write per frame, never further than 60 µs.

`cosineDurMs` is built so the cosine's *peak* rate equals `cruiseUsPerSec`. At
100% speed that is ~1481 µs/s ≈ **30 µs per frame**, so 60 leaves 2× headroom and
throttles nothing legitimate (jog and flap are both below it). **Do not raise
it.** Its purpose is to convert any wild step — a stalled ramp catching up, a
truncated-pulse command — into a controlled ramp an operator can STOP.

`sdService` clamps the *goal* before computing the step. Clamping after adding
the step would let a tightened limit yank the pulse into range in one frame,
which is the exact uncontrolled move the ceiling exists to prevent.

### `sdService()` must be called every loop

Not every frame — **every loop iteration**. `servosService()` calls it first,
unconditionally, before its own cadence gate. Reference supervision samples the
reference pad on every call and must sample far faster than the 20 ms frame it is
watching. Gating it to once per frame is a real bug that has already been made
twice; see §6.

### Commanded vs emitted

This distinction did not exist before and now matters everywhere:

- `sdCommanded(ch)` / `g_actual[ch]` in `servos.cpp` — what was **asked for**.
- `sdWritten(ch)` — what the servo has actually been **given**.
- `sdSettled(ch)` — the two agree; the driver has caught up.

They differ whenever the driver is slewing. Anything that means "where the servo
is" must read `sdWritten`. In particular `g_actual` is persisted to NVS as
*lastcmd* and reused as the position a servo is attached at next time, so a wrong
`g_actual` causes **real motion on the next arm**.

### API summary

| call | contract |
|---|---|
| `sdBegin()` | Config timer + channels, park all pads, prove the reference alive. `false` ⇒ nothing may attach. Idempotent; aborts re-init if release is unsafe. |
| `sdSetLimits(ch, hardMin, hardMax, softMin, softMax)` | Push calibration down. Re-clamps `commanded` and a pending `preparedUs`. |
| `sdPrepare(ch, us)` | Write duty, non-blocking. `SD_ALREADY` if attached. |
| `sdDutyLive(ch)` / `sdPreparedUs(ch)` | Hardware emitting it yet / what it would attach at. |
| `sdAttach(ch)` / `sdAttachGroup(chs, n)` | Gap-aligned handover. Group is all-or-nothing. |
| `sdAttachNow(ch, us)` | Prepare + wait + attach. Blocks ≤ `SD_LIVE_TIMEOUT_MS`. |
| `sdDetach(ch)` / `sdDetachGroup` / `sdDetachAll` | `false` ⇒ pad left routed, `sdFaultMask()` set. **Must be surfaced.** |
| `sdCommand(ch, us)` | Request a position. Clamped, slewed. |
| `sdWritten` / `sdSettled` / `sdAllSettled` | What the servo got; whether the driver caught up. |
| `sdService()` | Emit + supervise. **Every loop.** |

`SdResult`: `SD_OK`, `SD_ALREADY`, `SD_BAD_CH`, `SD_NOT_PREPARED`, `SD_NOT_LIVE`,
`SD_NO_GAP`, `SD_NO_REF`. `sdResultName()` gives the string.

---

## 4. What the rewire changed in `servos.cpp`

`servos.cpp` keeps poses, calibration, path/stage sequencing, the cosine motion
profile, and command handling. It owns *what* should happen; the driver owns
*whether it is safe*.

Deleted: `holdLedcUs`, `connectServoPad`, `releasePins`, `latchHigh`, `pwmWrite`,
`usToDuty`, `g_ledcOn`, the local `LEDC_CH` map, `firstPulseFragment`.

Behavioural changes worth knowing before editing:

- **Arrival reads `sdWritten`.** `chNeedsMove` uses the driver's emitted value
  when attached, `g_actual` when detached (correct — that is lastcmd).
- **Every release adopts `sdWritten` first.** `detachOne`, `detachAll`, and
  `servosStop` snap `g_actual` to what was emitted before letting go. Without
  this, releasing mid-move records a position the horn never reached.
- **The dwell is stamped at arrival**, after `groupArrived` *and* `groupSettled` —
  this is the settle fix.
- **The ramp clock holds while the driver catches up.** `serviceCosineRamp` pushes
  `g_rampT0` forward by the skipped interval rather than re-basing it, so the ease
  keeps its shape instead of restarting.
- **The attach mask is not mirrored.** `sdAttached` / `sdAttachMask` are the single
  source of truth. `g_armed` is **derived**, never asserted — including in
  `abortPath`, `servosDisarm`, and `servosStop`. A refused release therefore
  correctly keeps reporting armed.
- **Stages attach as a unit** via `attachStageTogether` → `sdAttachGroup`. If the
  group is refused, the path **aborts**; it does not fall back to per-servo
  attach. A partially attached stage can never report arrival, because the ramp
  only writes attached channels — the wing would sit parked mid-pose with pads
  driving until STOP. A strict "every member live or abort" guard backs this up
  and also covers the staggered D/home path. The flap has the same guard.
- **The lastcmd NVS write left the motion path.** `g_lastCmdDirty` is set on
  detach and flushed by `serviceLastCmdSave()` once no pad is live and no path is
  running.
- **Limits reach the driver** through `servosClampSoftInsideHard` (every setter
  funnels through it) plus `servosApplyDefaults`, `servosAuditEnvelope` (the
  post-NVS-load hook — `store.cpp` writes `g_cal` through the `servosCal()`
  reference and bypasses the setters), and `servosBegin`.
- **The trace ring was repurposed.** The first-pulse fragment probe is gone
  (short pulses are impossible by construction now). The sticky list holds
  `REFUSE` rows — what the driver declined and why — and the dump reports `refOk`
  and `faultMask`. Dump it with the serial `trace` command.

---

## 5. Verified vs not verified

**Verified on hardware** (Slave board, wrist hug + wrist raise, rail live), via
`drivertest/`:

- 2000 single-channel attach/detach cycles: **zero** short first pulses. First
  pulse 1546–1548 µs against a prepared 1550 µs — the 2 µs is 12-bit duty
  quantisation, and there was zero variance across the whole soak.
- 500 two-channel group cycles: zero truncations, **worst rise skew 0 µs**. This
  is the load-bearing measurement: it proves both pads go HIGH on the same counter
  reset, which is the premise the shared-timer design rests on and what makes a
  one-gap group handover meaningful.
- Slew ceiling exact: 60 µs max step, 5 steps for a 250 µs move.
- Dead reference: attach refused `NO_GAP` with the pad forced LOW — exactly the
  case a naive level check would have called a clean gap. Supervision caught it
  within one window; `sdBegin` recovered.
- Limit clamping and `preparedUs` re-clamping hold.
- `faultMask` 0x00 throughout, no refused detaches.

**Verified by build/review only:**

- The entire rewire (commits `d7e77d5`, `436c37d`). Clean full rebuild, zero
  warnings, no lints. **Never executed.**
- The detach-refusal branch (`sdFaultMask`). Dropped deliberately: forcing it
  requires stopping the LEDC peripheral with pads live, which only happens if the
  peripheral has died. Too rare to justify a rig; small enough to review
  statically. **Ken's explicit call — do not reopen this without asking.**

**Weight hardware evidence over static review.** Two of the three real driver
defects found so far were in code that earlier audits had explicitly passed.

---

## 6. Defects found so far, as a warning

Recording these because the same mistakes are easy to repeat:

1. **`ledcSetup` never bound channels to a timer.** Pre-rewrite. Channels sat on
   reset defaults.
2. **Supervision aliased against the frame it watched.** Sampled once per 20 ms
   while the reference is HIGH for only 2500 of every 20000 µs, so it landed at
   nearly the same phase every time and missed the HIGH indefinitely. Declared a
   healthy reference dead and refused every attach, forever. *Both audits passed
   this code.*
3. **`refProveAlive` checked the live duty before the frame could latch**,
   violating the driver's own documented rule, so `sdBegin` always returned false.
   Fixed with `refWaitLatched`.
4. **Supervision used a tumbling window**, so the window straddling a failure still
   held pre-failure evidence and reported healthy for up to two windows. Replaced
   with a staleness check — latency is now exactly one window, deterministic.
5. **Detach parked blind after a failed gap wait.** A real slam path. Now refuses
   and flags.
6. **Group-attach failure fell back to staggered attach**, allowing a partially
   live stage that could never arrive. Introduced during the rewire, caught by
   audit.

---

## 7. Remaining work

### NEXT — bench the rewired firmware

The audit's call, which I would follow: **fixture, supply e-stop in hand, not
worn, success path only.** Wrist raise and wrist hug are the servos Ken has made
safe to move.

Flash `firmware/`, then:

1. **At boot**, confirm the driver came up: no `SERVO DRIVER REFUSED TO START`,
   and `trace` reports `refOk=1 faultMask=0x00`.
2. **Arm one servo**, then run one open/close.
3. **Dump `trace`** and check:
   - every `ATTACH` row has `detail 0` (landed in the gap),
   - **no `REFUSE` rows**,
   - `DETACH` rows show a *healthy positive* dwell remaining — **this is the
     settle fix**; it used to be expired or negative,
   - no `JUMP` rows, no `STALL` rows.
4. Repeat at higher speed, since the symptom was speed-correlated.

Note `drivertest/` is currently the harness that was last flashed to the board;
the main firmware needs flashing over it.

### Then, in priority order

| id | item |
|---|---|
| `p2-blecore` | **Top structural risk.** BLE motion runs on the NimBLE host task on the *other core* (`ble.cpp` → `buttonDispatch` → `servosHandleCmd`) while `servosService` runs on the loop task. ESP-NOW already queues correctly via `nowService`; BLE does not, and the comment in `ble.cpp` claiming they are the same is false. `attachStageTogether`'s prepare → latch-wait → handover is a multi-step sequence with only the handover under `portMUX`, so a BLE command can land inside it. |
| `p2-queue` | The fix for the above, and Ken's stated principle: **every command source collapses to the same minimal internal signal, and the motion queue holds an enum only** — no transport ever passes data into motion. Verified against the command set: all production motion commands (`ARM`/`DISARM`/`HOME`/`TOGGLE_WING`/`HUG`/`SEQ`/`POSE_*`/`STOP`) are pure triggers. Exceptions are *config*, not motion (`SET_SPEED`, `SET_CH_SPEED`, `TEACH_POSE`, `SET_SENSE`), plus `JOG`, which carries data but is confined to setup mode. `SET_TARGETS` is already local-only and refused over BLE. |
| `p2-gate` | `motionBusy()` gate on all motion entry points. STOP and DISARM always pass. |
| `p2-stop` | Controlled-halt STOP: freeze ramp, settle, safe release; clear `g_poseKnown` so the pose must be re-established. Partly enabled already — STOP now halts at the emitted position. |
| `p2-abort` | `abortPath` should mark attached channels for settle-then-release so no pad is left pulsing unmanaged. Partly addressed (the new stage/flap aborts release explicitly) but `servosJog` and `servosDisarmCh` still call `abortPath` without releasing the rest of a stage. Pre-existing. |
| `p2-fob` | Split fob debounce from same-code repeat suppression (`FOB_REPEAT_MS`). |
| `p3-armseq` | **The feature Ken originally asked for**: master arm sequence. One button arms shoulder → elbow raise → elbow hug → wrist raise → wrist hug with an explicit `ARM_SEQ_GAP_MS` between each. Expose `CMD_ARM_ALL` to fob and BLE, abort on STOP or attach refusal, gate behind `motionBusy()`. Ken was explicit that safety had to be solid first. |

### Known-and-accepted, do not "fix" without asking

- **`accel` and `invert` are dead knobs**, deliberately neutered rather than
  removed: the command/help/status surface is gone, but `ChannelCal.invert`, the
  NVS `reserved[1..2]` round-trip, the BLE `accelMs` status field, and the
  `CMD_SET_ACCEL` enum id are all **kept on purpose** so NVS layout and app
  compatibility are unaffected. `storeLoad` gates on `sizeof(PersistData)`, so
  changing the struct resets calibration to defaults.
- **`FIRMWARE_VERSION` is still `0.2.67`** and probably should be bumped, but the
  app may key off it — left alone deliberately.
- **The 0.1% mid-pulse connect** (a benign 1490 µs first pulse) is an accepted
  residual, taken to avoid a 32 ms blocking wait per connect.
- **The stage latch-wait spins without yielding**, documented in place: the duty
  register is copied by hardware at the frame boundary so `sdService` is not
  needed, and yielding would widen the cross-core window. Revisit only as part of
  `p2-queue`.

---

## 8. Rules for whoever picks this up

**Safety**

- Never drive a servo pin without a verified-dead rail, or a fixture with the
  supply e-stop in reach. USB alone can backfeed the 6 V supply enough to move a
  servo with the 12 V supply off — this was observed.
- Do not flash or run motion tests without confirming with Ken which servos are
  physically safe to move.
- If `sdDetach` returns false anywhere, the servo is still driven. Cutting the
  supply is the only stop.

**Code**

- Nothing outside `servo_driver.cpp` touches a pad, an LEDC register, or the GPIO
  matrix.
- Do not raise `PULSE_MAX_STEP_US`.
- Do not make `sdService` conditional or move it behind a cadence gate.
- Anything meaning "where the servo is" reads `sdWritten`, not `g_actual`.
- A refused handover is the driver working correctly. Surface it; do not retry
  blind or fall back to a less safe path.
- Prefer the smallest change. Say what layer you are changing — UI, pipeline math,
  per-channel calibration, or global config — and ask if more than one could be
  meant.

**Process**

- Ken asks for a short question naming the readings rather than a guess, whenever
  a wrong guess would be expensive.
- Do not claim work is done from intent. Report what was actually run.
- Non-trivial work gets a supervisory audit subagent on an included Cursor model
  (Cursor Grok, or Composer for narrow mechanical subtasks) — never the parent
  model, and never `inherit`. The audit on `d7e77d5` found a real must-fix; it is
  worth doing.

---

## 9. Repo map

```
firmware/     the real firmware — flash this
  include/servo_driver.h    the specification; read this first
  src/servo_driver.cpp      the only code allowed near a pad
  src/servos.cpp            poses, stages, cosine profile, commands
  include/Config.h          pins, channel map, limits, motion constants
drivertest/   standalone bench harness for the driver (compiles the REAL
              servo_driver.cpp, not a copy, to prevent test drift)
padtest/      earlier pad-transition probes; the source of the measurements
app/          BLE app
```

Commit history, most recent first:

```
436c37d  Close the partial-attach wedge and the rest of the audit findings
d7e77d5  Rewire servos.cpp onto servo_driver; fix the settle defect
69ad32b  Harden reference supervision; validate group attach on hardware
29e6b5e  Fix two servo_driver defects that made every attach impossible
13b10ac  Add drivertest: hardware bench harness for servo_driver
9cd5fa4  Baseline: firmware 0.2.67, before the servo driver rewire
```
