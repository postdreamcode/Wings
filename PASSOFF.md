# Wings passoff — 2026-08-21

For a **new agent**. Do not trust prior chat conclusions. Read this, then the files. Ken is done with attach experiments that break SETUP or slam raises.

**Host:** Windows. PowerShell. Firmware: `d:\Personal\Wings\firmware` (`pio`). App: `d:\Personal\Wings\app` (Flutter `C:\Users\kenpo\flutter\bin\flutter.bat`).

**Rule:** `.cursor/rules/no-servo-slam.mdc` (always apply). Skill: `arduino-embedded-control`. Spawn audits with **Cursor Grok** (`cursor-grok-4.6-high-fast`), not the parent model.

**Do not** rewrite live NVS / taught poses / soft limits without an explicit yes. List what would be overwritten and wait.

---

## Hardware (do not invent)

- ESP32-S3 Mini, one binary, GPIO7 HIGH=Master / GND=Slave.
- Master on USB **COM6**, MAC `d4:05:92:40:c0:c4`, advertises `Wings-M-p2`.
- Phone talks to **Master** only. GATT protocol **2**.
- Job order (not GPIO order):

| ch | Name | GPIO | Typical taught (Master dump this session) |
|---|---|---|---|
| 0 | WRIST_HUG | 5 | closed/open 1450, hug 750, soft 670–1530 |
| 1 | WRIST_RAISE | 4 | closed 2450, open 1660, hug 1470, soft 1470–2500 |
| 2 | ELB_HUG | 3 | closed/open 1400, hug 1980, soft 1320–2060 |
| 3 | ELB_RAISE | 2 | closed **2340**, open **1450**, hug 1450, soft 1370–2420 |
| 4 | SHOULDER | 1 | closed **2150**, open **1300**, hug 1300, soft **1220–2230**, hard max 2240 |

- 150 kg on 12 V. 35 kg wrists on LM2596 6.0 V. ESP32 on 5 V. Common GND. E-stop is the **supply**, not firmware.
- Ken: 6 V / 5 V die if 12 V dies. Do not special-case wrists by supply.
- **No shaft feedback.** Commanded µs ≠ measured. Soft limits only apply to firmware dest, not to a pin driven LOW (~0 µs).
- Ken: **power-up is closed or near closed.** First COLD pulse should be taught CLOSED, then slow if anything still has to move.

---

## On the board right now (verify, do not assume)

| Item | Value | Notes |
|---|---|---|
| Firmware | **0.2.46** | Flashed COM6 this session |
| App source | **1.1.20** | APK: `app\build\app\outputs\flutter-apk\app-release.apk` |
| `NVS_MAGIC` | `0xA173` | `pio upload` does **not** erase NVS @ 0x9000 |
| Opening COM6 | DTR reset → **COLD** again | |

Confirm with serial `status` / `poses`. Boot banner `fw=0.2.46`. App must show **1.1.20** or Ken is on an old APK.

---

## What Ken has proven on the bench

### Works

**SETUP ARM** (app chip + ARM, or serial `arm <ch>`): pulse that one servo, ~1 s, Vin-brake. Slow to taught closed when COLD. Ken ran the full manual order and it was OK **after** 0.2.44/45 attach+seed, including a pass immediately before the latest RUN fail.

Manual order he wants everywhere:

1. Shoulder ARM → cycle complete (idle detach)
2. Elbow raise ARM → complete
3. Elbow hug ARM → complete
4. Wrist raise ARM → complete
5. Wrist hug ARM → complete
6. All five cycled this boot → **BRAKE READY**

### Fails — latest incident (do not dilute)

**2026-08-21 ~21:00.** Separate power cycle. SETUP sequence already OK on a prior cycle. Power up. **RUN screen ARM only.** Shoulder **immediately shot at full mechanical speed well beyond open and past soft limits.**

That is the bug. Not “app animation.” Not DISARM (unless he pressed it — he did not describe that). Soft min on shoulder is 1220; open is 1300; closed is 2150. Past soft = electrical pulse near **0 / min**, which firmware cannot clamp.

Earlier same evening (0.2.42, SETUP ARM shoulder / wrist raise): same class of slam (fly past open, then creep to closed). Hugs looked fine (lastcmd ~1450, so a 0 µs glitch is small). He refused to try elbow raise then.

### Older incidents (paid for)

