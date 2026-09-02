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
  bool _oneshot = false;
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
  static const _chimeDuck = Duration(milliseconds: 400);
  static const _vadFrame = 320; // 20 ms @ 16 kHz
  static const _minWakeSpeech = 9600; // 0.6 s
  static const _maxWakeSpeech = 16000; // 1.0 s
  static const _minEnrollSpeech = 11200; // 0.7 s
  DateTime? _kwsDuckUntil;
  Float32List? _spkClip;
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
          keywordsScore: 2.5,
          keywordsThreshold: 0.10,
          numTrailingBlanks: 1,
          maxActivePaths: 8,
        ),
      );
      final kwLines = await File(paths['keywords.txt']!).readAsLines();
      _kwWake = kwLines
          .map((l) => l.trim())
          .where((l) => l.contains('@wake'))
          .join('\n');
      _kwCmd = kwLines
          .map((l) => l.trim())
          .where((l) => l.contains('@c_') || l.contains('@v_'))
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
  final armed = ValueNotifier<bool>(false);

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
    armed.value = true;
    await _ensureMic();
  }

  /// Stop the listen session. Does not clear [VoiceStore.alwaysListen].
  Future<void> stopAlways({bool nativePark = true}) async {
    _always = false;
    _listenPaused = false;
    _cmdUntil = null;
    _disarmCmdWatch();
    armed.value = false;
    if (!_ptt) await _stopMic(commit: false);
    if (nativePark) {
      try {
        await _native.park();
      } catch (_) {}
    }
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

  bool get isPhoneMic => micRoute.value.trim().toLowerCase() == 'phone';

  Future<String> waitMicRoute() async {
    await _ensureMic();
    await Future<void>.delayed(const Duration(milliseconds: 900));
    return peekMicRoute();
  }

  Future<String> peekMicRoute() async {
    try {
      micRoute.value = await _native.micRoute();
    } catch (_) {}
    return micRoute.value;
  }

  /// Stop capture if we only started it to read MIC route.
  Future<void> dropMicIfIdle() async {
    if (_always || _ptt) return;
    await _stopMic(commit: false);
  }

  void _ping(String kind) {
    // Mute only while an enroll hold is in progress, not a leftover flag.
    if (_ptt && !_cue) return;
    _kwsDuckUntil = DateTime.now().add(_chimeDuck);
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
    _oneshot = false;
    _spkClip = null;
  }

  /// Open both beams once per mic session. Never rebuild on wake.
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
    if (_kwsDuckUntil != null && DateTime.now().isBefore(_kwsDuckUntil!)) {
      return;
    }
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
    if (cmd != null) _feed(spot, cmd, f, wakeSide: false);
    if (_always && !_ptt && _heard.isNotEmpty) {
      if (_spkPending) return;
      if (_oneshot) {
        _spkPending = true;
        unawaited(_verifyOneshot());
        return;
      }
      if (_wake || _cmdOpen) {
        _emitAlways();
      } else {
        _heard = '';
      }
    }
  }

  void _noteKw(String kw) {
    final prev = lastKw.value;
    if (prev.isEmpty) {
      lastKw.value = kw;
      return;
    }
    final last = prev.split(' → ').last;
    lastKw.value = last == kw ? kw : '$last → $kw';
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
      if (wakeSide) {
        if (_isWakeKw(kw)) {
          _noteKw(kw);
          _acceptWake();
        }
        spot.reset(stream);
      } else {
        var s = kw.trim().toLowerCase();
        var shot = false;
        if (s.startsWith('v_')) {
          s = s.substring(2);
          shot = true;
        } else if (s.startsWith('c_')) {
          s = s.substring(2);
          if (!_ptt && !_wake && !_cmdOpen) {
            spot.reset(stream);
            continue;
          }
        }
        if (_cmds.contains(s)) {
          _noteKw(kw);
          _heard = s;
          _oneshot = shot;
          if (shot) _spkClip = _wakeSpeechClip();
        }
        spot.reset(stream);
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

  /// Last speech island in ~1.5s, first 0.6–1.0s (wake, not trailing "open").
  Float32List _wakeSpeechClip() {
    return _vadTrim(_ringTail(24000), fromStart: true);
  }

  int _energySamples(List<double> pcm) {
    var n = 0;
    for (var i = 0; i + _vadFrame <= pcm.length; i += _vadFrame) {
      var e = 0.0;
      for (var j = 0; j < _vadFrame; j++) {
        final x = pcm[i + j];
        e += x * x;
      }
      if (e >= _vadFrame * 4e-5) n += _vadFrame;
    }
    return n;
  }

  Float32List _vadTrim(Float32List pcm, {required bool fromStart}) {
    if (pcm.isEmpty) return Float32List(0);
    final n = pcm.length;
    final frames = n ~/ _vadFrame;
    if (frames < 1) return Float32List(0);
    final energy = List<double>.filled(frames, 0.0);
    var maxE = 0.0;
    for (var f = 0; f < frames; f++) {
      var e = 0.0;
      final off = f * _vadFrame;
      for (var j = 0; j < _vadFrame; j++) {
        final x = pcm[off + j];
        e += x * x;
      }
      energy[f] = e;
      if (e > maxE) maxE = e;
    }
    final floor = _vadFrame * 4e-5;
    if (maxE < floor) return Float32List(0);
    final thr = max(maxE * 0.08, floor);
    var end = -1;
    for (var f = frames - 1; f >= 0; f--) {
      if (energy[f] >= thr) {
        end = f;
        break;
      }
    }
    if (end < 0) return Float32List(0);
    var start = end;
    while (start > 0 && energy[start - 1] >= thr) {
      start--;
    }
    var a = start * _vadFrame;
    var b = min(n, (end + 1) * _vadFrame);
    if (b - a > _maxWakeSpeech) {
      if (fromStart) {
        b = a + _maxWakeSpeech;
      } else {
        a = b - _maxWakeSpeech;
      }
    }
    final len = b - a;
    final out = Float32List(len);
    for (var i = 0; i < len; i++) {
      out[i] = pcm[a + i];
    }
    return out;
  }

  VoiceHit _scoreClip(Float32List? clip, {String keyword = ''}) {
    final bypass = store?.debugBypass == true;
    final raw = clip ?? Float32List(0);
    if (_energySamples(raw) < _minWakeSpeech) {
      return VoiceHit(
        keyword: keyword,
        speaker: '',
        score: 0,
        speakerOk: bypass,
      );
    }
    var pcm = raw.toList();
    if (pcm.length < 16000) {
      pcm.addAll(List<double>.filled(16000 - pcm.length, 0.0));
    } else if (pcm.length > 16000) {
      pcm = pcm.sublist(0, 16000);
    }
    final emb = _embed(pcm);
    var speaker = '';
    var score = 0.0;
    var ok = bypass;
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
    if (!already) _spkClip = _wakeSpeechClip();
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
    _spkPending = true;
    unawaited(_verifyWake());
  }

  Future<void> _verifyWake() async {
    await Future<void>.delayed(Duration.zero);
    if (!_wake) return;
    final gate = _scoreClip(_spkClip, keyword: 'valkyrie');
    if (!_wake) return;
    lastHit.value = gate;
    _spkPending = false;
    if (!gate.speakerOk) {
      _enterIdle();
      _disarmCmdWatch();
      _ping('no');
      return;
    }
    if (_always && !_ptt && _heard.isNotEmpty) {
      _emitAlways();
      return;
    }
    _ping('wake');
  }

  Future<void> _verifyOneshot() async {
    await Future<void>.delayed(Duration.zero);
    final cmd = _heard;
    _oneshot = false;
    _spkPending = false;
    if (cmd.isEmpty) return;
    final gate = _scoreClip(_spkClip, keyword: cmd);
    if (!gate.speakerOk) {
      _heard = '';
      lastHit.value = gate;
      _ping('no');
      return;
    }
    lastHit.value = VoiceHit(
      keyword: cmd,
      speaker: gate.speaker,
      score: gate.score,
      speakerOk: true,
    );
    _emitAlways();
  }

  void _acceptKw(String raw) {
    var s = raw.trim().toLowerCase();
    if (_isWakeKw(s)) {
      _acceptWake();
      return;
    }
    if (s.startsWith('c_') || s.startsWith('v_')) s = s.substring(2);
    if (!_cmds.contains(s)) return;
    _heard = s;
  }

  static String? _cmdFromRaw(String raw) {
    var s = raw.trim().toLowerCase();
    if (s.contains(' → ')) s = s.split(' → ').last;
    if (s.startsWith('c_') || s.startsWith('v_')) s = s.substring(2);
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

  /// Campplus wants ~1s. Reject clips with under ~0.7s of real energy (zero-pad is not speech).
  Future<Float32List?> enrollClip() async {
    var pcm = List<double>.from(_pcm);
    if (_energySamples(pcm) < _minEnrollSpeech) return null;
    final trimmed = _vadTrim(Float32List.fromList(pcm), fromStart: true);
    if (_energySamples(trimmed) < _minEnrollSpeech) return null;
    pcm = trimmed.toList();
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
