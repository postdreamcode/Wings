import 'dart:async';
import 'dart:io';
import 'dart:math';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:sherpa_onnx/sherpa_onnx.dart' as so;

import 'voice_store.dart';
import 'wings_native.dart';

class VoiceHit {
  VoiceHit({
    required this.keyword,
    required this.speaker,
    required this.score,
    required this.speakerOk,
  });

  final String keyword; // open|close|hug|home|stop|flap or ''
  final String speaker;
  final double score;
  final bool speakerOk;
}

/// On-device sherpa-onnx KWS + speaker verify. No cloud, no AccessKey.
class WingsVoice {
  WingsVoice(this._native);

  final WingsNative _native;
  VoiceStore? store;
  so.KeywordSpotter? _spotter;
  so.SpeakerEmbeddingExtractor? _ex;
  so.SpeakerEmbeddingManager? _mgr;
  bool ready = false;
  String error = '';
  final lastHit = ValueNotifier<VoiceHit?>(null);

  void Function(VoiceHit hit)? onLiveHit;

  StreamSubscription<dynamic>? _micSub;
  so.OnlineStream? _wakeStream;
  so.OnlineStream? _cmdStream;
  final _pcm = <double>[];
  String _heard = '';
  bool _wake = false;
  bool _always = false;
  bool _ptt = false;
  bool _spkPending = false;
  DateTime? _cmdUntil;
  Timer? _cmdWatch;
  bool _cue = true;
  String _kwWake = '';
  String _kwCmd = '';
  static const _cmdWindow = Duration(milliseconds: 4000);
  final lastKw = ValueNotifier<String>('');
  final micRoute = ValueNotifier<String>('');
  static const _ringN = 32000; // 2s @ 16 kHz
  final _ring = Float32List(_ringN);
  int _ringI = 0;
  int _ringLen = 0;

  static const _assets = <String, String>{
    'assets/sherpa/encoder.int8.onnx': 'encoder.int8.onnx',
    'assets/sherpa/decoder.int8.onnx': 'decoder.int8.onnx',
    'assets/sherpa/joiner.int8.onnx': 'joiner.int8.onnx',
    'assets/sherpa/tokens.txt': 'tokens.txt',
    'assets/sherpa/keywords.txt': 'keywords.txt',
    'assets/sherpa/campplus.onnx': 'campplus.onnx',
  };

  Future<void> init() async {
    if (ready) return;
    try {
      await so.initBindingsAsync();
      final dir = await _native.dataDir();
      if (dir.isEmpty) {
        error = 'dataDir empty';
        return;
      }
      final modelDir = '$dir/sherpa';
      await Directory(modelDir).create(recursive: true);
      final paths = <String, String>{};
      for (final e in _assets.entries) {
        // keywords.txt is tiny and changes; never keep a stale copy.
        final force = e.value == 'keywords.txt';
        paths[e.value] =
            await _copyAsset(e.key, '$modelDir/${e.value}', force: force);
      }
      store = VoiceStore('$dir/voice_profiles.json');
      await store!.load();

      _spotter = so.KeywordSpotter(
        so.KeywordSpotterConfig(
          model: so.OnlineModelConfig(
            transducer: so.OnlineTransducerModelConfig(
              encoder: paths['encoder.int8.onnx']!,
              decoder: paths['decoder.int8.onnx']!,
              joiner: paths['joiner.int8.onnx']!,
            ),
            tokens: paths['tokens.txt']!,
            numThreads: 2,
            provider: 'cpu',
          ),
          keywordsFile: paths['keywords.txt']!,
          keywordsScore: 1.5,
          keywordsThreshold: 0.15,
          numTrailingBlanks: 1,
        ),
      );
      final kwLines = await File(paths['keywords.txt']!).readAsLines();
      _kwWake = kwLines
          .map((l) => l.trim())
          .where((l) => l.contains('@wake'))
          .join('\n');
      _kwCmd = kwLines
          .map((l) => l.trim())
          .where((l) => l.contains('@c_'))
          .join('\n');
      _ex = so.SpeakerEmbeddingExtractor(
        config: so.SpeakerEmbeddingExtractorConfig(
          model: paths['campplus.onnx']!,
          numThreads: 1,
          provider: 'cpu',
        ),
      );
      _rebuildManager();
      ready = true;
    } catch (e) {
      error = '$e';
      ready = false;
    }
  }