| What we did | What Ken felt |
|---|---|
| First pulse taught CLOSED over lastcmd, or `PATH_CLOSE` group attach for RUN ARM | Ballistic / wrong-way full stroke |
| `ledcAttachPin` **on the servo GPIO** (0.2.36 — **PWM existed**) | SETUP worked; attach can drive **LOW one frame**; on a raise at 2100–2450 that is a run toward min |
| Matrix-only / no `ledc_channel_config` (0.2.35) | **Nothing moves** |
| Bind GPIO 21 + matrix + **`gpio_set_direction(OUTPUT)`** (0.2.41) | **Nothing moves** (pad stuck LOW) |
| Unify wrists onto that dead LEDC | Still dead; removed proven MCPWM |
| Restore `ledcAttachPin` on servo pin (0.2.42) | SETUP moved again; **raises slammed** (LOW / 0 µs) |
| COLD seed = taught closed (0.2.44) | SETUP OK |
| RUN seq skipped already-cycled chips (0.2.37–44) | RUN no-op **after** SETUP on **same** boot — **not** Ken’s later test |
| BLE `ARM` `v.size()>=2` → byte1 = channel (0.2.46 attempted fix) | Theory: padded RUN became ARM ch0. **Falsified by latest test:** RUN moved **shoulder** (seq[0]), so this was not “silent wrist hug” |

---

## Fundamental issue (state this, do not “solve” in prose)

**SETUP ARM can pulse a raise safely enough for Ken. RUN ARM on a cold boot still puts a ~0 µs / min command on the shoulder pin long enough for a full-speed stroke past open/soft.**

Firmware dest math (cosine, 10% crawl, soft clamp, taught closed) **does not apply** to that electrical first edge. Audits that only read `g_actual` / `homeUs` will **ship a slam**.

Soft limits do not save a LOW pin.

---

## Code that is actually in the tree (0.2.46)

Files: `firmware/src/servos.cpp`, `firmware/src/ble.cpp`, `firmware/include/Config.h`.

**SETUP (lock — Ken said lock this):**

- `servosArmCh(ch)` → `abortPath()` → `pulseArmCh(ch, false)`.
- **No** `detachAll()` first.

**RUN (broken on bench):**

- `servosArm()` → `abortPath()` → **`detachAll()`** → `pulseArmCh(ARM_SEQ[0], false)`.
- `ARM_SEQ[]` = SH, ER, EH, WR, WH (`CH_SHOULDER, CH_ELBOW1, CH_ELBOW2, CH_WRIST1, CH_WRIST2`).
- `serviceArmSeq`: wait until that chip not attached and not jogging, then next `pulseArmCh(..., false)`.
- **This is the only intentional SETUP vs RUN difference before the pulse:** RUN calls `detachAll()` (all five `ledcDetachPin` + `gpio_reset_pin` + INPUT). SETUP does not. On a fresh boot nothing is attached, but **resetting GPIO1 then attaching** is not what SETUP does. **Investigate this first.** Do not “improve” attach until this delta is ruled out.

**`pulseArmCh` (shared):**

- `g_armCrawl = true` (10% even after READY).
- `seedHomeIfNeeded` then dest = taught closed if first cycle this boot (`!g_cycledMask`), else lastcmd.
- `attachOne` then cosine if dest ≠ actual.

**`seedHomeIfNeeded` COLD (`!g_brakeReady`):**

- Sets `g_actual` / `g_target` to taught closed (`homeUs`). Ignores stale NVS lastcmd for the **first** seed this boot.
- After BRAKE READY, later seeds keep lastcmd (mask already set).

**`attachOne` now (unproven vs Ken’s slam):**

- `ledcSetup` → `ledcWrite(duty)` → `ledcAttachPin(PIN_LEDC_BIND=21)` → `ledcWrite` → `gpio_matrix_out(servoPin, LEDC_LS_SIG_OUT0_IDX + chan%8)`.
- LEDC_CH = `{5, 6, 2, 3, 4}`.
- No `ESP32Servo`. No `ledcAttachPin` on the servo GPIO (that path **moved** in 0.2.36 and **slammed** raises).
- Do **not** add `gpio_set_direction(OUTPUT)` / `gpio_reset_pin` on the servo pin in attach (0.2.41 mute).

**`poseClosedUs`:** if a raise’s taught CLOSED is ~1500, dest becomes OPEN. Live stamps are 2150/2450/2340 — remap should not fire. Do not “fix” this by rewriting poses.

**BLE ARM (0.2.46):**

- Channel ARM only if write is **exactly 2 bytes** and `ch < 5`.
- Else `servosArm()` (all).
- App 1.1.20 RUN sends `[CmdId.arm, 0xFF]`. SETUP sends `[cmd, ch]`.
- Serial: `arm` = all, `arm <ch>` = one chip.

