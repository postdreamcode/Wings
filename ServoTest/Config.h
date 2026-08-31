#pragma once

/*
  Hardware config for Mega 2560 + 3.5" MCUFRIEND TFT + ANNIMOS DS51150-12V

  POWER (critical):
    - Servos need a dedicated 12V supply (9–12.6V), NOT the Mega 5V rail.
    - Stall current ~8A each — size the PSU for both servos + headroom.
    - Common GND between Mega and servo PSU is required.
    - Signal wires only to SERVO*_PIN; never feed 12V into Mega I/O.

  LIMITS:
    - ABS  = datasheet pulse range (never exceeded)
    - HARD = mechanical stops (set once with horns/linkage)
    - SOFT = working envelope inside HARD (day-to-day travel)
*/

// ---------- Touch (common MCUFRIEND 3.5") ----------
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

// ---------- Servo pins ----------
#define SERVO1_PIN 44
#define SERVO2_PIN 45

// Datasheet absolute pulse range
#define SERVO_ABS_MIN 500
#define SERVO_ABS_MAX 2500
#define SERVO_CENTER  1500

#define SERVO_TRAVEL_DEG 270

// Defaults: HARD ≈ former soft range; SOFT tighter working band
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
#define TOUCH_DEBOUNCE_MS  220
#define VALUE_REFRESH_MS   80

// EEPROM (bumped for hard/soft/center fields)
#define EEPROM_MAGIC 0xA160
#define EEPROM_ADDR  0
