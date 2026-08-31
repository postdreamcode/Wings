#pragma once

/*
  Wings — Mega 2560 + 3.5" MCUFRIEND TFT
  S1/S2: main open/close (ANNIMOS 150kg / 12V)
  S3/S4: tip flap (ANNIMOS 35kg via DC-DC)
  Remote: DieseRC 433 MHz 4-ch dry-contact receiver (momentary mode)

  POWER:
    - Main servos: dedicated 12V PSU, common GND with Mega.
    - Tip servos: DC-DC to their rated voltage, common GND.
    - Never feed servo power into Mega I/O. Signal wires only.
    - Physical e-stop / power switch on servo supply is the hard kill.

  REMOTE WIRING (recommended):
    Receiver COM → Mega GND
    Each NO → REMOTE_*_PIN
    Pins use INPUT_PULLUP → pressed = LOW
*/

// ---------- Touch (MCUFRIEND 3.5") ----------
#define YP A3
#define XM A2
#define YM 9
#define XP 8

#define TS_MINX 150
#define TS_MAXX 920
#define TS_MINY 120
#define TS_MAXY 940
#define MINPRESSURE 200
#define MAXPRESSURE 1000
#define TS_OHMS 300

#define TOUCH_SWAP_XY  0
#define TOUCH_INVERT_X 0
#define TOUCH_INVERT_Y 1
#define TOUCH_DEBUG 0

// ---------- Servo pins (Mega Timer5-friendly) ----------
#define SERVO1_PIN 44   // main left
#define SERVO2_PIN 45   // main right
#define SERVO3_PIN 46   // tip left
#define SERVO4_PIN 47   // tip right

// ---------- Remote inputs (active LOW, pullup) ----------
#define REMOTE_A_PIN 22  // toggle main open/close
#define REMOTE_B_PIN 23  // toggle tips (only if main open)
#define REMOTE_C_PIN 24  // flap sequence
#define REMOTE_D_PIN 25  // emergency home (highest priority)

#define REMOTE_ACTIVE_LOW 1
#define REMOTE_DEBOUNCE_MS 40

// Datasheet absolute pulse range
#define SERVO_ABS_MIN 500
#define SERVO_ABS_MAX 2500
#define SERVO_CENTER  1500

#define SERVO_TRAVEL_DEG 270

// Defaults: HARD ≈ full usable; SOFT = working band
#define DEFAULT_HARD_MIN 900
#define DEFAULT_HARD_MAX 2100
#define DEFAULT_SOFT_MIN 1200
#define DEFAULT_SOFT_MAX 1800

// Motion / UI timing
#define SLIDER_DEADBAND_US 6
#define NUDGE_FINE_US      10
#define NUDGE_COARSE_US    50
#define RAMP_STEP_US       20
#define RAMP_INTERVAL_MS   8
#define ARRIVE_US          25
#define TOUCH_DEBOUNCE_MS  220
#define VALUE_REFRESH_MS   80

// Sequence (light flap)
#define SEQ_FLAP_CYCLES    4
#define SEQ_HOLD_MS        280
#define SEQ_MAIN_BREATH_US 40   // small main pulse during flap (0 = tips only)

// Boot: auto-arm after loading home targets
#define BOOT_AUTO_ARM 1

// EEPROM
#define EEPROM_MAGIC 0xA161
#define EEPROM_ADDR  0