  void _rebuildManager() {
    _mgr?.free();
    final dim = _ex?.dim ?? 0;
    if (dim < 1) return;
    _mgr = so.SpeakerEmbeddingManager(dim);
    for (final p in store?.profiles ?? []) {
      if (p.embeddings.isEmpty) continue;
      _mgr!.addMulti(name: p.name, embeddingList: p.embeddings);
    }
  }

  Future<String> _copyAsset(String asset, String dest, {bool force = false}) async {
    final f = File(dest);
    if (!force && await f.exists() && await f.length() > 1000) return dest;
    final data = await rootBundle.load(asset);
    await f.writeAsBytes(data.buffer.asUint8List(), flush: true);
    return dest;
  }

  bool get isAlways => _always;
  bool get isPtt => _ptt;
  bool get listenPaused => _listenPaused;
  bool _listenPaused = false;

  Future<void> startAlways() async {
    if (!ready || _spotter == null) return;
    if (_always && !_listenPaused && _micSub != null) return;
    _always = true;
    _cue = true;
    _ptt = false;
    _listenPaused = false;
    _heard = '';
    _wake = false;
    _cmdUntil = null;
    _spkPending = false;
    _disarmCmdWatch();
    await _ensureMic();
  }

  Future<void> stopAlways() async {
    _always = false;
    _listenPaused = false;
    _cmdUntil = null;
    _disarmCmdWatch();
    if (!_ptt) await _stopMic(commit: false);
    micRoute.value = '';
  }

  /// Mute KWS while servos run. Keep the headset mic/SCO up. No-op if not always-listen.
  Future<void> pauseListen() async {
    if (!_always || _ptt || _listenPaused) return;
    _listenPaused = true;
    _enterIdle();
    _disarmCmdWatch();
  }

  Future<void> resumeListen() async {
    if (!_always || !_listenPaused || _ptt) return;
    _listenPaused = false;
    if (_micSub == null) await _ensureMic();
  }

  void setCue(bool on) => _cue = on;

  Future<void> startPtt({bool cue = true}) async {
    if (!ready || _spotter == null) return;
    _ptt = true;
    _cue = cue;
    _pcm.clear();
    _heard = '';
    _wake = false;
    _cmdUntil = null;
    _listenPaused = false;
    _disarmCmdWatch();
    await _ensureMic();
  }

  void _ping(String kind) {
    // Mute only while an enroll hold is in progress, not a leftover flag.
    if (_ptt && !_cue) return;
    unawaited(_chimeSafe(kind));
  }

  Future<void> testChimes() async {
    await _chimeSafe('wake');
    await Future<void>.delayed(const Duration(milliseconds: 350));
    await _chimeSafe('ok');
    await Future<void>.delayed(const Duration(milliseconds: 350));
    await _chimeSafe('no');
  }

  Future<void> _chimeSafe(String kind) async {
    try {
      await _native.chime(kind);
    } catch (_) {}
  }

  void _armCmdWatch() {
    _cmdWatch?.cancel();
    _cmdWatch = Timer(_cmdWindow, () {
      if (!_wake && !_cmdOpen) return;
      _wake = false;
      _cmdUntil = null;
      _heard = '';
      _spkPending = false;
      _enterIdle();
      _ping('no');
      lastHit.value = VoiceHit(
        keyword: '',
        speaker: lastHit.value?.speaker ?? '',
        score: lastHit.value?.score ?? 0,
        speakerOk: lastHit.value?.speakerOk ?? false,
      );
    });
  }

  void _disarmCmdWatch() {
    _cmdWatch?.cancel();
    _cmdWatch = null;
  }

  Future<void> _ensureMic() async {
    if (_micSub != null) return;
    _openStreams();
    _ringI = 0;
    _ringLen = 0;
    _micSub = WingsNative.mic.receiveBroadcastStream().listen(_onPcm);
    unawaited(_refreshMicRoute());
  }

