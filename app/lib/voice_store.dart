import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

class VoiceProfile {
  VoiceProfile({required this.name, required this.embeddings});

  final String name;
  final List<Float32List> embeddings;

  Map<String, dynamic> toJson() => {
        'name': name,
        'embeddings': embeddings.map((e) => e.toList()).toList(),
      };

  static VoiceProfile fromJson(Map<String, dynamic> j) {
    final raw = (j['embeddings'] as List?) ?? [];
    return VoiceProfile(
      name: (j['name'] as String?) ?? 'profile',
      embeddings: raw
          .map((e) => Float32List.fromList(
                (e as List).map((x) => (x as num).toDouble()).toList(),
              ))
          .toList(),
    );
  }
}

class VoiceStore {
  VoiceStore(this._path);

  final String _path;
  final List<VoiceProfile> profiles = [];
  bool live = false;
  bool alwaysListen = false;
  bool debugBypass = false;
  double threshold = 0.50;

  Future<void> load() async {
    final f = File(_path);
    if (!await f.exists()) return;
    final j = jsonDecode(await f.readAsString()) as Map<String, dynamic>;
    live = j['live'] == true;
    alwaysListen = j['alwaysListen'] == true;
    debugBypass = j['debugBypass'] == true;
    threshold = (j['threshold'] as num?)?.toDouble() ?? 0.50;
    profiles
      ..clear()
      ..addAll(
        ((j['profiles'] as List?) ?? [])
            .map((e) => VoiceProfile.fromJson(e as Map<String, dynamic>)),
      );
  }

  Future<void> save() async {
    final f = File(_path);
    await f.parent.create(recursive: true);
    await f.writeAsString(
      jsonEncode({
        'live': live,
        'alwaysListen': alwaysListen,
        'debugBypass': debugBypass,
        'threshold': threshold,
        'profiles': profiles.map((p) => p.toJson()).toList(),
      }),
    );
  }
}
