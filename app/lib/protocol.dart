/// BLE UUIDs and command IDs — must match firmware PROTOCOL_VERSION / GATT layout.
library;

const int kProtocolVersion = 2;
const String kAppVersion = '1.1.37';

/// This pair's Slave WiFi/ESP-NOW MAC. LINK uses it when Master has never heard
/// a hello (phone on Master is often RX-deaf).
const List<int> kDefaultSlaveMac = [0xD4, 0x05, 0x92, 0x40, 0xC2, 0xD0];

/// Trapezoid accel=decel (firmware RAMP_ACCEL_*). Status offset 28 if blob >= 30.
const int kAccelMsMin = 50;
const int kAccelMsMax = 2000;
const int kAccelMsDefault = 250;

const String kServiceUuid = 'a1700001-0000-1000-8000-00805f9b34fb';
const String kStatusUuid = 'a1700002-0000-1000-8000-00805f9b34fb';
const String kCommandUuid = 'a1700003-0000-1000-8000-00805f9b34fb';
const String kCalUuid = 'a1700004-0000-1000-8000-00805f9b34fb';
const String kFobUuid = 'a1700005-0000-1000-8000-00805f9b34fb';
const String kPoseUuid = 'a1700006-0000-1000-8000-00805f9b34fb';
const String kPeerUuid = 'a1700007-0000-1000-8000-00805f9b34fb';

class SlaveLink {
  static const int none = 0;
  static const int heard = 1;
  static const int live = 2;
  static const int stale = 3;
  static const int paused = 4;

  static String name(int v) {
    switch (v) {
      case heard:
        return 'HEARD';
      case live:
        return 'SLAVE LIVE';
      case stale:
        return 'SLAVE SILENT';
      case paused:
        return 'ESP-NOW PAUSED';
      default:
        return 'SLAVE NONE';
    }
  }
}

String formatMac(List<int> m) {
  if (m.length < 6 || m.every((b) => b == 0)) return '—';
  return m
      .take(6)
      .map((b) => (b & 0xff).toRadixString(16).padLeft(2, '0'))
      .join(':');
}

bool macIsZero(List<int> m) => m.length < 6 || m.take(6).every((b) => b == 0);

const List<String> kServoNames = [
  'WRIST HUG',
  'WRIST RAISE',
  'ELB HUG',
  'ELB RAISE',
  'SHOULDER',
];

const List<String> kServoJobs = [
  'Wrist hug  GPIO5',
  'Wrist raise  GPIO4',
  'Elbow hug  GPIO3',
  'Elbow raise  GPIO2',
  'Shoulder  GPIO1',
];

class CmdId {
  static const int none = 0;
  static const int arm = 1;
  static const int disarm = 2;
  static const int home = 3;
  static const int toggleWing = 4;
  static const int toggleWrist = 5;
  static const int seq = 6;
  static const int setTargets = 7;
  static const int jog = 8;
  static const int poseFolded = 9;
  static const int poseOpen = 10;
  static const int teachPose = 11;
  static const int setSense = 12;
  static const int setSpeed = 13;
  static const int setAccel = 14; // RETIRED; id held so later ids keep value
  static const int stop = 15;
  static const int poseHug = 16;
  static const int setChSpeed = 17; // payload: ch, percent
  static const int armAll = 18; // SH ER EH WR WH, each step is ARM+ch
}

class WingPose {
  static const int closed = 0;
  static const int open = 1;
  static const int hug = 2;

  static String name(int p) {
    switch (p) {
      case open:
        return 'OPEN';
      case hug:
        return 'HUG';
      default:
        return 'CLOSED';
    }
  }
}

class ChannelCal {
  int softMin;
  int softMax;
  int hardMin;
  int hardMax;
  int center;
  int invert;

  ChannelCal({
    this.softMin = 1200,
    this.softMax = 1800,
    this.hardMin = 900,
    this.hardMax = 2100,
    this.center = 1500,
    this.invert = 0,
  });

  /// Packed as firmware ChannelCal: 5×int16 + uint8 + pad → 12 bytes each
  static ChannelCal fromBytes(List<int> b, int offset) {
    int i16(int o) {
      final lo = b[o] & 0xff;
      final hi = b[o + 1] & 0xff;
      var v = lo | (hi << 8);
      if (v >= 0x8000) v -= 0x10000;
      return v;
    }

    return ChannelCal(
      softMin: i16(offset),
      softMax: i16(offset + 2),
      hardMin: i16(offset + 4),
      hardMax: i16(offset + 6),
      center: i16(offset + 8),
      invert: b[offset + 10] & 0xff,
    );
  }

  List<int> toBytes() {
    List<int> i16(int v) => [v & 0xff, (v >> 8) & 0xff];
    return [
      ...i16(softMin),
      ...i16(softMax),
      ...i16(hardMin),
      ...i16(hardMax),
      ...i16(center),
      invert & 0xff,
      0, // pad to 12 (align with typical struct packing)
    ];
  }
}

class ChannelPoses {
  final List<int> closed;
  final List<int> open;
  final List<int> hug;
  final List<int> sense;

