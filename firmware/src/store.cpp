#include "store.h"
#include <Preferences.h>

static Preferences prefs;
static PersistData g_data;

void storeDefaults(PersistData& d) {
  memset(&d, 0, sizeof(d));
  d.magic = NVS_MAGIC;
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    d.cal[i].hardMin = DEFAULT_HARD_MIN;
    d.cal[i].hardMax = DEFAULT_HARD_MAX;
    d.cal[i].softMin = DEFAULT_SOFT_MIN;
    d.cal[i].softMax = DEFAULT_SOFT_MAX;
    d.cal[i].center = SERVO_CENTER;
    d.cal[i].invert = 0;
    d.cal[i].pad = 0;
    d.sense[i] = 0;
  }
  d.fobMap[0] = (uint8_t)CMD_TOGGLE_WING;
  d.fobMap[1] = (uint8_t)CMD_TOGGLE_WRIST;
  d.fobMap[2] = (uint8_t)CMD_SEQ;
  d.fobMap[3] = (uint8_t)CMD_HOME;
}

static void fillDefaultPoses(PersistData& d) {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    const bool hug = (i == CH_ELBOW2 || i == CH_WRIST2);
    if (hug) {
      d.poseClosed[i] = d.cal[i].center;
      d.poseOpen[i] = d.cal[i].center;
      d.poseHug[i] = d.cal[i].softMax;
    } else {
      d.poseClosed[i] = d.cal[i].softMin;
      d.poseOpen[i] = d.cal[i].softMax;
      d.poseHug[i] = d.cal[i].softMax;
    }
  }
}

void storeBegin() {
  storeDefaults(g_data);
  fillDefaultPoses(g_data);
  prefs.begin(NVS_NAMESPACE, false);
  storeLoad();
}

PersistData& storeData() { return g_data; }

void storeApplyToServos() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    ChannelCal& c = servosCal(i);
    c = g_data.cal[i];
    servosClampSoftInsideHard(i);
    servosSetSense(i, g_data.sense[i] != 0);
    servosSetPoseUs(i, POSE_CLOSED, g_data.poseClosed[i]);
    servosSetPoseUs(i, POSE_OPEN, g_data.poseOpen[i]);
    servosSetPoseUs(i, POSE_HUG, g_data.poseHug[i]);
  }
  if (g_data.reserved[0] >= 1 && g_data.reserved[0] <= 100) {
    servosSetSpeedPct(g_data.reserved[0]);
  }
  {
    uint16_t accel = (uint16_t)g_data.reserved[1] | ((uint16_t)g_data.reserved[2] << 8);
    if (accel != 0) servosSetAccelMs(accel);
  }
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    uint8_t p = g_data.reserved[3 + i];
    servosSetChSpeedPct(i, (p >= 1 && p <= 100) ? p : 100);
  }
  applyPoseClosed();
  if (!storeLoadLastCmd()) servosAlignActualToTargets();
}

void storeSaveLastCmd() {
  int16_t u[NUM_SERVOS];
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    u[i] = (int16_t)servosGetActual(i);
  }
  prefs.putBytes("lastcmd", u, sizeof(u));
}

bool storeLoadLastCmd() {
  if (prefs.getBytesLength("lastcmd") != sizeof(int16_t) * NUM_SERVOS) return false;
  int16_t u[NUM_SERVOS];
  prefs.getBytes("lastcmd", u, sizeof(u));
  servosApplyLastCmd(u);
  Serial.println(F("lastcmd loaded"));
  return true;
}

void storeSaveLastHeard(const uint8_t mac[6]) {
  if (!mac) return;
  prefs.putBytes("lasthear", mac, 6);
}

bool storeLoadLastHeard(uint8_t out[6]) {
  if (!out) return false;
  if (prefs.getBytesLength("lasthear") != 6) return false;
  prefs.getBytes("lasthear", out, 6);
  return true;
}

void storePullFromServos() {
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    g_data.cal[i] = servosCal(i);
    g_data.sense[i] = servosGetSense(i);
    g_data.poseClosed[i] = servosGetPoseUs(i, POSE_CLOSED);
    g_data.poseOpen[i] = servosGetPoseUs(i, POSE_OPEN);
    g_data.poseHug[i] = servosGetPoseUs(i, POSE_HUG);
  }
  g_data.reserved[0] = servosGetSpeedPct();
  {
    uint16_t accel = servosGetAccelMs();
    g_data.reserved[1] = (uint8_t)(accel & 0xff);
    g_data.reserved[2] = (uint8_t)((accel >> 8) & 0xff);
  }
  for (uint8_t i = 0; i < NUM_SERVOS; i++) {
    g_data.reserved[3 + i] = servosGetChSpeedPct(i);
  }
  g_data.magic = NVS_MAGIC;
}

bool storeLoad() {
  size_t n = prefs.getBytesLength("persist");
  if (n == sizeof(PersistDataV171)) {
    PersistDataV171 oldp;
    prefs.getBytes("persist", &oldp, sizeof(oldp));
    if (oldp.magic == NVS_MAGIC_V171) {
      storeDefaults(g_data);
      memcpy(g_data.cal, oldp.cal, sizeof(oldp.cal));
      memcpy(g_data.fobCode, oldp.fobCode, sizeof(oldp.fobCode));
      memcpy(g_data.fobMap, oldp.fobMap, sizeof(oldp.fobMap));
      memcpy(g_data.peerMac, oldp.peerMac, sizeof(oldp.peerMac));
      fillDefaultPoses(g_data);
      g_data.magic = NVS_MAGIC;
      Serial.println(F("NVS migrated 0xA171 (fob kept)"));
      return true;
    }
  }
  if (n != sizeof(PersistData)) {
    Serial.println(F("NVS empty/size mismatch — defaults"));
    storeDefaults(g_data);
    fillDefaultPoses(g_data);
    return false;
  }
  PersistData tmp;
  prefs.getBytes("persist", &tmp, sizeof(tmp));
  if (tmp.magic == NVS_MAGIC) {
    g_data = tmp;
    Serial.println(F("NVS loaded"));
    return true;
  }
  if (tmp.magic == NVS_MAGIC_V172) {
    g_data = tmp;
    fillDefaultPoses(g_data);
    g_data.magic = NVS_MAGIC;
    Serial.println(F("NVS migrated 0xA172 → wrists-first (fob kept)"));
    return true;
  }
  Serial.println(F("NVS bad magic — defaults"));
  storeDefaults(g_data);
  fillDefaultPoses(g_data);
  return false;
}

bool storeSave() {
  storePullFromServos();
  g_data.magic = NVS_MAGIC;
  size_t w = prefs.putBytes("persist", &g_data, sizeof(g_data));
  bool ok = (w == sizeof(g_data));
  Serial.println(ok ? F("NVS saved") : F("NVS save FAILED"));
  return ok;
}
