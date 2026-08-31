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
  so.OnlineStream? _kwsStream;
  final _pcm = <double>[];
  String _heard = '';
  bool _wake = false;
  bool _always = false;
  bool _ptt = false;
  DateTime? _cmdUntil;
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
        paths[e.value] = await _copyAsset(e.key, '$modelDir/${e.value}');
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
          keywordsThreshold: 0.20,
        ),
      );
      _ex = so.SpeakerEmbeddingExtractor(
        config: so.SpeakerEmbeddingExtractorConfig(
          model: paths['campplus.onnx']!,
          numThreads: 2,
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

  Future<String> _copyAsset(String asset, String dest) async {
    final f = File(dest);
    if (await f.exists() && await f.length() > 1000) return dest;
    final data = await rootBundle.load(asset);
    await f.writeAsBytes(data.buffer.asUint8List(), flush: true);
    return dest;
  }

  bool get isAlways => _always;

  Future<void> startAlways() async {
    if (!ready || _spotter == null || _always) return;
    _always = true;
    _heard = '';
    _wake = false;
    _cmdUntil = null;
    await _ensureMic();
  }

  Future<void> stopAlways() async {
    _always = false;
    _cmdUntil = null;
    if (!_ptt) await _stopMic(commit: false);
  }

  Future<void> startPtt() async {
    if (!ready || _spotter == null) return;
    _ptt = true;
    _pcm.clear();
    _heard = '';
    _wake = false;
    _cmdUntil = null;
    await _ensureMic();
  }

  Future<void> _ensureMic() async {
    if (_micSub != null) return;
    _kwsStream?.free();
    _kwsStream = _spotter!.createStream();
    _ringI = 0;
    _ringLen = 0;
    _micSub = WingsNative.mic.receiveBroadcastStream().listen(_onPcm);
  }

  Future<void> _stopMic({required bool commit}) async {
    await _micSub?.cancel();
    _micSub = null;
    final stream = _kwsStream;
    final spot = _spotter;
    if (stream != null && spot != null) {
      stream.acceptWaveform(samples: Float32List(8000), sampleRate: 16000);
      while (spot.isReady(stream)) {
        spot.decode(stream);
        final kw = spot.getResult(stream).keyword;
        if (kw.isNotEmpty) _acceptKw(kw);
      }
      stream.free();
    }
    _kwsStream = null;
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
    final bytes = raw is Uint8List ? raw : Uint8List.fromList(List<int>.from(raw as List));
    final n = bytes.length ~/ 2;
    final f = Float32List(n);
    final bd = ByteData.sublistView(bytes);
    for (var i = 0; i < n; i++) {
      f[i] = bd.getInt16(i * 2, Endian.little) / 32768.0;
    }
    _pushRing(f);
    if (_ptt) _pcm.addAll(f);
    final stream = _kwsStream;
    final spot = _spotter;
    if (stream == null || spot == null) return;
    stream.acceptWaveform(samples: f, sampleRate: 16000);
    while (spot.isReady(stream)) {
      spot.decode(stream);
      final kw = spot.getResult(stream).keyword;
      if (kw.isNotEmpty) {
        _acceptKw(kw);
        spot.reset(stream);
      }
    }
    if (_always && _heard.isNotEmpty) _emitAlways();
  }

  static const _cmds = ['open', 'close', 'hug', 'home', 'stop', 'flap'];

  bool get _cmdOpen {
    final u = _cmdUntil;
    return u != null && DateTime.now().isBefore(u);
  }

  VoiceHit _scoreRing({String keyword = ''}) {
    final emb = _embed(_ringSnap().toList());
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

  void _acceptKw(String raw) {
    var s = raw.trim().toLowerCase();
    if (s == 'wake' || s == 'valkyrie') {
      if (_always) {
        final gate = _scoreRing(keyword: '');
        lastHit.value = gate;
        if (!gate.speakerOk) {
          _wake = false;
          _cmdUntil = null;
          return;
        }
      }
      _wake = true;
      _cmdUntil = DateTime.now().add(const Duration(milliseconds: 2500));
      return;
    }
    if (s.startsWith('c_')) {
      s = s.substring(2);
      if (_cmds.contains(s) && (_wake || _cmdOpen)) _heard = s;
      return;
    }
    if (_cmds.contains(s)) _heard = s;
  }

  void _emitAlways() {
    final cmd = _heard;
    _heard = '';
    _wake = false;
    _cmdUntil = null;
    if (cmd.isEmpty) return;
    final hit = _scoreRing(keyword: cmd);
    lastHit.value = hit;
    onLiveHit?.call(hit);
  }

  Future<VoiceHit?> stopPtt({bool commit = true}) async {
    _ptt = false;
    if (_always) {
      if (!commit) return null;
    } else {
      await _stopMic(commit: commit);
      if (!commit) return null;
    }

    final emb = _embed(_pcm);
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
      keyword: _heard,
      speaker: speaker,
      score: score,
      speakerOk: ok,
    );
    lastHit.value = hit;
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

  Future<Float32List?> enrollClip() async {
    final emb = _embed(_pcm);
    return emb;
  }

  Future<void> addProfile(String name, List<Float32List> embeds) async {
    if (store == null) return;
    if (store!.profiles.length >= 2) return;
    store!.profiles.add(VoiceProfile(name: name, embeddings: embeds));
    await store!.save();
    _rebuildManager();
  }

  Future<void> deleteProfile(String name) async {
    store?.profiles.removeWhere((p) => p.name == name);
    await store?.save();
    _rebuildManager();
  }

  bool get enrolled => (store?.profiles.isNotEmpty ?? false);
}