  Future<void> _refreshMicRoute() async {
    await Future<void>.delayed(const Duration(milliseconds: 900));
    try {
      micRoute.value = await _native.micRoute();
    } catch (_) {}
  }

  void _enterIdle() {
    _wake = false;
    _cmdUntil = null;
    _spkPending = false;
    _heard = '';
  }

  void _openStreams() {
    final spot = _spotter;
    if (spot == null) return;
    _wakeStream?.free();
    _cmdStream?.free();
    try {
      _wakeStream = _kwWake.isEmpty
          ? spot.createStream()
          : spot.createStream(keywords: _kwWake);
      _cmdStream = _kwCmd.isEmpty
          ? spot.createStream()
          : spot.createStream(keywords: _kwCmd);
    } catch (_) {
      _wakeStream = spot.createStream();
      _cmdStream = spot.createStream();
    }
  }

  Float32List _ringTail(int n) {
    final take = min(n, _ringLen);
    if (take < 1600) return Float32List(0);
    final out = Float32List(take);
    var i = (_ringI - take + _ringN) % _ringN;
    for (var k = 0; k < take; k++) {
      out[k] = _ring[i];
      i = (i + 1) % _ringN;
    }
    return out;
  }

  Future<void> _stopMic({required bool commit}) async {
    await _micSub?.cancel();
    _micSub = null;
    final spot = _spotter;
    if (commit && spot != null) {
      for (final s in [_wakeStream, _cmdStream]) {
        if (s == null) continue;
        s.acceptWaveform(samples: Float32List(8000), sampleRate: 16000);
        while (spot.isReady(s)) {
          spot.decode(s);
          final kw = spot.getResult(s).keyword;
          if (kw.isNotEmpty) {
            lastKw.value = kw;
            _acceptKw(kw);
          }
        }
      }
    }
    _wakeStream?.free();
    _cmdStream?.free();
    _wakeStream = null;
    _cmdStream = null;
  }

  void _pushRing(Float32List f) {
    for (var i = 0; i < f.length; i++) {
      _ring[_ringI] = f[i];
      _ringI = (_ringI + 1) % _ringN;
      if (_ringLen < _ringN) _ringLen++;
    }
  }

  Float32List _ringSnap() {
    if (_ringLen < 16000) return Float32List(0);
    final n = _ringLen;
    final out = Float32List(n);
    var i = (_ringI - n + _ringN) % _ringN;
    for (var k = 0; k < n; k++) {
      out[k] = _ring[i];
      i = (i + 1) % _ringN;
    }
    return out;
  }

  void _onPcm(dynamic raw) {
    if (_listenPaused && !_ptt) return;
    final bytes = raw is Uint8List ? raw : Uint8List.fromList(List<int>.from(raw as List));
    final n = bytes.length ~/ 2;
    final f = Float32List(n);
    final bd = ByteData.sublistView(bytes);
    for (var i = 0; i < n; i++) {
      f[i] = bd.getInt16(i * 2, Endian.little) / 32768.0;
    }
    _pushRing(f);
    if (_ptt) _pcm.addAll(f);
    final spot = _spotter;
    if (spot == null) return;
    final wake = _wakeStream;
    final cmd = _cmdStream;
    if (wake != null) _feed(spot, wake, f, wakeSide: true);
    final wantCmd = !_always || _ptt || _wake || _cmdOpen;
    if (cmd != null && wantCmd) _feed(spot, cmd, f, wakeSide: false);
    if (_always && !_ptt && _heard.isNotEmpty) {
      if (_spkPending) return;
      if (_wake || _cmdOpen) {
        _emitAlways();
      } else {
        lastHit.value = VoiceHit(
          keyword: _heard,
          speaker: lastHit.value?.speaker ?? '',
          score: lastHit.value?.score ?? 0,
          speakerOk: false,
        );
        _heard = '';
      }
    }
  }

