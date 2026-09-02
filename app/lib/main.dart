import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import 'actions.dart';
import 'protocol.dart';
import 'settings_page.dart';
import 'voice_engine.dart';
import 'voice_page.dart';
import 'wings_session.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const WingsApp());
}

class WingsApp extends StatelessWidget {
  const WingsApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Wings',
      theme: ThemeData(
        brightness: Brightness.dark,
        colorSchemeSeed: Colors.cyan,
        useMaterial3: true,
      ),
      home: const HomePage(),
    );
  }
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> with SingleTickerProviderStateMixin {
  final _ble = WingsSession.instance.ble;
  final _native = WingsSession.instance.native;
  late final WingsActions _actions;
  late final TabController _tabs;
  String? _lastId;
  List<ScanResult> _results = [];
  bool _scanning = false;
  String _msg = '';
  WingsStatus? _status;
  List<ChannelCal> _cal = List.generate(5, (_) => ChannelCal());
  ChannelPoses _poses = ChannelPoses();
  bool _posesFromBoard = false;
  List<int> _fobMap = [CmdId.toggleWing, CmdId.toggleWrist, CmdId.seq, CmdId.home];
  StreamSubscription<WingsStatus>? _sub;
  StreamSubscription<bool>? _readySub;
  Timer? _listenResume;
  bool _ready = false;
  int _setupCh = 0;
  int _speedPct = 10;
  bool _speedDragging = false;
  final List<int> _chSpeed = List.filled(5, 100);
  bool _chSpeedDragging = false;
  List<int> _lastPeerMac = List.filled(6, 0);

  static const _appVer = kAppVersion;

  @override
  void initState() {
    super.initState();
    _actions = WingsActions(_cmd);
    _tabs = TabController(length: 4, vsync: this);
    _tabs.addListener(() {
      if (!_tabs.indexIsChanging && _tabs.index == 1 && _ready) {
        _syncFromBoard();
      }
    });
    _attachBle();
    _native.getLastId().then((id) {
      if (mounted) setState(() => _lastId = id);
    });
    final v = WingsSession.instance.voice;
    v.onLiveHit = (hit) {
      if (!mounted) return;
      final live = v.store?.live == true &&
          (v.enrolled || v.store?.debugBypass == true);
      _onVoiceHeard(hit, fire: live);
    };
    v.init().then((_) async {
      if (!mounted) return;
      if (v.store?.live == true && v.store?.alwaysListen == true) {
        await Permission.microphone.request();
        await v.startAlways();
      }
    });
  }

  @override
  void dispose() {
    _listenResume?.cancel();
    _sub?.cancel();
    _readySub?.cancel();
    _tabs.dispose();
    // Do not disconnect — FGS + WingsSession own the GATT link.
    super.dispose();
  }

  void _attachBle() {
    _readySub?.cancel();
    _readySub = _ble.readyController.stream.listen((ok) async {
      setState(() {
        _ready = ok;
        if (!ok) {
          _status = null;
          _posesFromBoard = false;
          _msg = _ble.holdingLink ? 'Reconnecting…' : 'Link dropped';
        } else {
          _msg = 'Connected ${_ble.device?.platformName ?? ''}';
        }
      });
      if (ok) {
        await _syncFromBoard();
        if (mounted) setState(() => _status = _ble.lastStatus);
      }
      if (_ble.holdingLink) {
        try {
          await _native.updateFg(ok ? 'Wings connected' : 'Wings reconnecting…');
        } catch (_) {}
      }
    });
    _sub?.cancel();
    _sub = _ble.statusController.stream.listen((s) {
      setState(() {
        _status = s;
        if (!macIsZero(s.peerMac)) _lastPeerMac = List<int>.from(s.peerMac);
        if (!_speedDragging && s.speedPct >= 1 && s.speedPct <= 100) {
          _speedPct = s.speedPct;
        }
        if (!_chSpeedDragging && s.chSpeed.length == 5) {
          for (var i = 0; i < 5; i++) {
            final p = s.chSpeed[i];
            if (p >= 1 && p <= 100) _chSpeed[i] = p;
          }
        }
      });
      unawaited(_syncListenToMotion(s));
    });
    if (_ble.isReady) {
      _ready = true;
      _status = _ble.lastStatus;
      _msg = 'Connected ${_ble.device?.platformName ?? ''}';
      _syncFromBoard();
    }
  }

  /// Pause KWS while path/seq is active. BLE and the earpiece stay connected.
  Future<void> _syncListenToMotion(WingsStatus s) async {
    final v = WingsSession.instance.voice;
    if (!v.isAlways) return;
    final moving = s.pathActive || s.seq;
    if (moving) {
      _listenResume?.cancel();
      _listenResume = null;
      await v.pauseListen();
      try {
        await _native.updateFg('Wings moving…');
      } catch (_) {}
      return;
    }
    if (!v.listenPaused) return;
    if (_listenResume != null) return;
    _listenResume = Timer(const Duration(milliseconds: 400), () async {
      _listenResume = null;
      final now = _ble.lastStatus;
      if (now != null && (now.pathActive || now.seq)) return;
      if (!v.isAlways) return;
      unawaited(_ble.setLinkPriority(high: false));
      await v.resumeListen();
      try {
        await _native.updateFg('Wings listening');
      } catch (_) {}
    });
  }

  /// Board NVS/RAM is source of truth. Status notify never carries taught poses.
  Future<void> _syncFromBoard() async {
    if (!_ble.isReady) return;
    try {
      final r = await _ble.readCal();
      final p = await _ble.readPoses();
      final f = await _ble.readFobMaps();
      if (!mounted) return;
      setState(() {
        if (r.length == 5) _cal = r;
        _fobMap = f;
        if (p != null) {
          _poses = p;
          _posesFromBoard = true;
        } else {
          _posesFromBoard = false;
        }
      });
    } catch (e) {
      if (mounted) {
        setState(() {
          _posesFromBoard = false;
          _msg = 'Board read failed: $e';
        });
      }
    }
  }

  Future<void> _scan() async {
    setState(() {
      _scanning = true;
      _msg = 'Scanning…';
      _results = [];
    });
    try {
      await _ble.requestPermissions();
      final r = await _ble.scan();
      setState(() {
        _results = r;
        _msg = r.isEmpty ? 'No Wings-* found' : 'Found ${r.length}';
      });
    } catch (e) {
      setState(() => _msg = 'Scan error: $e');
    } finally {
      setState(() => _scanning = false);
    }
  }

  Future<void> _connect(BluetoothDevice d) async {
    setState(() => _msg = 'Connecting…');
    try {
      // Start FGS while the Activity is still in front (API 31+).
      try {
        await _native.startFg(
          'Wings connecting…',
          mic: WingsSession.instance.voice.store?.live == true,
        );
      } catch (_) {}
      await _ble.connect(d);
      try {
        await _native.saveLastId(d.remoteId.str);
        await _native.updateFg('Wings connected');
      } catch (_) {}
      await _syncFromBoard();
      setState(() {
        _lastId = d.remoteId.str;
        _ready = _ble.isReady;
        _status = _ble.lastStatus;
        final sp = _ble.lastStatus?.speedPct ?? 10;
        if (sp >= 1 && sp <= 100) _speedPct = sp;
        _msg = 'Connected ${d.platformName}';
      });
    } catch (e) {
      try {
        await _native.stopFg();
      } catch (_) {}
      setState(() => _msg = 'Connect failed: $e');
    }
  }

  Future<void> _disconnect() async {
    try {
      await _native.stopFg();
    } catch (_) {}
    await _ble.disconnect();
    setState(() {
      _ready = false;
      _status = null;
      _posesFromBoard = false;
      _msg = 'Disconnected';
    });
  }

  Future<void> _reconnectLast() async {
    final id = _lastId;
    if (id == null || id.isEmpty) return;
    await _connect(BluetoothDevice.fromId(id));
  }

  Future<void> _cmd(int id, [List<int>? payload]) async {
    try {
      await _ble.sendCmd(id, payload);
    } catch (e) {
      if (!mounted) return;
      setState(() => _msg = 'Command failed: $e');
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Command failed: $e')),
      );
    }
  }

  Future<void> _setSpeed(int pct) async {
    var p = pct;
    if (p < 1) p = 1;
    if (p > 100) p = 100;
    setState(() => _speedPct = p);
    await _ble.setSpeed(p);
  }

  Widget _speedOverride() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Text(
          'Speed $_speedPct%',
          style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 18),
        ),
        const Text(
          'Master scale after BRAKE READY. Per-servo % is on SETUP.',
          style: TextStyle(color: Colors.white70),
        ),
        Slider(
          value: _speedPct.clamp(1, 100).toDouble(),
          min: 1,
          max: 100,
          divisions: 99,
          label: '$_speedPct%',
          onChanged: (v) {
            setState(() {
              _speedDragging = true;
              _speedPct = v.round();
            });
          },
          onChangeEnd: (v) async {
            await _setSpeed(v.round());
            setState(() => _speedDragging = false);
          },
        ),
        Row(
          children: [
            for (final p in [10, 25, 50, 100])
              Expanded(
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 4),
                  child: FilledButton.tonal(
                    style: FilledButton.styleFrom(minimumSize: const Size(0, 48)),
                    onPressed: () => _setSpeed(p),
                    child: Text('$p%'),
                  ),
                ),
              ),
          ],
        ),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    final connected = _ready;
    final proto = _status?.proto ?? 0;
    return Scaffold(
      appBar: AppBar(
        title: const Text('Wings'),
        bottom: connected
            ? TabBar(
                controller: _tabs,
                tabs: const [
                  Tab(text: 'RUN'),
                  Tab(text: 'SETUP'),
                  Tab(text: 'FOB'),
                  Tab(text: 'VOICE'),
                ],
              )
            : null,
        actions: [
          IconButton(
            tooltip: 'Phone settings',
            icon: const Icon(Icons.settings),
            onPressed: () {
              Navigator.of(context).push(
                MaterialPageRoute<void>(builder: (_) => const SettingsPage()),
              );
            },
          ),
          if (connected) ...[
            const Padding(
              padding: EdgeInsets.only(right: 8),
              child: Center(
                child: Text('BLE', style: TextStyle(color: Colors.greenAccent)),
              ),
            ),
            TextButton(
              onPressed: _disconnect,
              child: const Text('DISCONNECT'),
            ),
          ],
        ],
      ),
      body: Padding(
        padding: EdgeInsets.only(bottom: MediaQuery.viewPaddingOf(context).bottom),
        child: connected ? _connectedBody() : _connectBody(proto),
      ),
    );
  }

  Widget _connectBody(int proto) {
    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Card(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text('App $_appVer', style: Theme.of(context).textTheme.titleMedium),
                  Text('Firmware protocol expected: $kProtocolVersion'),
                  if (_status != null) Text('Last seen protocol: $proto'),
                  const SizedBox(height: 8),
                  Text(_msg, style: const TextStyle(color: Colors.white70)),
                ],
              ),
            ),
          ),
          const SizedBox(height: 12),
          FilledButton.icon(
            onPressed: _scanning ? null : _scan,
            icon: _scanning
                ? const SizedBox(
                    width: 18, height: 18,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : const Icon(Icons.bluetooth_searching),
            label: Text(_scanning ? 'Scanning…' : 'Scan for Wings-M-p2 / Wings-S-p2'),
          ),
          if (_lastId != null && _lastId!.isNotEmpty) ...[
            const SizedBox(height: 8),
            OutlinedButton(
              onPressed: _scanning ? null : _reconnectLast,
              child: const Text('Reconnect last wing'),
            ),
          ],
          const SizedBox(height: 8),
          const Text(
            'Connect Master for RUN / voice / fob. Connect either wing for SETUP. '
            'Pairing is ESP-NOW — LINK on the Master RUN tab after both are up.',
            style: TextStyle(color: Colors.white70),
          ),
          const SizedBox(height: 8),
          OutlinedButton(
            onPressed: () {
              Navigator.of(context).push(
                MaterialPageRoute<void>(
                  builder: (_) => Scaffold(
                    appBar: AppBar(title: const Text('Voice')),
                    body: VoicePage(onHeard: _onVoiceHeard),
                  ),
                ),
              );
            },
            child: const Text('Voice enroll / test (no motion)'),
          ),
          const SizedBox(height: 12),
          Expanded(
            child: ListView.builder(
              itemCount: _results.length,
              itemBuilder: (ctx, i) {
                final r = _results[i];
                final name = r.advertisementData.advName.isNotEmpty
                    ? r.advertisementData.advName
                    : r.device.platformName;
                return ListTile(
                  leading: const Icon(Icons.bluetooth),
                  title: Text(name.isEmpty ? '(no name)' : name),
                  subtitle: Text(r.device.remoteId.str),
                  trailing: Text('${r.rssi} dBm'),
                  onTap: () => _connect(r.device),
                );
              },
            ),
          ),
        ],
      ),
    );
  }

  Widget _connectedBody() {
    return TabBarView(
      controller: _tabs,
      children: [
        _runTab(),
        _setupTab(),
        _fobTab(),
        VoicePage(onHeard: _onVoiceHeard),
      ],
    );
  }

  Future<void> _onVoiceHeard(VoiceHit hit, {required bool fire}) async {
    setState(() {
      _msg = hit.keyword.isEmpty
          ? 'Heard nothing  score ${hit.score.toStringAsFixed(2)}'
          : hit.keyword == 'valkyrie'
              ? 'Valkyrie  ${hit.speaker}  ${hit.score.toStringAsFixed(2)}  say open/close/hug/home/stop/flap'
              : 'Heard ${hit.keyword}  ${hit.speaker}  ${hit.score.toStringAsFixed(2)}'
                  '${hit.speakerOk ? "" : "  SPEAKER NO"}';
    });
    if (!fire) return;
    if (!hit.speakerOk || hit.keyword.isEmpty || hit.keyword == 'valkyrie') {
      return;
    }
    if (hit.keyword == 'stop') {
      await _actions.stop();
      return;
    }
    if (_status == null || _status!.isCold) {
      setState(() => _msg = 'COLD — home each servo on SETUP first');
      return;
    }
    switch (hit.keyword) {
      case 'open':
        await _actions.open();
        break;
      case 'close':
        await _actions.close();
        break;
      case 'hug':
        await _actions.hug();
        break;
      case 'home':
        await _actions.home();
        break;
      case 'flap':
        await _actions.flap();
        break;
    }
  }

  bool _runPttDown = false;

  Widget _runPtt() {
    if (WingsSession.instance.voice.isAlways) {
      return Container(
        height: 56,
        width: double.infinity,
        alignment: Alignment.center,
        decoration: BoxDecoration(
          color: Colors.green.shade900,
          borderRadius: BorderRadius.circular(12),
        ),
        child: const Text(
          'ALWAYS LISTEN — say Valkyrie then a command',
          style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold),
        ),
      );
    }
    return Listener(
      onPointerDown: (_) async {
        setState(() => _runPttDown = true);
        await Permission.microphone.request();
        await WingsSession.instance.voice.init();
        await WingsSession.instance.voice.startPtt();
      },
      onPointerUp: (_) async {
        setState(() => _runPttDown = false);
        final hit = await WingsSession.instance.voice.stopPtt();
        if (hit != null) {
          final live = WingsSession.instance.voice.store?.live == true &&
              (WingsSession.instance.voice.enrolled ||
                  WingsSession.instance.voice.store?.debugBypass == true);
          await _onVoiceHeard(hit, fire: live);
        }
      },
      onPointerCancel: (_) async {
        setState(() => _runPttDown = false);
        await WingsSession.instance.voice.stopPtt(commit: false);
      },
      child: Container(
        height: 56,
        width: double.infinity,
        alignment: Alignment.center,
        decoration: BoxDecoration(
          color: _runPttDown ? Colors.red.shade800 : Colors.cyan.shade900,
          borderRadius: BorderRadius.circular(12),
        ),
        child: Text(
          _runPttDown ? 'RELEASE' : 'HOLD TO TALK',
          style: const TextStyle(
            color: Colors.white,
            fontWeight: FontWeight.bold,
          ),
        ),
      ),
    );
  }

  Widget _runTab() {
    final s = _status;
    final bottom = MediaQuery.paddingOf(context).bottom;
    return ListView(
      padding: EdgeInsets.fromLTRB(16, 16, 16, 24 + bottom),
      children: [
        Text(
          'App $_appVer  ·  FW proto ${s?.proto ?? "?"}  ·  '
          '${s?.role == 1 ? "MASTER" : "SLAVE"}  ·  '
          'WH=0  WR=1  EH=2  ER=3  SH=4',
          style: const TextStyle(color: Colors.white70),
        ),
        const SizedBox(height: 8),
        _pairCard(s),
        const SizedBox(height: 8),
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            _armChip(s),
            _chip(WingPose.name(s?.pose ?? 0), Colors.cyan),
            if (s?.pathActive == true) _chip('MOVING', Colors.orange),
            _chip('$_speedPct%', Colors.amber),
          ],
        ),
        const SizedBox(height: 12),
        _speedOverride(),
        const SizedBox(height: 16),
        SizedBox(
          height: 48,
          width: double.infinity,
          child: FilledButton(
            onPressed: () => _actions.armAll(),
            child: const Text('ARM ALL  SH → ER → EH → WR → WH'),
          ),
        ),
        const SizedBox(height: 8),
        Row(
          children: [
            Expanded(
              child: FilledButton.tonal(
                onPressed: () => _cmd(CmdId.toggleWing),
                child: const Text('A OPEN/CLOSE'),
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: FilledButton.tonal(
                onPressed: () => _cmd(CmdId.toggleWrist),
                child: const Text('B HUG'),
              ),
            ),
          ],
        ),
        const SizedBox(height: 8),
        Row(
          children: [
            Expanded(
              child: FilledButton.tonal(
                onPressed: () => _cmd(CmdId.seq),
                child: const Text('C FLAP'),
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: FilledButton(
                style: FilledButton.styleFrom(backgroundColor: Colors.deepOrange),
                onPressed: () => _actions.home(),
                child: const Text('D ARM / SLOW CLOSE'),
              ),
            ),
          ],
        ),
        const SizedBox(height: 8),
        SizedBox(
          height: 48,
          width: double.infinity,
          child: FilledButton(
            style: FilledButton.styleFrom(backgroundColor: Colors.red.shade800),
            onPressed: () => _actions.stop(),
            child: const Text('STOP'),
          ),
        ),
        const SizedBox(height: 8),
        _runPtt(),
        const SizedBox(height: 24),
        const Text('Live µs (commanded / ramped)', style: TextStyle(fontWeight: FontWeight.bold)),
        const SizedBox(height: 8),
        if (s != null)
          ...List.generate(5, (i) {
            return ListTile(
              dense: true,
              title: Text(kServoNames[i]),
              subtitle: Text('tgt ${s.target[i]}  act ${s.actual[i]}'),
              trailing: Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  IconButton(
                    icon: const Icon(Icons.remove),
                    onPressed: () => _ble.jog(i, -50),
                  ),
                  IconButton(
                    icon: const Icon(Icons.add),
                    onPressed: () => _ble.jog(i, 50),
                  ),
                ],
              ),
            );
          }),
      ],
    );
  }

  Widget _setupTab() {
    final c = _cal[_setupCh];
    final tgt = _status?.target[_setupCh];
    final bottom = MediaQuery.paddingOf(context).bottom;
    return ListView(
      padding: EdgeInsets.fromLTRB(16, 16, 16, 24 + bottom),
      children: [
        const Text(
          'ARM this servo to taught CLOSED, then brake (PWM off). '
          'May move quickly if last position was unknown. After all five are '
          'homed (BRAKE READY), use RUN poses.',
          style: TextStyle(color: Colors.white70),
        ),
        const SizedBox(height: 8),
        Wrap(
          spacing: 6,
          children: List.generate(5, (i) {
            return ChoiceChip(
              label: Text(kServoNames[i]),
              selected: _setupCh == i,
              onSelected: (_) => setState(() => _setupCh = i),
            );
          }),
        ),
        const SizedBox(height: 8),
        Text(kServoJobs[_setupCh], style: const TextStyle(color: Colors.cyanAccent)),
        _setupArmRow(),
        const SizedBox(height: 12),
        _speedOverride(),
        const SizedBox(height: 8),
        _chSpeedOverride(),
        const SizedBox(height: 8),
        if (tgt != null)
          Text(
            'Commanded $tgt µs   act ${_status?.actual[_setupCh] ?? "—"}',
            style: const TextStyle(color: Colors.white70),
          ),
        const SizedBox(height: 8),
        Row(
          children: [
            Expanded(
              child: OutlinedButton(
                onPressed: () => _ble.jog(_setupCh, -10),
                child: const Text('− 10'),
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: FilledButton(
                onPressed: () => _ble.jog(_setupCh, 10),
                child: const Text('+ 10'),
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: OutlinedButton(
                onPressed: () => _ble.jog(_setupCh, -50),
                child: const Text('− 50'),
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: FilledButton(
                onPressed: () => _ble.jog(_setupCh, 50),
                child: const Text('+ 50'),
              ),
            ),
          ],
        ),
        SwitchListTile(
          title: const Text('Flip sense (+ wrong way)'),
          subtitle: Text(_setupCh == 0 || _setupCh == 2
              ? '+ should be more hug'
              : '+ should be more open'),
          value: _poses.sense[_setupCh] != 0,
          onChanged: (v) async {
            await _ble.setSense(_setupCh, v);
            await Future<void>.delayed(const Duration(milliseconds: 200));
            await _syncFromBoard();
          },
        ),
        const SizedBox(height: 8),
        const Text('Stamp current commanded µs', style: TextStyle(fontWeight: FontWeight.bold)),
        const SizedBox(height: 8),
        Row(
          children: [
            Expanded(child: OutlinedButton(onPressed: () => _teach(WingPose.closed), child: const Text('SET CLOSED'))),
            const SizedBox(width: 6),
            Expanded(child: OutlinedButton(onPressed: () => _teach(WingPose.open), child: const Text('SET OPEN'))),
            const SizedBox(width: 6),
            Expanded(child: OutlinedButton(onPressed: () => _teach(WingPose.hug), child: const Text('SET HUG'))),
          ],
        ),
        Text(
          _posesFromBoard
              ? 'Taught  C ${_poses.closed[_setupCh]}  O ${_poses.open[_setupCh]}  H ${_poses.hug[_setupCh]}'
              : 'Taught  —  (not loaded from board)',
          style: const TextStyle(color: Colors.white70),
        ),
        const SizedBox(height: 16),
        const Text('Envelope (safety)', style: TextStyle(fontWeight: FontWeight.bold)),
        _calSlider('Soft min', c.softMin, 500, 2500, (v) {
          setState(() => c.softMin = v);
        }),
        _calSlider('Soft max', c.softMax, 500, 2500, (v) {
          setState(() => c.softMax = v);
        }),
        _calSlider('Hard min', c.hardMin, 500, 2500, (v) {
          setState(() => c.hardMin = v);
        }),
        _calSlider('Hard max', c.hardMax, 500, 2500, (v) {
          setState(() => c.hardMax = v);
        }),
        _calSlider('Center', c.center, 500, 2500, (v) {
          setState(() => c.center = v);
        }),
        Row(
          children: [
            Expanded(child: OutlinedButton(onPressed: () => _limitFromCurrent((us) => c.softMin = us), child: const Text('SOFT MIN=NOW'))),
            const SizedBox(width: 6),
            Expanded(child: OutlinedButton(onPressed: () => _limitFromCurrent((us) => c.softMax = us), child: const Text('SOFT MAX=NOW'))),
          ],
        ),
        SwitchListTile(
          title: const Text('Pair invert (left-wing ESP-NOW mirror)'),
          value: c.invert != 0,
          onChanged: (v) => setState(() => c.invert = v ? 1 : 0),
        ),
        const SizedBox(height: 8),
        FilledButton(
          onPressed: () async {
            await _ble.writeCal(_cal);
            await Future<void>.delayed(const Duration(milliseconds: 80));
            await _syncFromBoard();
            if (mounted) {
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(
                  content: Text('Envelope saved. Taught poses left as on the board.'),
                ),
              );
            }
          },
          child: const Text('WRITE CAL'),
        ),
        OutlinedButton(
          onPressed: _syncFromBoard,
          child: const Text('RELOAD FROM BOARD'),
        ),
      ],
    );
  }

  Future<void> _teach(int pose) async {
    await _ble.teachPose(_setupCh, pose);
    // Teach is queued onto the loop task; NVS save follows. Do not stamp from
    // status — that can lag and is what overwrote NVS via WRITE CAL + POSES.
    await Future<void>.delayed(const Duration(milliseconds: 250));
    await _syncFromBoard();
  }

  void _limitFromCurrent(void Function(int us) apply) {
    final tgt = _status?.target[_setupCh];
    if (tgt == null) return;
    setState(() => apply(tgt));
  }

  Widget _fobTab() {
    const labels = ['A', 'B', 'C', 'D'];
    const options = <MapEntry<String, int>>[
      MapEntry('A Open/Close', CmdId.toggleWing),
      MapEntry('B Hug', CmdId.toggleWrist),
      MapEntry('C Flap', CmdId.seq),
      MapEntry('D Arm or slow close', CmdId.home),
      MapEntry('Arm all', CmdId.armAll),
      MapEntry('Disarm all', CmdId.disarm),
    ];
    final bottom = MediaQuery.paddingOf(context).bottom;
    return ListView(
      padding: EdgeInsets.fromLTRB(16, 16, 16, 24 + bottom),
      children: [
        const Text(
          'Fob codes are already learned on the Master. Map should stay A/B/C/D. Do not write this tab unless you mean to change command IDs.',
          style: TextStyle(color: Colors.white70),
        ),
        const SizedBox(height: 12),
        ...List.generate(4, (i) {
          return ListTile(
            title: Text('Button ${labels[i]}'),
            trailing: DropdownButton<int>(
              value: options.any((e) => e.value == _fobMap[i])
                  ? _fobMap[i]
                  : CmdId.home,
              items: options
                  .map((e) => DropdownMenuItem(value: e.value, child: Text(e.key)))
                  .toList(),
              onChanged: (v) {
                if (v == null) return;
                setState(() => _fobMap[i] = v);
              },
            ),
          );
        }),
        FilledButton(
          onPressed: () async {
            await _ble.writeFobMap(_fobMap);
            if (mounted) {
              ScaffoldMessenger.of(context).showSnackBar(
                const SnackBar(content: Text('Fob map written')),
              );
            }
          },
          child: const Text('WRITE FOB MAP'),
        ),
      ],
    );
  }

  Widget _calSlider(String label, int value, int min, int max, ValueChanged<int> onChanged) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text('$label: $value µs'),
        Slider(
          value: value.clamp(min, max).toDouble(),
          min: min.toDouble(),
          max: max.toDouble(),
          divisions: (max - min) ~/ 10,
          onChanged: (v) => onChanged(v.round()),
        ),
      ],
    );
  }

  List<int> _macToLink() {
    if (_status != null && !macIsZero(_status!.peerMac)) {
      return _status!.peerMac;
    }
    if (!macIsZero(_lastPeerMac)) return _lastPeerMac;
    return List<int>.from(kDefaultSlaveMac);
  }

  Future<void> _linkSlave() async {
    final mac = _macToLink();
    if (macIsZero(mac)) return;
    try {
      await _ble.writePeerMac(mac);
      if (!mounted) return;
      setState(() => _lastPeerMac = List<int>.from(mac));
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Slave linked ${formatMac(mac)}')),
      );
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Link failed: $e')),
      );
    }
  }

  Future<void> _unlinkSlave() async {
    try {
      await _ble.clearPeerMac();
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Slave unlinked')),
      );
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('Unlink failed: $e')),
      );
    }
  }

  Widget _pairCard(WingsStatus? s) {
    if (s == null) return const SizedBox.shrink();
    if (!s.isMaster) {
      final paused = s.slaveLink == SlaveLink.paused;
      return Card(
        color: paused ? Colors.orange.shade900 : Colors.blueGrey.shade900,
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Text(
            paused
                ? 'SLAVE · ESP-NOW paused while this phone is connected. '
                    'Disconnect when SETUP is done so Master can see this wing.'
                : 'SLAVE · SETUP / manual only. RUN, voice, and fob go through Master.',
            style: const TextStyle(fontWeight: FontWeight.bold),
          ),
        ),
      );
    }

    Color bg;
    String title;
    String body;
    switch (s.slaveLink) {
      case SlaveLink.live:
        bg = Colors.green.shade900;
        title = 'SLAVE LIVE';
        body = 'Answering the same RUN / voice / fob commands.  ${formatMac(s.peerMac)}';
        break;
      case SlaveLink.heard:
        bg = Colors.amber.shade900;
        title = 'SLAVE HEARD';
        body = 'Responding over ESP-NOW but not saved. LINK to keep this pair.  ${formatMac(s.peerMac)}';
        break;
      case SlaveLink.stale:
        bg = Colors.orange.shade900;
        title = 'SLAVE PAIRED';
        body = 'Saved ${formatMac(s.peerMac)}. Card stays quiet while this phone is on Master — RUN still broadcasts. Unlink to forget.';
        break;
      default:
        bg = Colors.orange.shade900;
        title = 'SLAVE NONE';
        body = 'Not paired. LINK SLAVE saves ${formatMac(_macToLink())} and broadcasts RUN to that wing. Phone on Master will not show LIVE.';
    }

    return Card(
      color: bg,
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(title, style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 18)),
            const SizedBox(height: 4),
            Text(body),
            if (s.canLinkSlave || s.canUnlinkSlave) ...[
              const SizedBox(height: 10),
              SizedBox(
                height: 48,
                child: s.canUnlinkSlave
                    ? OutlinedButton(
                        onPressed: _unlinkSlave,
                        child: const Text('UNLINK'),
                      )
                    : FilledButton(
                        onPressed: _linkSlave,
                        child: const Text('LINK SLAVE'),
                      ),
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _armChip(WingsStatus? s) {
    if (s == null || s.isCold) return _chip('COLD', Colors.red);
    if (s.isLive) return _chip('LIVE', Colors.orange);
    return _chip('BRAKE READY', Colors.green);
  }

  Widget _setupArmRow() {
    final live = _status?.chArmed(_setupCh) == true;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        const SizedBox(height: 8),
        _chip(
          live ? '${kServoNames[_setupCh]} LIVE' : '${kServoNames[_setupCh]} idle',
          live ? Colors.orange : Colors.grey,
        ),
        const SizedBox(height: 8),
        Row(
          children: [
            Expanded(
              child: FilledButton(
                onPressed: () => _actions.arm(_setupCh),
                child: Text('ARM ${kServoNames[_setupCh]}'),
              ),
            ),
            const SizedBox(width: 8),
            Expanded(
              child: OutlinedButton(
                onPressed: () => _actions.disarm(_setupCh),
                child: Text('DISARM ${kServoNames[_setupCh]}'),
              ),
            ),
          ],
        ),
      ],
    );
  }

  Future<void> _setChSpeed(int pct) async {
    var p = pct;
    if (p < 1) p = 1;
    if (p > 100) p = 100;
    setState(() => _chSpeed[_setupCh] = p);
    await _ble.setChSpeed(_setupCh, p);
  }

  Widget _chSpeedOverride() {
    final p = _chSpeed[_setupCh];
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Text(
          '${kServoNames[_setupCh]} $p%',
          style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 18),
        ),
        const Text(
          'This servo only. Multiplies the RUN master scale. Saved across power loss.',
          style: TextStyle(color: Colors.white70),
        ),
        Slider(
          value: p.clamp(1, 100).toDouble(),
          min: 1,
          max: 100,
          divisions: 99,
          label: '$p%',
          onChanged: (v) {
            setState(() {
              _chSpeedDragging = true;
              _chSpeed[_setupCh] = v.round();
            });
          },
          onChangeEnd: (v) async {
            await _setChSpeed(v.round());
            setState(() => _chSpeedDragging = false);
          },
        ),
      ],
    );
  }

  Widget _chip(String label, Color color) {
    return Chip(
      label: Text(label),
      backgroundColor: color.withValues(alpha: 0.25),
      side: BorderSide(color: color),
    );
  }
}
