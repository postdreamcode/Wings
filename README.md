# Bird Wing Project — ESP32-S3

Hardware wired. Firmware + Flutter APK live in this repo.

## Layout

| Path | Purpose |
|------|---------|
| [firmware/](firmware/) | PlatformIO ESP32-S3 (same binary, GPIO7 role) |
| [app/](app/) | Flutter sideload APK |
| [Wings/](Wings/) | Mega 2560 reference (do not rewrite) |
| [ServoTest/](ServoTest/) | Mega bench reference |

## Quick start

1. Wire role pin: Master = GPIO7 floating; Slave = GPIO7→GND.
2. `cd firmware && pio run -t upload`
3. Serial 115200 → `help` → `arm` when ready.
4. Learn fob: `learn A` … `learn D`, then `save`.
5. Pair wings: on Slave note `mac`, on Master `peer <mac>` then `save`.
6. Phone: build APK from `app/`, sideload, connect to `Wings-M`.

See [firmware/README.md](firmware/README.md) and [app/README.md](app/README.md).