  ChannelPoses({
    List<int>? closed,
    List<int>? open,
    List<int>? hug,
    List<int>? sense,
  })  : closed = closed ?? List.filled(5, 1200),
        open = open ?? List.filled(5, 1800),
        hug = hug ?? List.filled(5, 1800),
        sense = sense ?? List.filled(5, 0);

  static ChannelPoses fromBytes(List<int> b) {
    int i16(int o) {
      final lo = b[o] & 0xff;
      final hi = b[o + 1] & 0xff;
      var v = lo | (hi << 8);
      if (v >= 0x8000) v -= 0x10000;
      return v;
    }

    final closed = <int>[];
    final open = <int>[];
    final hug = <int>[];
    final sense = <int>[];
    for (var i = 0; i < 5; i++) {
      closed.add(i16(i * 2));
      open.add(i16(10 + i * 2));
      hug.add(i16(20 + i * 2));
      sense.add(i < b.length - 30 ? b[30 + i] & 0xff : 0);
    }
    return ChannelPoses(closed: closed, open: open, hug: hug, sense: sense);
  }

  List<int> toBytes() {
    List<int> i16(int v) => [v & 0xff, (v >> 8) & 0xff];
    final out = <int>[];
    for (final v in closed) {
      out.addAll(i16(v));
    }
    for (final v in open) {
      out.addAll(i16(v));
    }
    for (final v in hug) {
      out.addAll(i16(v));
    }
    out.addAll(sense.map((s) => s & 0xff));
    return out;
  }
}

class WingsStatus {
  final int proto;
  final int role; // 1 master
  final bool armed;
  final int pose;
  final bool pathActive;
  final bool seq;
  final List<int> target;
  final List<int> actual;
  final int speedPct;
  final int attachMask;
  final int accelMs;
  final bool brakeReady;
  final List<int> chSpeed;
  final int slaveLink;
  final int slaveAgeCs;
  final List<int> localMac;
  final List<int> peerMac;

  WingsStatus({
    required this.proto,
    required this.role,
    required this.armed,
    required this.pose,
    required this.pathActive,
    required this.seq,
    required this.target,
    required this.actual,
    this.speedPct = 10,
    this.attachMask = 0,
    this.accelMs = kAccelMsDefault,
    this.brakeReady = false,
    List<int>? chSpeed,
    this.slaveLink = SlaveLink.none,
    this.slaveAgeCs = 255,
    List<int>? localMac,
    List<int>? peerMac,
  })  : chSpeed = chSpeed ?? List.filled(5, 100),
        localMac = localMac ?? List.filled(6, 0),
        peerMac = peerMac ?? List.filled(6, 0);

  bool get isMaster => role == 1;
  bool get slaveLive => slaveLink == SlaveLink.live;
  bool get canLinkSlave =>
      isMaster &&
      (slaveLink == SlaveLink.heard || slaveLink == SlaveLink.none);
  bool get canUnlinkSlave =>
      isMaster &&
      (slaveLink == SlaveLink.live || slaveLink == SlaveLink.stale);

  bool get isLive => armed || pathActive || attachMask != 0;
  bool get isCold => !brakeReady;

  bool chArmed(int ch) => ch >= 0 && ch < 5 && (attachMask & (1 << ch)) != 0;

  factory WingsStatus.fromBytes(List<int> b) {
    int i16(int o) {
      final lo = b[o] & 0xff;
      final hi = b[o + 1] & 0xff;
      var v = lo | (hi << 8);
      if (v >= 0x8000) v -= 0x10000;
      return v;
    }

    final tgt = <int>[];
    final act = <int>[];
    for (var i = 0; i < 5; i++) {
      tgt.add(i16(6 + i * 2));
      act.add(i16(6 + 10 + i * 2));
    }
    return WingsStatus(
      proto: b[0] & 0xff,
      role: b[1] & 0xff,
      armed: (b[2] & 0xff) != 0,
      pose: b[3] & 0xff,
      pathActive: (b[4] & 0xff) != 0,
      seq: (b[5] & 0xff) != 0,
      target: tgt,
      actual: act,
      speedPct: b.length >= 27 ? (b[26] & 0xff) : 10,
      attachMask: b.length >= 28 ? (b[27] & 0xff) : (b.length >= 3 && (b[2] & 0xff) != 0 ? 0x1f : 0),
      accelMs: b.length >= 30
          ? ((b[28] & 0xff) | ((b[29] & 0xff) << 8))
          : kAccelMsDefault,
      brakeReady: b.length >= 31 && (b[30] & 0xff) != 0,
      chSpeed: [
        for (var i = 0; i < 5; i++)
          b.length >= 36 ? (b[31 + i] & 0xff).clamp(1, 100) : 100,
      ],
      slaveLink: b.length >= 37 ? (b[36] & 0xff) : SlaveLink.none,
      slaveAgeCs: b.length >= 38 ? (b[37] & 0xff) : 255,
      localMac: [
        for (var i = 0; i < 6; i++)
          b.length >= 44 ? (b[38 + i] & 0xff) : 0,
      ],
      peerMac: [
        for (var i = 0; i < 6; i++)
          b.length >= 50 ? (b[44 + i] & 0xff) : 0,
      ],
    );
  }
}