  void _feed(
    so.KeywordSpotter spot,
    so.OnlineStream stream,
    Float32List f, {
    required bool wakeSide,
  }) {
    stream.acceptWaveform(samples: f, sampleRate: 16000);
    while (spot.isReady(stream)) {
      spot.decode(stream);
      final kw = spot.getResult(stream).keyword;
      if (kw.isEmpty) continue;
      lastKw.value = kw;
      if (wakeSide) {
        if (_isWakeKw(kw)) {
          _acceptWake();
          spot.reset(stream);
        }
      } else {
        var s = kw.trim().toLowerCase();
        if (s.startsWith('c_')) s = s.substring(2);
        if (_cmds.contains(s)) {
          _heard = s;
          spot.reset(stream);
        }
      }
    }
  }

  static const _cmds = ['open', 'close', 'hug', 'home', 'stop', 'flap'];

  static bool _isWakeKw(String raw) {
    final s = raw.trim().toLowerCase();
    return s == 'wake' || s == 'valkyrie';
  }

  bool get wakeOpen => _wake || _cmdOpen;

  bool get _cmdOpen {
    final u = _cmdUntil;
    return u != null && DateTime.now().isBefore(u);
  }

  VoiceHit _scoreTail({String keyword = ''}) {
    var pcm = _ringTail(19200).toList();
    if (pcm.length < 8000) {
      return VoiceHit(
        keyword: keyword,
        speaker: '',
        score: 0,
        speakerOk: store?.debugBypass == true,
      );
    }
    if (pcm.length < 16000) {
      pcm.addAll(List<double>.filled(16000 - pcm.length, 0.0));
    }
    final emb = _embed(pcm);
    var speaker = '';
    var score = 0.0;
    var ok = store?.debugBypass == true;
    if (emb != null && store != null) {
      final best = _bestSpeaker(emb);
      speaker = best.$1;
      score = best.$2;
      ok = store!.debugBypass ||
          (speaker.isNotEmpty && score >= store!.threshold);
    }
    return VoiceHit(
      keyword: keyword,
      speaker: speaker,
      score: score,
      speakerOk: ok,
    );
  }

  void _acceptWake() {
    final already = _wake || _cmdOpen;
    _wake = true;
    _cmdUntil = DateTime.now().add(_cmdWindow);
    if (!_always) {
      lastHit.value = VoiceHit(
        keyword: 'valkyrie',
        speaker: lastHit.value?.speaker ?? '',
        score: lastHit.value?.score ?? 0,
        speakerOk: true,
      );
      _ping('wake');
      return;
    }
    _armCmdWatch();
    if (already) return;
    final c = _cmdStream;
    final spot = _spotter;
    if (c != null && spot != null) {
      try {
        spot.reset(c);
      } catch (_) {}
    }
    _spkPending = true;
    unawaited(_verifyWake());
  }

  Future<void> _verifyWake() async {
    await Future<void>.delayed(Duration.zero);
    if (!_wake) return;
    final gate = _scoreTail(keyword: 'valkyrie');
    if (!_wake) return;
    lastHit.value = gate;
    _spkPending = false;
    if (!gate.speakerOk) {
      _enterIdle();
      _disarmCmdWatch();
      _ping('no');
      return;
    }
    _ping('wake');
    if (_always && !_ptt && _heard.isNotEmpty) _emitAlways();
  }

  void _acceptKw(String raw) {
    var s = raw.trim().toLowerCase();
    if (_isWakeKw(s)) {
      _acceptWake();
      return;
    }
    if (s.startsWith('c_')) s = s.substring(2);
    if (!_cmds.contains(s)) return;
    _heard = s;
  }

  static String? _cmdFromRaw(String raw) {
    var s = raw.trim().toLowerCase();
    if (s.startsWith('c_')) s = s.substring(2);
    return _cmds.contains(s) ? s : null;
  }

  void _emitAlways() {
    if (_spkPending) return;
    final cmd = _heard;
    _heard = '';
    _wake = false;
    _cmdUntil = null;
    _disarmCmdWatch();
    if (cmd.isEmpty) return;
    final prev = lastHit.value;
    final hit = VoiceHit(
      keyword: cmd,
      speaker: prev?.speaker ?? '',
      score: prev?.score ?? 0,
      speakerOk: true,
    );
    lastHit.value = hit;
    _ping('ok');
    onLiveHit?.call(hit);
  }

