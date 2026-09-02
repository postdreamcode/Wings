#include "serial_cmd.h"
#include "Config.h"
#include "role.h"
#include "servos.h"
#include "store.h"
#include "remote.h"
#include "now.h"
#include "button.h"
#include "ble.h"
#include <WiFi.h>

static String g_line;

void serialCmdPrintHelp() {
  Serial.println(F("=== Wings ESP32 commands ==="));
  Serial.println(F("  help              this list"));
  Serial.println(F("  status            dump state"));
  Serial.println(F("  arm               ARM ALL this board: SH ER EH WR WH"));
  Serial.println(F("  arm <ch>          SETUP pulse to taught CLOSED, then brake"));
  Serial.println(F("  disarm / disarm <ch>"));
  Serial.println(F("  home              D: COLD=ARM ALL; live=STOP then slow close"));
  Serial.println(F("  open / close / hug   absolute pose (path + brake)"));
  Serial.println(F("  stop              abort path, keep last µs, detach"));
  Serial.println(F("  seq               C flap: ± about closed, elbow toward hug, wrist opposite"));
  Serial.println(F("  jog <ch> <dus>    eased; + = more open/hug after sense"));
  Serial.println(F("  set <ch> <us>     absolute target µs"));
  Serial.println(F("  teach <ch> closed|open|hug"));
  Serial.println(F("  sense <ch> <0|1>  flip +jog direction"));
  Serial.println(F("  speed <1-100>     global % (saved; 10% while COLD)"));
  Serial.println(F("  chspeed <ch> <1-100>  per-servo % (saved)"));
  Serial.println(F("  poses             dump taught µs"));
  Serial.println(F("  softmin/softmax/hardmin/hardmax/center <ch> <us>"));
  Serial.println(F("  save / load       NVS"));
  Serial.println(F("  learn <A|B|C|D>    capture primary remote code"));
  Serial.println(F("  learn2 <A|B|C|D>   capture backup remote code"));
  Serial.println(F("  fob                dump fob codes/map"));
  Serial.println(F("  setcode <A-D> <0|1> <n>  set code slot manually"));
  Serial.println(F("  peer <aabbccddeeff>  set ESP-NOW peer MAC"));
  Serial.println(F("  peer clear        forget saved peer"));
  Serial.println(F("  mac               print this MAC"));
  Serial.println(F("  trace             dump the motion trace ring"));
  Serial.println(F("  trace clear       reset the motion trace ring"));
}

static const char* poseName(WingPose p) {
  if (p == POSE_OPEN) return "OPEN";
  if (p == POSE_HUG) return "HUG";
  return "CLOSED";
}

static void printStatus() {
  Serial.printf("role=%s live=%d brake=%d mask=0x%02X pose=%s path=%d ble=%d\n",
                roleName(),
                servosIsArmed() ? 1 : 0,
                servosIsBrakeReady() ? 1 : 0,
                (unsigned)servosAttachMask(),
                poseName(getWingPose()),
                (int)pathGet(),
                bleIsConnected() ? 1 : 0);
  Serial.printf("fw=%s proto=%d mac=%s speed_live=%d%% saved=%d%%\n",
                FIRMWARE_VERSION, PROTOCOL_VERSION, WiFi.macAddress().c_str(),
                (int)servosGetEffectiveSpeedPct(), (int)servosGetSpeedPct());
  uint8_t peer[6];
  nowGetStatusPeerMac(peer);
  const char* link = "NONE";
  switch (nowSlaveLink()) {
    case NOW_LINK_HEARD:  link = "HEARD"; break;
    case NOW_LINK_LIVE:   link = "LIVE"; break;
    case NOW_LINK_STALE:  link = "STALE"; break;
    case NOW_LINK_PAUSED: link = "PAUSED"; break;
    default: break;
  }
  Serial.printf("ESP-NOW link=%s age=%ucs ch=%u rx=%lu peer=%02X:%02X:%02X:%02X:%02X:%02X\n",
                link, (unsigned)nowSlaveAgeCs(),
                (unsigned)nowWifiChannel(), (unsigned long)nowRxCount(),
                peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    ChannelCal& c = servosCal(i);
    Serial.printf("  %s tgt=%d act=%d spd=%u%% soft=%d..%d hard=%d..%d ctr=%d sense=%u pin=%d att=%d\n",
                  SERVO_NAMES[i],
                  servosGetTarget(i), servosGetActual(i),
                  (unsigned)servosGetChSpeedPct(i),
                  c.softMin, c.softMax, c.hardMin, c.hardMax, c.center,
                  (unsigned)servosGetSense(i), SERVO_PINS[i],
                  (servosAttachMask() & (1u << i)) ? 1 : 0);
  }
}

