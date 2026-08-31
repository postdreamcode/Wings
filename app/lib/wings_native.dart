import 'package:flutter/services.dart';

/// Android MethodChannel: FGS, last BLE id, Samsung battery screens.
class WingsNative {
  static const _ch = MethodChannel('wings/native');
  static const mic = EventChannel('wings/mic');

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
}
