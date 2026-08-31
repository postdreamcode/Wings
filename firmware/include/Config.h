#pragma once

/*
  Wings ESP32-S3 — Config
  One binary per wing. Role via ROLE_PIN (GPIO7).

  POWER (non-negotiable):
    - 150 kg servos: dedicated 12 V, common GND with ESP32.
    - 35 kg wrists: LM2596 → 6.0 V, common GND.
    - ESP32 on clean 5 V. Never route servo power through the MCU.
    - Physical e-stop on the servo supply is the hard kill.

  GPIO 3 (Elbow hug) is a strapping pin — confirm boot with signal lead attached.
  If the board fails to boot, move Elbow hug to GPIO 8+ here only.
*/

#include <Arduino.h>

// ---------- Protocol / identity ----------
#define FIRMWARE_VERSION   "0.2.67"
#define PROTOCOL_VERSION   2
#define NVS_MAGIC          0xA173   // channel order: jobs, not GPIO numbers
#define NVS_MAGIC_V171     0xA171
#define NVS_MAGIC_V172     0xA172
#define FOB_REMOTES        2        // primary + backup remote
#define NVS_NAMESPACE      "wings"

// ---------- Channel count / names ----------
#define NUM_SERVOS 5

enum ServoCh : uint8_t {
  CH_WRIST2   = 0,  // WRIST HUG
  CH_WRIST1   = 1,  // WRIST RAISE
  CH_ELBOW2   = 2,  // ELB HUG
  CH_ELBOW1   = 3,  // ELB RAISE
  CH_SHOULDER = 4
};

static const char* const SERVO_NAMES[NUM_SERVOS] = {
  "WRIST_HUG", "WRIST_RAISE", "ELB_HUG", "ELB_RAISE", "SHOULDER"
};

// ---------- Pins (ESP32-S3 Mini) — Ken's live harness 2026-08-15 ----------
// GPIO1 Shoulder, GPIO2 Elb raise, GPIO3 Elb hug, GPIO4 Wrist raise, GPIO5 Wrist hug
#define PIN_SHOULDER  1
#define PIN_ELBOW1    2   // ELB RAISE
#define PIN_ELBOW2    3   // ELB HUG — strapping
#define PIN_WRIST1    4   // WRIST RAISE
#define PIN_WRIST2    5   // WRIST HUG
#define PIN_RXB6      6   // Master only
#define PIN_ROLE      7   // INPUT_PULLUP: HIGH=Master, GND=Slave
#define PIN_STATUS_LED 9  // spare (avoid GPIO8 — reserved as Elbow2 fallback)
#define PIN_LEDC_BIND  21 // unused pad: start lastcmd PWM here, then matrix servo pin

static const uint8_t SERVO_PINS[NUM_SERVOS] = {
  PIN_WRIST2, PIN_WRIST1, PIN_ELBOW2, PIN_ELBOW1, PIN_SHOULDER
};

// ---------- Pulse / limits (µs) ----------
#define SERVO_ABS_MIN  500
#define SERVO_ABS_MAX  2500
#define SERVO_CENTER   1500

#define DEFAULT_HARD_MIN 900
#define DEFAULT_HARD_MAX 2100
#define DEFAULT_SOFT_MIN 1200
#define DEFAULT_SOFT_MAX 1800

// ---------- Motion ----------
// Pose/flap ramps: cosine ease, open/close=full, hug=half, D=quarter.
// BRAKE READY after attach→closed→detach on all 5 (gates A/B/C).
// ARM: taught CLOSED only. Duty is live on BIND and ledcRead-checked
// before the servo pad is connected. ledcSetup starts at duty 0 — never
// matrix until the closed duty reads back. gpio_matrix_out enable-window
// is latched HIGH at boot, detach, and connect (raises: LOW = slam
// open). Never pinMode OUTPUT,
// gpio_reset_pin, or ledcAttachPin on a servo GPIO.
#define RAMP_STEP_US      20
#define RAMP_STEP_MIN_US  10   // LEDC 12-bit ≈ 5 µs/count; 1 µs steps only buzz
#define RAMP_INTERVAL_MS  8
#define ARRIVE_US         25
#define SPEED_SCALE_PCT   10
#define SETUP_IDLE_DETACH_MS 200  // one clean frame after arrival, then brake
#define ATTACH_STAGGER_MS 150  // D dwell only; RUN open/close attaches the group at once
#define MIN_STAGE_HOLD_MS 250  // visible pulse before Vin-brake, even if already there

// DS51150-12V datasheet: 0.21 s/60° no-load, 270° over 500–2500 µs.
// Load derate keeps command slower than no-load so the horn can follow.
#define SERVO_SPEC_MS_PER_60DEG  210
#define SERVO_SPEC_TRAVEL_DEG    270
#define SERVO_SPEC_SPAN_US       2000
#define SERVO_SPEC_LOAD_PCT      70
#define RAMP_ACCEL_MS            250  // default; live value is NVS reserved[1..2]
#define RAMP_ACCEL_MIN_MS        50
#define RAMP_ACCEL_MAX_MS        2000

// ---------- Sequence (C flap) ----------
// Hug pair only, from OPEN. ±SEQ_FLAP_DEG about cal.center.
// Hug pose is "forward". Wrist lags elbow.
// Strokes go extreme-to-extreme through center (no stop at center).
#define SEQ_FLAP_CYCLES        4
#define SEQ_FLAP_DEG           15
#define SEQ_FLAP_WRIST_LAG_MS  150
#define SEQ_HOLD_MS            0
#define SEQ_SHOULDER_BREATH_US 40

// ---------- Remote ----------
#define REMOTE_DEBOUNCE_MS 350
#define FOB_BUTTONS 4  // A B C D

// ---------- ESP-NOW ----------
#define NOW_CHANNEL 1
#define NOW_MAX_PEERS 1
#define NOW_HELLO_MS 1000
#define NOW_ALIVE_MS 2500
#define NOW_CMD_HELLO 0xF0  // not a servo CmdId
#define NOW_CMD_ACK   0xF1
#define NOW_CMD_PING  0xF2  // Master → Slave; 0.2.52 Slave ACKs as unknown cmd

// ---------- BLE ----------
#define BLE_DEVICE_PREFIX "Wings"
