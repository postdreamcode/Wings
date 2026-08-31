#include "remote.h"
#include "role.h"
#include "store.h"
#include "now.h"
#include "button.h"
#include <RCSwitch.h>

static RCSwitch rx;
static bool g_learning = false;
static uint8_t g_learnBtn = 0;
static uint8_t g_learnSlot = 0;
static unsigned long g_lastEdgeMs = 0;
static uint32_t g_lastCode = 0;

void remoteBegin() {
  if (!roleIsMaster()) {
    Serial.println(F("RXB6: skipped (Slave)"));
    return;
  }
  rx.enableReceive(PIN_RXB6);
  Serial.printf("RXB6: listening on GPIO %d\n", PIN_RXB6);
}

void remoteStartLearn(uint8_t button, uint8_t remoteSlot) {
  if (!roleIsMaster()) return;
  if (button >= FOB_BUTTONS) return;
  if (remoteSlot >= FOB_REMOTES) return;
  g_learning = true;
  g_learnBtn = button;
  g_learnSlot = remoteSlot;
  Serial.printf("LEARN%s: press fob for slot %c (remote %u)\n",
                remoteSlot ? "2" : "",
                'A' + button,
                (unsigned)(remoteSlot + 1));
}

void remoteCancelLearn() {
  g_learning = false;
  Serial.println(F("LEARN cancelled"));
}

bool remoteIsLearning() { return g_learning; }
uint8_t remoteLearnButton() { return g_learnBtn; }
uint8_t remoteLearnSlot() { return g_learnSlot; }

void remoteClearCode(uint8_t button, uint8_t remoteSlot) {
  if (button >= FOB_BUTTONS || remoteSlot >= FOB_REMOTES) return;
  storeData().fobCode[button][remoteSlot] = 0;
}

void remoteClearAllCodes(uint8_t button) {
  if (button >= FOB_BUTTONS) return;
  for (uint8_t s = 0; s < FOB_REMOTES; s++) {
    storeData().fobCode[button][s] = 0;
  }
}

uint32_t remoteGetCode(uint8_t button, uint8_t remoteSlot) {
  if (button >= FOB_BUTTONS || remoteSlot >= FOB_REMOTES) return 0;
  return storeData().fobCode[button][remoteSlot];
}

void remoteSetMap(uint8_t button, CmdId cmd) {
  if (button >= FOB_BUTTONS) return;
  storeData().fobMap[button] = (uint8_t)cmd;
}

CmdId remoteGetMap(uint8_t button) {
  if (button >= FOB_BUTTONS) return CMD_NONE;
  return (CmdId)storeData().fobMap[button];
}

static void fireButton(uint8_t btn) {
  CmdId cmd = remoteGetMap(btn);
  Serial.printf("FOB %c -> cmd %u\n", 'A' + btn, (unsigned)cmd);
  buttonDispatch(BTN_FOB, cmd, nullptr, 0);
}

void remoteService() {
  if (!roleIsMaster()) return;
  if (!rx.available()) return;

  uint32_t code = rx.getReceivedValue();
  unsigned long now = millis();
  rx.resetAvailable();

  if (code == 0) return;

  if (code == g_lastCode && (now - g_lastEdgeMs) < REMOTE_DEBOUNCE_MS) {
    return;
  }
  if ((now - g_lastEdgeMs) < REMOTE_DEBOUNCE_MS) return;
  g_lastEdgeMs = now;
  g_lastCode = code;

  if (g_learning) {
    storeData().fobCode[g_learnBtn][g_learnSlot] = code;
    Serial.printf("LEARNED%s %c = %lu\n",
                  g_learnSlot ? "2" : "",
                  'A' + g_learnBtn,
                  (unsigned long)code);
    g_learning = false;
    storeSave();
    return;
  }

  for (uint8_t i = 0; i < FOB_BUTTONS; i++) {
    for (uint8_t s = 0; s < FOB_REMOTES; s++) {
      uint32_t c = storeData().fobCode[i][s];
      if (c != 0 && c == code) {
        fireButton(i);
        return;
      }
    }
  }
  Serial.printf("FOB unknown code %lu\n", (unsigned long)code);
}
