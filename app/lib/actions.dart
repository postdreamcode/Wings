import 'protocol.dart';

/// Single map from operator intent to CmdId.
/// Voice, RUN STOP, and later test-fire use this. A/B/C stay toggles.
class WingsActions {
  WingsActions(this._send);

  final Future<void> Function(int cmd, [List<int>? payload]) _send;

  Future<void> open() => _send(CmdId.poseOpen);
  Future<void> close() => _send(CmdId.poseFolded);
  Future<void> hug() => _send(CmdId.poseHug);
  Future<void> home() => _send(CmdId.home);
  Future<void> stop() => _send(CmdId.stop);
  Future<void> flap() => _send(CmdId.seq);

  Future<void> arm(int ch) => _send(CmdId.arm, [ch & 0xff]);

  Future<void> disarm(int ch) => _send(CmdId.disarm, [ch & 0xff]);
}
