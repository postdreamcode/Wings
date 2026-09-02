/*
  Wings ESP32-S3 — main
  Same firmware on both wings. GPIO7: HIGH=Master, GND=Slave.
  Boot: load NVS, DISARMED (no attach / no pulses).
*/

#include <Arduino.h>
#include "Config.h"
#include "role.h"
#include "servos.h"
#include "store.h"
#include "remote.h"
#include "now.h"
#include "ble.h"
#include "serial_cmd.h"
#include "button.h"

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("================================"));
  Serial.printf("Wings ESP32-S3  fw=%s  proto=%d\n",
                FIRMWARE_VERSION, PROTOCOL_VERSION);
  Serial.println(F("================================"));

  roleBegin();
  Serial.printf("Role: %s (GPIO%d %s)\n",
                roleName(), PIN_ROLE,
                roleIsMaster() ? "floating/HIGH" : "tied to GND");

  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, roleIsMaster() ? HIGH : LOW);

  servosBegin();
  storeBegin();
  storeApplyToServos();   // poses + lastcmd actual, still disarmed
  servosAuditEnvelope();

  nowBegin();
  remoteBegin();
  bleBegin();
  nowRebind();  // NimBLE init kills ESP-NOW recv until rebound
  serialCmdBegin();

  Serial.println(F("Pin map:"));
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    Serial.printf("  ch%d %s -> GPIO %u\n", i, SERVO_NAMES[i], SERVO_PINS[i]);
  }
  Serial.println(F("Boot: DISARMED. SETUP ARM each servo to taught CLOSED."));
  Serial.println(F("NOTE: GPIO3=Elbow hug is a strapping pin — confirm boot with lead attached."));
}

void loop() {
  serialCmdService();
  remoteService();
  nowService();
  buttonService();
  servosService();
  bleService();
}