**DISARM** on RUN screen: leftover. `abortPath` + `detachAll`. Does nothing unless pressed. STOP is the same class of kill. SETUP has no Disarm button.

**A/B/D** still use `startRunMotion` / `PATH_*` / group attach. Not the SETUP pulse. Do not conflate with ARM unless Ken asks.

---

## Live NVS (do not wipe)

Last serial dump this session (after 0.2.42, COM6 reset). Re-dump before changing persist.

```
WRIST_HUG    tgt/act 1450  closed=1450 open=1450 hug=750
WRIST_RAISE  2450/2450     closed=2450 open=1660
ELB_HUG      1400/1400     closed=1400 open=1400 hug=1980
ELB_RAISE    tgt=2340 act=1450   << lastcmd was OPEN; COLD seed should first-pulse 2340
SHOULDER     2150/2150     closed=2150 open=1300 soft 1220..2230
speed saved 25%, COLD crawl 10%, accel 250 ms unused in duration
ESP-NOW: no peer MAC
```

If Ken completed SETUP on all five since then, lastcmd on disk may now be closed. **Dump again.** `storeSaveLastCmd` on detach. Ghost lastcmd from 0.2.41 (software ease, dead PWM) poisoned `act` without moving the shaft — that is real, but the **latest** RUN fail is shoulder full-speed past open, which is the **0 µs pin** class even when dest/closed is 2150.

---

## What the next agent must do

1. **Do not touch SETUP ARM** (`servosArmCh` / `pulseArmCh`) unless Ken says SETUP broke.
2. Capture a **serial log** of one cold RUN ARM (and one SETUP shoulder) without guessing: `SEED`, `PWM`, `ARM ch=`, `CMD ARM`, `BLE cmd= len=`. If RUN prints `CMD ARM all` and `ARM ch4 SHOULDER ... dest=2150` and it still slams, the bug is **electrical attach**, not dest.
3. Diff RUN vs SETUP on that log. First suspect: `detachAll()` at the start of `servosArm()`. Try **deleting only that `detachAll()`** so RUN is `abortPath` + `pulseArmCh(SH, false)` like SETUP. That is a one-line experiment. Do not restack bind/matrix/MCPWM in the same flash.
4. Do not flash a new attach path unless a **12 V job has moved on USB** with that path, and Ken has not just seen a slam.
5. Do not overwrite lastcmd/poses/soft to “fix” a LOW pin.
6. Do not bring back `PATH_CLOSE` for ARM.
7. Do not unify PWM blocks “for cleanliness.”
8. Soft limits never clip a GPIO LOW. Say that if you talk about limits.
9. After any motion-writer change: Cursor Grok audit of `attachOne`, `pwmWrite`, `seedHomeIfNeeded`, `pulseArmCh`, `servosArm`, `detachAll`.

---

## Suggested first question to Ken

Only if the log is ambiguous: “On the slam, did it fly then creep back to closed (0 µs then lastcmd), or stay past open (stuck min)?”

---

## Repo map

| Path | Role |
|---|---|
| `firmware/src/servos.cpp` | All motion. Attach, ARM, paths, flap |
| `firmware/src/ble.cpp` | BLE parse (ARM length) |
| `firmware/src/store.cpp` | NVS, lastcmd load/save |
| `firmware/include/Config.h` | Version, pins, motion constants |
| `app/lib/actions.dart` | `arm()` / `arm(ch)` |
| `app/lib/main.dart` | RUN ARM + leftover DISARM; SETUP per-chip ARM |
| `.cursor/rules/no-servo-slam.mdc` | Session law |
| Mega `Wings/` `ServoTest/` | Reference only. Do not rewrite |

**Transcript (this session):** [Wings slam / ARM](1ec82342-34d8-47a8-afe2-d2df15955b0a)

---

## Versions that existed (do not re-flash to “try”)

0.2.35 mute PWM → 0.2.36 `ledcAttachPin` on servo (moved, twitch) → 0.2.37 RUN as seq but skip/PATH leftovers → 0.2.38 flap → 0.2.39–41 bind/matrix (mute) → 0.2.42 attach restore (slam) → 0.2.44 COLD=closed (SETUP OK) → 0.2.45 RUN no-skip + `detachAll` → 0.2.46 BLE ARM exact-2-byte. **SETUP OK. Cold RUN ARM still slams shoulder.**
