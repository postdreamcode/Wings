import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';

import 'voice_engine.dart';
import 'wings_session.dart';

const _prompts = [
  'Valkyrie',
  'Valkyrie',
  'Valkyrie',
  'Valkyrie',
  'Valkyrie',
  'Valkyrie',
  'Valkyrie',
  'Valkyrie',
  'open',
  'close',
  'hug',
  'home',
  'stop',
];

class VoicePage extends StatefulWidget {
  const VoicePage({super.key, required this.onHeard});

  final Future<void> Function(VoiceHit hit, {required bool fire}) onHeard;

  @override
  State<VoicePage> createState() => _VoicePageState();
}

class _VoicePageState extends State<VoicePage> {
  final _name = TextEditingController();
  final _voice = WingsSession.instance.voice;
  bool _holding = false;
  bool _enrolling = false;
  int _enrollI = 0;
  final _clips = <Float32List>[];
  String _msg = '';

  @override
  void initState() {
    super.initState();
    _boot();
  }

  @override
  void dispose() {
    _name.dispose();
    super.dispose();
  }

  Future<void> _boot() async {
    await Permission.microphone.request();
    await _voice.init();
    if (mounted) setState(() {});
  }

  Future<void> _down() async {
    await Permission.microphone.request();
    setState(() {
      _holding = true;
      _msg = _enrolling ? 'Recording ${_prompts[_enrollI]}…' : 'Listening…';
    });
    await _voice.startPtt();
  }

