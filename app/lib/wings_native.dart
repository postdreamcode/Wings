import 'package:flutter/services.dart';

/// Android MethodChannel: FGS, last BLE id, Samsung battery screens.
class WingsNative {
  static const _ch = MethodChannel('wings/native');
  static const mic = EventChannel('wings/mic');

  WingsNative() {
    _ch.setMethodCallHandler(_onNative);
  }

  void Function()? onParked;
  void Function()? onDisconnect;
  void Function()? onResumeListen;

  Future<dynamic> _onNative(MethodCall call) async {
    switch (call.method) {
      case 'parked':
        onParked?.call();
        return;
      case 'disconnect':
        onDisconnect?.call();
        return;
      case 'resumeListen':
        onResumeListen?.call();
        return;
    }
  }

  Future<String> dataDir() async {
    return await _ch.invokeMethod<String>('dataDir') ?? '';
  }

  Future<void> startFg(String text, {bool mic = false}) async {
    await _ch.invokeMethod('startFg', {'text': text, 'mic': mic});
  }

  Future<void> stopFg() async {
    await _ch.invokeMethod('stopFg');
  }

  Future<void> updateFg(String text, {bool mic = false}) async {
    await _ch.invokeMethod('updateFg', {'text': text, 'mic': mic});
  }

  Future<void> saveLastId(String id) async {
    await _ch.invokeMethod('saveLastId', {'id': id});
  }

  Future<String?> getLastId() async {
    return _ch.invokeMethod<String>('getLastId');
  }

  Future<bool> ignoringBattery() async {
    return await _ch.invokeMethod<bool>('ignoringBattery') ?? false;
  }

  Future<void> openBatteryExemption() async {
    await _ch.invokeMethod('openBatteryExemption');
  }

  Future<void> openSamsungNeverSleep() async {
    await _ch.invokeMethod('openSamsungNeverSleep');
  }

  Future<void> openAppDetails() async {
    await _ch.invokeMethod('openAppDetails');
  }

  Future<void> openNotificationSettings() async {
    await _ch.invokeMethod('openNotificationSettings');
  }

  Future<void> openBluetoothSettings() async {
    await _ch.invokeMethod('openBluetoothSettings');
  }

  /// Earpiece/HFP beep: `wake`, `ok`, or `no`.
  Future<void> chime(String kind) async {
    await _ch.invokeMethod('chime', {'kind': kind});
  }

  /// Active capture path: headset product name, `headset`, or `phone`.
  Future<String> micRoute() async {
    return await _ch.invokeMethod<String>('micRoute') ?? 'phone';
  }

  /// Stop AudioRecord, force-stop SCO, stop microphone FGS.
  Future<void> park() async {
    await _ch.invokeMethod('park');
  }
}