  Future<VoiceHit?> stopPtt({bool commit = true}) async {
    // Keep _ptt armed through the end-of-utterance flush. KWS often fires
    // on the trailing silence, after the finger is already up.
    if (_always) {
      if (!commit) {
        _ptt = false;
        return null;
      }
      await Future<void>.delayed(const Duration(milliseconds: 250));
    } else {
      await _stopMic(commit: commit);
      if (!commit) {
        _ptt = false;
        return null;
      }
    }
    _ptt = false;
    _cue = true;
    final hadWake = _wake || _cmdOpen;
    _wake = false;
    _cmdUntil = null;
    _disarmCmdWatch();

    var cmd = _heard;
    if (cmd.isEmpty) cmd = _cmdFromRaw(lastKw.value) ?? '';
    _heard = '';

    List<double> pcm = _pcm;
    if (pcm.length < 16000) pcm = _ringSnap().toList();
    final emb = _embed(pcm);
    var speaker = '';
    var score = 0.0;
    var ok = store?.debugBypass == true;
    if (emb != null && store != null) {
      final best = _bestSpeaker(emb);
      speaker = best.$1;
      score = best.$2;
      ok = store!.debugBypass ||
          (speaker.isNotEmpty && score >= store!.threshold);
    }
    final hit = VoiceHit(
      keyword: cmd,
      speaker: speaker,
      score: score,
      speakerOk: ok,
    );
    lastHit.value = hit;
    if (cmd.isEmpty) {
      if (hadWake) _ping('no');
    } else {
      _ping(ok ? 'ok' : 'no');
    }
    return hit;
  }

  Float32List? _embed(List<double> pcm) {
    final ex = _ex;
    if (ex == null || pcm.length < 16000) return null; // need ~1s
    final s = ex.createStream();
    s.acceptWaveform(
      samples: Float32List.fromList(pcm),
      sampleRate: 16000,
    );
    s.acceptWaveform(samples: Float32List(8000), sampleRate: 16000);
    if (!ex.isReady(s)) {
      s.free();
      return null;
    }
    final e = ex.compute(s);
    s.free();
    return e.isEmpty ? null : e;
  }

  (String, double) _bestSpeaker(Float32List emb) {
    var name = '';
    var best = -1.0;
    for (final p in store?.profiles ?? []) {
      for (final ref in p.embeddings) {
        if (ref.length != emb.length) continue;
        final c = _cosine(emb, ref);
        if (c > best) {
          best = c;
          name = p.name;
        }
      }
    }
    return (name, best);
  }

  double _cosine(Float32List a, Float32List b) {
    var dot = 0.0, na = 0.0, nb = 0.0;
    for (var i = 0; i < a.length; i++) {
      dot += a[i] * b[i];
      na += a[i] * a[i];
      nb += b[i] * b[i];
    }
    final d = sqrt(na) * sqrt(nb);
    return d == 0 ? 0 : dot / d;
  }

  /// Campplus wants ~1s. Short "open"/"stop" holds are padded; under 0.5s is rejected.
  Future<Float32List?> enrollClip() async {
    var pcm = List<double>.from(_pcm);
    if (pcm.length < 8000) return null;
    if (pcm.length < 16000) {
      pcm.addAll(List<double>.filled(16000 - pcm.length, 0.0));
    }
    return _embed(pcm);
  }

  Future<bool> addProfile(String name, List<Float32List> embeds) async {
    if (store == null || embeds.isEmpty) return false;
    final i = store!.profiles.indexWhere((p) => p.name == name);
    if (i >= 0) {
      store!.profiles[i] = VoiceProfile(name: name, embeddings: embeds);
    } else {
      if (store!.profiles.length >= 2) return false;
      store!.profiles.add(VoiceProfile(name: name, embeddings: embeds));
    }
    try {
      await store!.save();
    } catch (_) {
      return false;
    }
    _rebuildManager();
    return true;
  }

  Future<void> deleteProfile(String name) async {
    store?.profiles.removeWhere((p) => p.name == name);
    await store?.save();
    _rebuildManager();
  }

  bool get enrolled => (store?.profiles.isNotEmpty ?? false);
}
