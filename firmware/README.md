# Wings ESP32 Firmware

PlatformIO project for ESP32-S3 Mini (one binary, both wings).

## Role

| GPIO7 | Role |
|-------|------|
| floating / HIGH | Master — RXB6 + BLE primary + ESP-NOW TX |
| tied to GND | Slave — ESP-NOW RX + optional BLE cal |

## Build / upload

```bash
cd firmware
pio run
pio run -t upload --upload-port COMx
pio device monitor -b 115200
```

**ESP32-S3 Mini (FH4R2):** must set `board_build.flash_size = 4MB` and `board_build.partitions = default.csv`. The PlatformIO `esp32-s3-devkitc-1` board defaults to 8MB + `default_8MB.csv`, which boot-loops on 4MB parts.

Native USB upload tip: hold BOOT → plug/tap RESET → release BOOT if auto-reset upload fails.

## Boot policy

- Loads NVS calibration + taught poses
- Targets = closed (hug servos parked)
- **DISARMED** — no `attach()`, no PWM until `arm`
- Physical e-stop on the servo supply is the hard kill

## Serial commands

Type `help` in the monitor. Useful:

- `arm` / `disarm` / `home` (D: unhug then close) / `open` / `close` / `hug`
- `jog <ch> <delta_us>`  + = more open (0/1/3) or more hug (2/4) after `sense`
- `teach <ch> closed|open|hug` / `sense <ch> 0|1` / `poses`
- `learn A` … `learn D` — capture primary remote codes
- `learn2 A` … `learn2 D` — capture backup remote codes (same map)
- `setcode A|B|C|D 0|1 <n>` — set primary (0) or backup (1) code by hand
- `fob` — dump r1/r2 codes + map
- `peer aabbccddeeff` — set ESP-NOW peer MAC
- `save` / `load` — NVS


## BLE (nRF Connect)

Advertise as `Wings-M-p2` or `Wings-S-p2`.

| UUID | Role |
|------|------|
| `a1700001-...` | Service |
| `a1700002-...` | Status (read/notify) — pose in byte3 |
| `a1700003-...` | Command (write) — byte0 = CmdId |
| `a1700004-...` | Calibration R/W |
| `a1700005-...` | Fob r1+r2+map (36 bytes) |
| `a1700006-...` | Taught poses + sense |

Write command `0x01` to arm, `0x03` to home, etc. See `app/README.md`.

## Hardware note

GPIO3 (Elbow 2) is a strapping pin. Confirm the board boots with the servo signal lead attached. If not, move Elbow 2 to GPIO 8+ in `include/Config.h` only.

## Power

12 V → 150 kg servos. LM2596 @ 6.0 V → wrists. ESP32 on clean 5 V. Common GND. Never servo power through the MCU.
