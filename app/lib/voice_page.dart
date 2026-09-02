import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';

import 'voice_engine.dart';
import 'wings_session.dart';

const _prompts = [
  'Hey Valkyrie',
  'Hey Valkyrie',
  'Valkyrie',
  'Valkyrie',
  'Valkyrie',
  'Valkyrie',
  'Valkyrie open',
  'Valkyrie open',
  'Valkyrie close',
  'Valkyrie close',
  'open',
  'open',
  'close',
  'close',
  'hug',
  'home',
  'stop',
  'flap',
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
  bool _busy = false;
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

  Future<bool> _headsetOrRefuse() async {
    final r = await _voice.waitMicRoute();
    final phone = r.trim().toLowerCase() == 'phone';
    await _voice.dropMicIfIdle();
    if (phone) {
      if (mounted) {
        setState(() => _msg =
            'MIC: PHONE (earpiece not captured — reconnect the bud, then Retrain)');
      }
      return false;
    }
    return true;
  }

  Future<void> _down() async {
    if (_holding || _busy) return;
    if (_enrolling && _voice.isPhoneMic) {
      setState(() => _msg =
          'MIC: PHONE (earpiece not captured — reconnect the bud, then Retrain)');
      return;
    }
    await Permission.microphone.request();
    setState(() {
      _holding = true;
      _msg = _enrolling ? 'Recording ${_prompts[_enrollI]}…' : 'Listening…';
    });
    await _voice.startPtt(cue: !_enrolling);
  }

  Future<void> _up() async {
    if (!_holding || _busy) return;
    _holding = false;
    _busy = true;
    setState(() {});
    try {
      if (_enrolling) {
        await _voice.stopPtt(commit: false);
        await _voice.peekMicRoute();
        if (_voice.isPhoneMic) {
          _msg =
              'MIC: PHONE (earpiece not captured — reconnect the bud, then Retrain)';
          if (mounted) setState(() {});
          return;
        }
        final emb = await _voice.enrollClip();
        if (emb == null) {
          _msg =
              'Too short — keep holding about a second. Still: ${_prompts[_enrollI]} '
              '(${_enrollI + 1}/${_prompts.length}, ${_clips.length} saved)';
          if (mounted) setState(() {});
          return;
        }
        _clips.add(emb);
        _enrollI++;
        if (_enrollI >= _prompts.length) {
          final n = _name.text.trim();
          if (n.isEmpty) {
            _msg = 'Name the profile first, then enroll again.';
          } else {
            final ok =
                await _voice.addProfile(n, List<Float32List>.from(_clips));
            _msg = ok
                ? 'Saved profile $n (${_clips.length} clips)'
                : 'Save failed (already 2 profiles, or storage error).';
          }
          _enrolling = false;
          _enrollI = 0;
          _clips.clear();
          _voice.setCue(true);
        } else {
          _msg =
              'Saved ${_clips.length}. Next: ${_prompts[_enrollI]} '
              '(${_enrollI + 1}/${_prompts.length})';
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
    } finally {
      _busy = false;
    }
  }

  @override
  Widget build(BuildContext context) {
    final st = _voice.store;
    final bottom = MediaQuery.paddingOf(context).bottom;
    return ListView(
      padding: EdgeInsets.fromLTRB(16, 16, 16, 24 + bottom),
      children: [
        Text(
          _voice.ready
              ? (_voice.isAlways
                  ? 'Always listen  ·  Valkyrie (your voice) then command'
                  : 'PTT test  ·  Valkyrie open, or Valkyrie then open/close')
              : (_voice.error.isEmpty ? 'Loading models…' : _voice.error),
          style: const TextStyle(color: Colors.white70),
        ),
        const SizedBox(height: 8),
        ValueListenableBuilder<String>(
          valueListenable: _voice.micRoute,
          builder: (context, route, _) {
            if (route.isEmpty) return const SizedBox.shrink();
            final phone = route.toLowerCase() == 'phone';
            return Text(
              phone
                  ? 'MIC: PHONE (earpiece not captured — reconnect the bud, then Retrain)'
                  : 'MIC: $route',
              style: TextStyle(
                color: phone ? Colors.orangeAccent : Colors.lightGreenAccent,
                fontWeight: FontWeight.bold,
              ),
            );
          },
        ),
        const SizedBox(height: 8),
        ValueListenableBuilder<VoiceHit?>(
            valueListenable: _voice.lastHit,
            builder: (context, hit, _) {
              if (hit == null) return const SizedBox.shrink();
              final isWake = hit.keyword == 'valkyrie';
              final hasCmd = hit.keyword.isNotEmpty && !isWake;
              final title = hit.keyword.isEmpty
                  ? 'No command'
                  : isWake
                      ? 'VALKYRIE — say a command'
                      : hit.keyword.toUpperCase();
              return Card(
                color: (hit.speakerOk && hasCmd)
                    ? Colors.green.shade900
                    : (isWake && hit.speakerOk)
                        ? Colors.cyan.shade900
                        : Colors.grey.shade900,
                child: ListTile(
                  title: Text(
                    title,
                    style: const TextStyle(
                      fontSize: 22,
                      fontWeight: FontWeight.bold,
                    ),
                  ),
                  subtitle: ValueListenableBuilder<String>(
                    valueListenable: _voice.lastKw,
                    builder: (context, kw, _) {
                      return Text(
                        'speaker ${hit.speaker.isEmpty ? "-" : hit.speaker}  '
                        'score ${hit.score.toStringAsFixed(2)} / ${(st?.threshold ?? 0.35).toStringAsFixed(2)}  '
                        '${hit.score < 0 ? "NO PROFILE" : hit.speakerOk ? "SPEAKER OK" : "SPEAKER NO"}'
                        '${kw.isEmpty ? "" : "  kws $kw"}',
                      );
                    },
                  ),
                ),
              );
            },
          ),
        const SizedBox(height: 8),
        Text(_msg, style: const TextStyle(color: Colors.white70)),
        const SizedBox(height: 12),
        if (_voice.isAlways && !_enrolling)
          Container(
            height: 72,
            alignment: Alignment.center,
            decoration: BoxDecoration(
              color: Colors.green.shade900,
              borderRadius: BorderRadius.circular(12),
            ),
            child: const Text(
              'HANDS-FREE — Valkyrie open, or Valkyrie then open',
              textAlign: TextAlign.center,
              style: TextStyle(
                fontSize: 18,
                fontWeight: FontWeight.bold,
                color: Colors.white,
              ),
            ),
          )
        else
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
                _holding
                    ? 'RELEASE'
                    : (_enrolling ? 'HOLD TO RECORD' : 'HOLD TO TALK'),
                style: const TextStyle(
                  fontSize: 20,
                  fontWeight: FontWeight.bold,
                  color: Colors.white,
                ),
              ),
            ),
          ),
        const SizedBox(height: 8),
        TextButton(
          onPressed: () async {
            await _voice.testChimes();
            if (mounted) {
              setState(() => _msg = 'Played wake / ok / no. Hear them in the earpiece?');
            }
          },
          child: const Text('Test earpiece chimes'),
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
                  if (v && !await _headsetOrRefuse()) return;
                  st.live = v;
                  await st.save();
                  if (v) {
                    await WingsSession.instance.native.startFg(
                      st.alwaysListen
                          ? 'Wings listening'
                          : 'Wings listening (PTT)',
                      mic: true,
                    );
                    if (st.alwaysListen) {
                      await _voice.startAlways();
                    }
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
            'Say Valkyrie open in one breath, or Valkyrie then open. '
            'Short open/close only after the wake chime. '
            'Mic mutes while the wings move (fob / e-stop still abort). '
            'Earpiece chimes wake / ok / no. Hold-to-talk is only for enroll.',
          ),
          value: st?.alwaysListen ?? false,
          onChanged: (st == null || !st.live)
              ? null
              : (v) async {
                  if (v && !await _headsetOrRefuse()) return;
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
        const SizedBox(height: 4),
        const Text(
          'Retrain with the earpiece in and the phone away from your mouth. '
          'The MIC line above must show the headset, not PHONE.',
          style: TextStyle(color: Colors.white70, fontSize: 13),
        ),
        for (final p in st?.profiles ?? [])
          ListTile(
            title: Text(p.name),
            subtitle: Text('${p.embeddings.length} clips'),
            trailing: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                TextButton(
                  onPressed: () async {
                    if (!await _headsetOrRefuse()) return;
                    _name.text = p.name;
                    setState(() {
                      _enrolling = true;
                      _enrollI = 0;
                      _clips.clear();
                      _msg =
                          'Retrain ${p.name} (old clips stay until save). Hold PTT: ${_prompts[0]}';
                    });
                    _voice.setCue(false);
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
              onPressed: () async {
                if (_name.text.trim().isEmpty) {
                  setState(() => _msg = 'Name the profile first.');
                  return;
                }
                if (!await _headsetOrRefuse()) return;
                setState(() {
                  _enrolling = true;
                  _enrollI = 0;
                  _clips.clear();
                  _msg = 'Hold PTT and say: ${_prompts[0]}';
                });
                _voice.setCue(false);
              },
              child: const Text('Enroll 18 clips (Hey Valkyrie + Valkyrie open)'),
            ),
        ],
      ],
    );
  }
}