static void printPoses() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    const int closed = (int)servosGetPoseUs(i, POSE_CLOSED);
    const int openp = (int)servosGetPoseUs(i, POSE_OPEN);
    const int hug = (int)servosGetPoseUs(i, POSE_HUG);
    const ChannelCal& c = servosCal(i);
    const int lo = (closed < openp) ? closed : openp;
    const int hi = (closed > openp) ? closed : openp;
    const int hugLo = (hug < lo) ? hug : lo;
    const int hugHi = (hug > hi) ? hug : hi;
    Serial.printf("  %s closed=%d open=%d hug=%d dOpen=%+d dHug=%+d "
                  "soft=%d..%d beyond=%d..%d sense=%u\n",
                  SERVO_NAMES[i], closed, openp, hug,
                  openp - closed, hug - closed,
                  c.softMin, c.softMax,
                  hugLo - c.softMin, c.softMax - hugHi,
                  (unsigned)servosGetSense(i));
  }
}

static bool parseMac(const String& s, uint8_t out[6]) {
  if (s.length() != 12) return false;
  for (int i = 0; i < 6; i++) {
    char buf[3] = { s.charAt(i * 2), s.charAt(i * 2 + 1), 0 };
    char* end = nullptr;
    long v = strtol(buf, &end, 16);
    if (end == buf) return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

static void handleLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  int sp = line.indexOf(' ');
  String cmd = (sp < 0) ? line : line.substring(0, sp);
  String rest = (sp < 0) ? "" : line.substring(sp + 1);
  rest.trim();
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") {
    serialCmdPrintHelp();
  } else if (cmd == "status") {
    printStatus();
  } else if (cmd == "arm") {
    int ch = -1;
    if (rest.length() > 0) sscanf(rest.c_str(), "%d", &ch);
    if (ch >= 0 && ch < NUM_SERVOS) {
      int16_t p[1] = { (int16_t)ch };
      buttonDispatch(BTN_SERIAL, CMD_ARM, p, 1);
    } else {
      buttonDispatch(BTN_SERIAL, CMD_ARM_ALL, nullptr, 0);
      Serial.println(F("ARM ALL queued (STOP aborts)"));
    }
  } else if (cmd == "disarm") {
    int ch = -1;
    if (rest.length() > 0) sscanf(rest.c_str(), "%d", &ch);
    if (ch >= 0 && ch < NUM_SERVOS) {
      int16_t p[1] = { (int16_t)ch };
      buttonDispatch(BTN_SERIAL, CMD_DISARM, p, 1);
    } else {
      buttonDispatch(BTN_SERIAL, CMD_DISARM, nullptr, 0);
    }
  } else if (cmd == "home") {
    buttonDispatch(BTN_SERIAL, CMD_HOME, nullptr, 0);
  } else if (cmd == "open") {
    buttonDispatch(BTN_SERIAL, CMD_POSE_OPEN, nullptr, 0);
  } else if (cmd == "close") {
    buttonDispatch(BTN_SERIAL, CMD_POSE_FOLDED, nullptr, 0);
  } else if (cmd == "hug" || cmd == "wrists") {
    buttonDispatch(BTN_SERIAL, CMD_POSE_HUG, nullptr, 0);
  } else if (cmd == "stop") {
    buttonDispatch(BTN_SERIAL, CMD_STOP, nullptr, 0);
  } else if (cmd == "trace") {
    if (rest.equalsIgnoreCase("clear")) servosTraceClear();
    else servosTraceDump();
  } else if (cmd == "poses") {
    printPoses();
  } else if (cmd == "teach") {
    int ch = 0;
    char which[12] = {};
    if (sscanf(rest.c_str(), "%d %11s", &ch, which) != 2) {
      Serial.println(F("usage: teach <ch> closed|open|hug"));
      return;
    }
    WingPose p = POSE_CLOSED;
    if (strcasecmp(which, "open") == 0) p = POSE_OPEN;
    else if (strcasecmp(which, "hug") == 0) p = POSE_HUG;
    else if (strcasecmp(which, "closed") != 0 && strcasecmp(which, "close") != 0) {
      Serial.println(F("pose must be closed|open|hug"));
      return;
    }
    int16_t pl[2] = { (int16_t)ch, (int16_t)p };
    buttonDispatch(BTN_SERIAL, CMD_TEACH_POSE, pl, 2);
  } else if (cmd == "sense") {
    int ch = 0, s = 0;
    if (sscanf(rest.c_str(), "%d %d", &ch, &s) != 2) {
      Serial.println(F("usage: sense <ch> <0|1>"));
      return;
    }
    int16_t pl[2] = { (int16_t)ch, (int16_t)(s != 0) };
    buttonDispatch(BTN_SERIAL, CMD_SET_SENSE, pl, 2);
  } else if (cmd == "speed") {
    int pct = 0;
    if (sscanf(rest.c_str(), "%d", &pct) != 1) {
      Serial.println(F("usage: speed <1-100>"));
      return;
    }
    int16_t p[1] = { (int16_t)pct };
    buttonDispatch(BTN_SERIAL, CMD_SET_SPEED, p, 1);
  } else if (cmd == "chspeed") {
    int ch = 0, pct = 0;
    if (sscanf(rest.c_str(), "%d %d", &ch, &pct) != 2) {
      Serial.println(F("usage: chspeed <ch> <1-100>"));
      return;
    }
    int16_t p[2] = { (int16_t)ch, (int16_t)pct };
    buttonDispatch(BTN_SERIAL, CMD_SET_CH_SPEED, p, 2);
  } else if (cmd == "seq") {
    buttonDispatch(BTN_SERIAL, CMD_SEQ, nullptr, 0);
  } else if (cmd == "save") {
    storeSave();
  } else if (cmd == "load") {
    if (storeLoad()) storeApplyToServos();
  } else if (cmd == "mac") {
    Serial.println(WiFi.macAddress());
  } else if (cmd == "fob") {
    for (uint8_t i = 0; i < FOB_BUTTONS; i++) {
      Serial.printf("  %c r1=%lu r2=%lu map=%u\n", 'A' + i,
                    (unsigned long)remoteGetCode(i, 0),
                    (unsigned long)remoteGetCode(i, 1),
                    (unsigned)remoteGetMap(i));
    }
  } else if (cmd == "learn" || cmd == "learn2") {
    uint8_t slot = (cmd == "learn2") ? 1 : 0;
    if (rest.length() < 1) {
      Serial.printf("usage: %s A|B|C|D\n", cmd.c_str());
      return;
    }
    char b = toupper(rest.charAt(0));
    if (b < 'A' || b > 'D') {
      Serial.println(F("button must be A-D"));
      return;
    }
    remoteStartLearn((uint8_t)(b - 'A'), slot);
  } else if (cmd == "setcode") {
    char bch = 0;
    int slot = 0;
    unsigned long code = 0;
    if (sscanf(rest.c_str(), "%c %d %lu", &bch, &slot, &code) != 3) {
      Serial.println(F("usage: setcode A|B|C|D 0|1 <code>"));
      return;
    }
    bch = toupper(bch);
    if (bch < 'A' || bch > 'D' || slot < 0 || slot >= FOB_REMOTES) {
      Serial.println(F("bad button/slot"));
      return;
    }
    storeData().fobCode[(uint8_t)(bch - 'A')][(uint8_t)slot] = (uint32_t)code;
    Serial.printf("set %c slot%u = %lu\n", bch, (unsigned)slot, code);
  } else if (cmd == "peer") {
    rest.trim();
    if (rest.length() == 0) {
      uint8_t mac[6];
      nowGetStatusPeerMac(mac);
      Serial.printf("peer %02X:%02X:%02X:%02X:%02X:%02X  saved=%d  link=%u\n",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                    nowHasPeer() ? 1 : 0, (unsigned)nowSlaveLink());
      return;
    }
    if (rest.equalsIgnoreCase("clear") || rest == "0") {
      nowClearPeer();
      storeSave();
      return;
    }
    uint8_t mac[6];
    String hex = rest;
    hex.replace(":", "");
    hex.replace("-", "");
    hex.toLowerCase();
    if (!parseMac(hex, mac)) {
      Serial.println(F("usage: peer aabbccddeeff | peer clear"));
      return;
    }
    nowSetPeerMac(mac);
    storeSave();
  } else if (cmd == "jog") {
    int ch = 0, dus = 0;
    if (sscanf(rest.c_str(), "%d %d", &ch, &dus) != 2) {
      Serial.println(F("usage: jog <ch> <delta_us>"));
      return;
    }
    int16_t p[2] = { (int16_t)ch, (int16_t)dus };
    buttonDispatch(BTN_SERIAL, CMD_JOG, p, 2);
  } else if (cmd == "set") {
    int ch = 0, us = 0;
    if (sscanf(rest.c_str(), "%d %d", &ch, &us) != 2) {
      Serial.println(F("usage: set <ch> <us>"));
      return;
    }
    servosSetTarget((uint8_t)ch, us);
  } else if (cmd == "softmin" || cmd == "softmax" || cmd == "hardmin" ||
             cmd == "hardmax" || cmd == "center") {
    int ch = 0, us = 0;
    if (sscanf(rest.c_str(), "%d %d", &ch, &us) != 2) {
      Serial.println(F("usage: <lim> <ch> <us>"));
      return;
    }
    if (cmd == "softmin") servosSetSoftMin((uint8_t)ch, us);
    else if (cmd == "softmax") servosSetSoftMax((uint8_t)ch, us);
    else if (cmd == "hardmin") servosSetHardMin((uint8_t)ch, us);
    else if (cmd == "hardmax") servosSetHardMax((uint8_t)ch, us);
    else servosSetCenter((uint8_t)ch, us);
  } else {
    Serial.printf("unknown: %s  (help)\n", cmd.c_str());
  }
}

void serialCmdBegin() {
  g_line.reserve(80);
  serialCmdPrintHelp();
}

void serialCmdService() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      handleLine(g_line);
      g_line = "";
    } else if (g_line.length() < 120) {
      g_line += c;
    }
  }
}