  Future<void> _up() async {
    setState(() => _holding = false);
    if (_enrolling) {
      await _voice.stopPtt(commit: false);
      final emb = await _voice.enrollClip();
      if (emb != null) _clips.add(emb);
      _enrollI++;
      if (_enrollI >= _prompts.length) {
        final n = _name.text.trim();
        if (n.isNotEmpty && _clips.length >= 10) {
          await _voice.addProfile(n, List<Float32List>.from(_clips));
          _msg = 'Saved profile $n';
        } else {
          _msg = 'Need a name and 10+ clips. Got ${_clips.length}.';
        }
        _enrolling = false;
        _enrollI = 0;
        _clips.clear();
      } else {
        _msg = 'Got clip ${_clips.length}. Next: ${_prompts[_enrollI]}';
      }
      if (mounted) setState(() {});
      return;
    }
    final hit = await _voice.stopPtt();
    if (hit == null) return;
    final live = _voice.store?.live == true &&
        (_voice.enrolled || _voice.store?.debugBypass == true);
    await widget.onHeard(hit, fire: live);
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    final st = _voice.store;
    final hit = _voice.lastHit.value;
    final bottom = MediaQuery.paddingOf(context).bottom;
    return ListView(
      padding: EdgeInsets.fromLTRB(16, 16, 16, 24 + bottom),
      children: [
        Text(
          _voice.ready
              ? (_voice.isAlways
                  ? 'Always listen  ·  Valkyrie (your voice) then command'
                  : 'PTT test  ·  Valkyrie then open/close/hug/home/stop/flap')
              : (_voice.error.isEmpty ? 'Loading models…' : _voice.error),
          style: const TextStyle(color: Colors.white70),
        ),
        const SizedBox(height: 8),
        if (hit != null)
          Card(
            color: hit.speakerOk && hit.keyword.isNotEmpty
                ? Colors.green.shade900
                : Colors.grey.shade900,
            child: ListTile(
              title: Text(
                hit.keyword.isEmpty ? 'No command' : hit.keyword.toUpperCase(),
                style: const TextStyle(fontSize: 22, fontWeight: FontWeight.bold),
              ),
              subtitle: Text(
                'speaker ${hit.speaker.isEmpty ? "-" : hit.speaker}  '
                'score ${hit.score.toStringAsFixed(2)}  '
                '${hit.speakerOk ? "MATCH" : "NO MATCH"}',
              ),
            ),
          ),
        const SizedBox(height: 8),
        Text(_msg, style: const TextStyle(color: Colors.white70)),
        const SizedBox(height: 12),
        Listener(
          onPointerDown: (_) => _down(),
          onPointerUp: (_) => _up(),
          onPointerCancel: (_) => _up(),
          child: Container(
            height: 72,
            alignment: Alignment.center,
            decoration: BoxDecoration(
              color: _holding ? Colors.red : Colors.cyan.shade800,
              borderRadius: BorderRadius.circular(12),
            ),
            child: Text(
              _holding ? 'RELEASE' : 'HOLD TO TALK',
              style: const TextStyle(
                fontSize: 20,
                fontWeight: FontWeight.bold,
                color: Colors.white,
              ),
            ),
          ),
        ),
        const SizedBox(height: 16),
        SwitchListTile(
          title: const Text('Enable live voice'),
          subtitle: const Text(
            'Servos stay still until this is on AND a profile matches. '
            'Poses need BRAKE READY (home each servo on SETUP first). STOP always works.',
          ),
          value: st?.live ?? false,
          onChanged: (st == null || (!_voice.enrolled && !(st.debugBypass)))
              ? null
              : (v) async {
                  st.live = v;
                  await st.save();
                  if (v) {
                    await WingsSession.instance.native.startFg(
                      st.alwaysListen
                          ? 'Wings listening'
                          : 'Wings listening (PTT)',
                      mic: true,
                    );
                    if (st.alwaysListen) await _voice.startAlways();
                  } else {
                    await _voice.stopAlways();
                    await WingsSession.instance.native
                        .updateFg('Wings connected');
                  }
                  setState(() {});
                },
        ),
        SwitchListTile(
          title: const Text('Always listen (earpiece)'),
          subtitle: const Text(
            'Background listen for Valkyrie from an enrolled voice, '
            'then a 2.5s window for open/close/hug/home/stop/flap. '
            'PTT stays for enroll and test.',
          ),
          value: st?.alwaysListen ?? false,
          onChanged: (st == null || !st.live) ? null : (v) async {
            st.alwaysListen = v;
            await st.save();
            if (v) {
              await _voice.startAlways();
              await WingsSession.instance.native
                  .startFg('Wings listening', mic: true);
            } else {
              await _voice.stopAlways();
              await WingsSession.instance.native
                  .startFg('Wings listening (PTT)', mic: true);
            }
            setState(() {});
          },
        ),
        SwitchListTile(
          title: const Text('Debug: skip speaker lock'),
          subtitle: const Text('Lets you enable live without a profile.'),
          value: st?.debugBypass ?? false,
          onChanged: st == null
              ? null
              : (v) async {
                  st.debugBypass = v;
                  await st.save();
                  setState(() {});
                },
        ),
        const Divider(),
        const Text('Profiles (1–2)', style: TextStyle(fontWeight: FontWeight.bold)),
        for (final p in st?.profiles ?? [])
          ListTile(
            title: Text(p.name),
            subtitle: Text('${p.embeddings.length} clips'),
            trailing: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                TextButton(
                  onPressed: () async {
                    _name.text = p.name;
                    await _voice.deleteProfile(p.name);
                    setState(() {
                      _enrolling = true;
                      _enrollI = 0;
                      _clips.clear();
                      _msg = 'Retrain ${p.name}. Hold PTT: ${_prompts[0]}';
                    });
                  },
                  child: const Text('Retrain'),
                ),
                IconButton(
                  icon: const Icon(Icons.delete),
                  onPressed: () async {
                    await _voice.deleteProfile(p.name);
                    setState(() {});
                  },
                ),
              ],
            ),
          ),
        if ((st?.profiles.length ?? 0) < 2) ...[
          TextField(
            controller: _name,
            decoration: const InputDecoration(labelText: 'Profile name'),
          ),
          const SizedBox(height: 8),
          if (_enrolling)
            Text('Say: ${_prompts[_enrollI]}  (${_enrollI + 1}/${_prompts.length})')
          else
            FilledButton.tonal(
              onPressed: () {
                if (_name.text.trim().isEmpty) {
                  setState(() => _msg = 'Name the profile first.');
                  return;
                }
                setState(() {
                  _enrolling = true;
                  _enrollI = 0;
                  _clips.clear();
                  _msg = 'Hold PTT and say: ${_prompts[0]}';
                });
              },
              child: const Text('Enroll 13 clips (8× Valkyrie)'),
            ),
        ],
      ],
    );
  }
}
