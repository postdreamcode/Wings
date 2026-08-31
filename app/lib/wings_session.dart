import 'ble_client.dart';
import 'voice_engine.dart';
import 'wings_native.dart';

/// Process-level owner. Do not create WingsBle inside a page — HomePage
/// dispose must not tear the GATT link while the FGS is running.
class WingsSession {
  WingsSession._();
  static final WingsSession instance = WingsSession._();

  final ble = WingsBle();
  final native = WingsNative();
  late final voice = WingsVoice(native);
}
