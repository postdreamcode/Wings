# Wings Flutter App

Sideload APK for Wings ESP32 BLE control. No Play Store.

## Build APK

Requires Flutter + Android SDK + JDK 17.

```powershell
cd app
flutter pub get
flutter build apk --release
```

Output (email / Drive this file):

`build/app/outputs/flutter-apk/app-release.apk`

Bump `version:` in `pubspec.yaml` for every sideload (e.g. `1.1.1+3`).

## First launch on phone

1. Enable install from unknown sources / allow the file manager.
2. Open the APK.
3. Grant Bluetooth (and Location on older Android).
4. Scan → connect to `Wings-M-p2` (Master) or `Wings-S-p2`.

Connect screen shows **app version** and **expected firmware protocol** (2).

## BLE protocol (matches firmware)

- Device name: `Wings-M-p2` / `Wings-S-p2`
- Service: `a1700001-0000-1000-8000-00805f9b34fb`
- Status notify: `a1700002-...` — byte3 = pose (0 closed, 1 open, 2 hug)
- Command write: `a1700003-...` — first byte = CmdId
- Cal R/W: `a1700004-...` — 5× ChannelCal (12 bytes each)
- Fob R/W: `a1700005-...` — r1[4] + r2[4] + map[4] = 36 bytes
- Poses R/W: `a1700006-...` — closed/open/hug int16[5] + sense[5]

### CmdId

| Id | Name |
|----|------|
| 1 | ARM |
| 2 | DISARM |
| 3 | HOME (D: unhug then close) |
| 4 | TOGGLE_WING (A) |
| 5 | HUG (B; id kept) |
| 6 | SEQ (C stub) |
| 7 | SET_TARGETS |
| 8 | JOG |
| 9 | POSE_FOLDED |
| 10 | POSE_OPEN |
| 11 | TEACH_POSE |
| 12 | SET_SENSE |

| 17 | SET_CH_SPEED |
| 18 | ARM_ALL (SH ER EH WR WH) |

## Tabs

- **RUN** — ARM ALL, A/B/C/D, STOP, speed. Accel slider removed (cosine ease; firmware ignores SET_ACCEL).
- **SETUP** — per-channel ARM/DISARM, jog, sense, SET CLOSED/OPEN/HUG, soft/hard envelope
- **FOB** — command map (codes already learned on Master)
