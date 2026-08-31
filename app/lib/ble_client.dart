import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import 'protocol.dart';

class WingsBle {
  BluetoothDevice? device;
  BluetoothCharacteristic? _status;
  BluetoothCharacteristic? _cmd;
  BluetoothCharacteristic? _cal;
  BluetoothCharacteristic? _fob;
  BluetoothCharacteristic? _pose;
  BluetoothCharacteristic? _peer;
  StreamSubscription<List<int>>? _statusSub;
  StreamSubscription<BluetoothConnectionState>? _connSub;

  WingsStatus? lastStatus;
  final statusController = StreamController<WingsStatus>.broadcast();
  final readyController = StreamController<bool>.broadcast();

  /// User wants the link. False only after DISCONNECT. Screen-off must not clear this.
  bool holdingLink = false;
  String? lastRemoteId;
  Timer? _retryTimer;
  int _backoffMs = 1200;
  bool _binding = false;

  /// GATT command char is bound — do not trust device.isConnected alone.
  bool get isReady => _cmd != null;

  Future<bool> requestPermissions() async {
    final statuses = await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
      Permission.notification,
    ].request();
    return statuses.values.every((s) => s.isGranted || s.isLimited);
  }

  Future<List<ScanResult>> scan({Duration timeout = const Duration(seconds: 5)}) async {
    final found = <ScanResult>[];
    final sub = FlutterBluePlus.scanResults.listen((batch) {
      for (final r in batch) {
        final name = r.advertisementData.advName;
        if (name.startsWith('Wings-') ||
            r.advertisementData.serviceUuids
                .map((u) => u.str.toLowerCase())
                .contains(kServiceUuid.toLowerCase())) {
          if (!found.any((e) => e.device.remoteId == r.device.remoteId)) {
            found.add(r);
          }
        }
      }
    });
    await FlutterBluePlus.startScan(timeout: timeout);
    await Future<void>.delayed(timeout + const Duration(milliseconds: 300));
    await FlutterBluePlus.stopScan();
    await sub.cancel();
    return found;
  }

  Future<void> connect(BluetoothDevice d) async {
    holdingLink = true;
    _retryTimer?.cancel();
    lastRemoteId = d.remoteId.str;
    await _bind(d);
  }

  Future<void> _bind(BluetoothDevice d) async {
    if (_binding) return;
    _binding = true;
    try {
      await _bindInner(d);
    } finally {
      _binding = false;
    }
  }

  Future<void> _bindInner(BluetoothDevice d) async {
    await FlutterBluePlus.stopScan();
    await _release(dropDevice: true);
    try {
      await d.disconnect();
    } catch (_) {}
    await Future<void>.delayed(const Duration(milliseconds: 400));

    device = d;

    Object? lastErr;
    for (var attempt = 0; attempt < 3; attempt++) {
      try {
        // mtu:null — requesting 512 right after connect is a common Android 133.
        await d.connect(
          timeout: const Duration(seconds: 15),
          autoConnect: false,
          mtu: null,
        );
        lastErr = null;
        break;
      } catch (e) {
        lastErr = e;
        try {
          await d.disconnect();
        } catch (_) {}
        if (attempt == 0) {
          try {
            await d.clearGattCache();
          } catch (_) {}
        }
        await Future<void>.delayed(Duration(milliseconds: 700 * (attempt + 1)));
      }
    }
    if (lastErr != null) throw lastErr;
    final services = await d.discoverServices();
    for (final s in services) {
      if (s.uuid != Guid(kServiceUuid)) continue;
      for (final c in s.characteristics) {
        if (c.uuid == Guid(kStatusUuid)) _status = c;
        if (c.uuid == Guid(kCommandUuid)) _cmd = c;
        if (c.uuid == Guid(kCalUuid)) _cal = c;
        if (c.uuid == Guid(kFobUuid)) _fob = c;
        if (c.uuid == Guid(kPoseUuid)) _pose = c;
        if (c.uuid == Guid(kPeerUuid)) _peer = c;
      }
    }
    if (_status == null || _cmd == null) {
      throw StateError('GATT characteristics missing');
    }
    await Future<void>.delayed(const Duration(milliseconds: 300));
    try {
      await d.requestMtu(185);
    } catch (_) {}
    await Future<void>.delayed(const Duration(milliseconds: 200));
    _statusSub = _status!.onValueReceived.listen((v) {
      if (v.length < 26) return;
      final st = WingsStatus.fromBytes(v);
      lastStatus = st;
      statusController.add(st);
    });
    await _status!.setNotifyValue(true);
    // Android often returns false on the first read after a flash/cache clear.
    // Notify is enough; do not fail the whole connect.
    try {
      final raw = await _status!.read();
      if (raw.length >= 26) {
        lastStatus = WingsStatus.fromBytes(raw);
        statusController.add(lastStatus!);
      }
    } catch (_) {}

    _connSub = d.connectionState.listen((s) {
      if (s != BluetoothConnectionState.disconnected) return;
      Future<void>.delayed(const Duration(milliseconds: 400), () {
        if (!holdingLink) return;
        if (device != d) return;
        if (d.isConnected) return;
        _statusSub?.cancel();
        _statusSub = null;
        _status = null;
        _cmd = null;
        _cal = null;
        _fob = null;
        _pose = null;
        _peer = null;
        readyController.add(false);
        _scheduleReconnect();
      });
    });
    _backoffMs = 1200;
    readyController.add(true);
  }

  void _scheduleReconnect() {
    if (!holdingLink) return;
    _retryTimer?.cancel();
    final wait = _backoffMs;
    _backoffMs = (_backoffMs * 2).clamp(1200, 30000);
    _retryTimer = Timer(Duration(milliseconds: wait), () async {
      if (!holdingLink) return;
      final id = lastRemoteId;
      if (id == null) return;
      try {
        await _bind(BluetoothDevice.fromId(id));
      } catch (_) {
        _scheduleReconnect();
      }
    });
  }

  Future<void> _release({required bool dropDevice}) async {
    await _connSub?.cancel();
    _connSub = null;
    await _statusSub?.cancel();
    _statusSub = null;
    _status = null;
    _cmd = null;
    _cal = null;
    _fob = null;
    _pose = null;
    _peer = null;
    if (!dropDevice) return;
    final d = device;
    device = null;
    if (d != null) {
      try {
        await d.disconnect();
      } catch (_) {}
    }
  }

  /// User DISCONNECT — stops reconnect and tears GATT. Do not call on pause.
  Future<void> disconnect() async {
    holdingLink = false;
    _retryTimer?.cancel();
    _retryTimer = null;
    _backoffMs = 1200;
    await _release(dropDevice: true);
    readyController.add(false);
  }

  Future<void> sendCmd(int cmd, [List<int>? payload]) async {
    if (_cmd == null) {
      throw StateError('Not connected');
    }
    final bytes = <int>[cmd & 0xff, ...?payload];
    try {
      await _cmd!.write(bytes, withoutResponse: false);
    } catch (_) {
      await _cmd!.write(bytes, withoutResponse: true);
    }
  }

  Future<void> jog(int ch, int deltaUs) async {
    final bd = ByteData(3);
    bd.setUint8(0, ch & 0xff);
    bd.setInt16(1, deltaUs, Endian.little);
    await sendCmd(CmdId.jog, bd.buffer.asUint8List());
  }

  Future<List<ChannelCal>> readCal() async {
    if (_cal == null) return [];
    final v = await _cal!.read();
    final out = <ChannelCal>[];
    for (var i = 0; i < 5; i++) {
      final o = i * 12;
      if (o + 12 > v.length) break;
      out.add(ChannelCal.fromBytes(v, o));
    }
    return out;
  }

  Future<void> writeCal(List<ChannelCal> cals) async {
    if (_cal == null || cals.length != 5) return;
    final bytes = <int>[];
    for (final c in cals) {
      bytes.addAll(c.toBytes());
    }
    await _cal!.write(bytes, withoutResponse: false);
  }

  Future<void> writeFobMap(List<int> maps) async {
    if (_fob == null || maps.length != 4) return;
    final cur = await _fob!.read();
    final bytes = List<int>.from(cur.length >= 36 ? cur : List.filled(36, 0));
    if (cur.length >= 20 && cur.length < 36) {
      // legacy 20-byte: don't copy map bytes into r2
      for (var i = 0; i < 16; i++) {
        bytes[i] = cur[i];
      }
    }
    for (var i = 0; i < 4; i++) {
      bytes[32 + i] = maps[i] & 0xff;
    }
    await _fob!.write(bytes, withoutResponse: false);
  }

  Future<List<int>> readFobMaps() async {
    if (_fob == null) return [4, 5, 6, 3];
    final v = await _fob!.read();
    if (v.length >= 36) return [v[32], v[33], v[34], v[35]];
    if (v.length >= 20) return [v[16], v[17], v[18], v[19]];
    return [4, 5, 6, 3];
  }

  Future<ChannelPoses?> readPoses() async {
    if (_pose == null) return null;
    final v = await _pose!.read();
    if (v.length < 35) return null;
    return ChannelPoses.fromBytes(v);
  }

  Future<void> writePeerMac(List<int> mac) async {
    if (_peer == null || mac.length < 6) {
      throw StateError('Peer characteristic missing — flash 0.2.52+');
    }
    await _peer!.write(mac.take(6).toList(), withoutResponse: false);
  }

  Future<void> clearPeerMac() async {
    await writePeerMac(List.filled(6, 0));
  }

  Future<void> writePoses(ChannelPoses poses) async {
    if (_pose == null) return;
    await _pose!.write(poses.toBytes(), withoutResponse: false);
  }

  Future<void> teachPose(int ch, int pose) async {
    await sendCmd(CmdId.teachPose, [ch & 0xff, pose & 0xff]);
  }

  Future<void> setSense(int ch, bool flip) async {
    await sendCmd(CmdId.setSense, [ch & 0xff, flip ? 1 : 0]);
  }

  Future<void> setSpeed(int pct) async {
    var p = pct;
    if (p < 1) p = 1;
    if (p > 100) p = 100;
    await sendCmd(CmdId.setSpeed, [p & 0xff]);
  }

  Future<void> setChSpeed(int ch, int pct) async {
    var p = pct;
    if (p < 1) p = 1;
    if (p > 100) p = 100;
    await sendCmd(CmdId.setChSpeed, [ch & 0xff, p & 0xff]);
  }

  Future<void> setAccel(int ms) async {
    var v = ms;
    if (v < kAccelMsMin) v = kAccelMsMin;
    if (v > kAccelMsMax) v = kAccelMsMax;
    await sendCmd(CmdId.setAccel, [v & 0xff, (v >> 8) & 0xff]);
  }

  void dispose() {
    holdingLink = false;
    _retryTimer?.cancel();
    statusController.close();
    disconnect();
  }
}
