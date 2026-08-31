import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';

import 'wings_session.dart';

/// Phone taps Ken has to do once (Samsung A16 / S23 FE, Android 16).
class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  final _native = WingsSession.instance.native;
  bool? _batteryOk;
  String _mic = 'not asked';

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  Future<void> _refresh() async {
    final bat = await _native.ignoringBattery();
    final mic = await Permission.microphone.status;
    if (!mounted) return;
    setState(() {
      _batteryOk = bat;
      _mic = mic.isGranted ? 'allowed' : 'denied / not yet';
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Phone settings (once)')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Text(
            'Do these on the Galaxy A16 and the S23 FE. Without them, '
            'Samsung will kill BLE when the screen is off.',
            style: Theme.of(context).textTheme.bodyMedium,
          ),
          const SizedBox(height: 16),
          _step(
            '1',
            'Notifications — allow',
            'Needed for the persistent “Wings connected” notice.',
            'Open notification settings',
            () async {
              await Permission.notification.request();
              await _native.openNotificationSettings();
              await _refresh();
            },
          ),
          _step(
            '2',
            'Nearby devices + Microphone — allow',
            'Nearby devices is BLE. Mic is for later voice; grant it now so the '
            'foreground service can add the microphone type from this screen.',
            'Ask BLE + mic',
            () async {
              await [
                Permission.bluetoothScan,
                Permission.bluetoothConnect,
                Permission.microphone,
              ].request();
              await _refresh();
            },
          ),
          _step(
            '3',
            'Battery — Unrestricted',
            _batteryOk == true
                ? 'This phone says optimizations are already ignored.'
                : 'Tap Allow / Unrestricted. If the dialog never appears, use app details.',
            'Ignore battery optimization',
            () async {
              await Permission.ignoreBatteryOptimizations.request();
              await _native.openBatteryExemption();
              await _refresh();
            },
          ),
          _step(
            '4',
            'Never sleeping apps (Samsung)',
            'Device care → Battery → Background usage limits → Never sleeping apps → Wings. '
            'The A16 is aggressive about this.',
            'Open Samsung battery',
            () => _native.openSamsungNeverSleep(),
          ),
          _step(
            '5',
            'Pair the earpiece (HFP / headset)',
            'Use Android Bluetooth settings. Needs the headset profile, not media-only. '
            'Voice slice will use this mic.',
            'Open Bluetooth settings',
            () => _native.openBluetoothSettings(),
          ),
          const ListTile(
            leading: CircleAvatar(child: Text('6')),
            title: Text('Lock Wings in Recents'),
            subtitle: Text(
              'Open Recents, tap the Wings icon, turn on Lock this app '
              'so a swipe does not kill the process.',
            ),
          ),
          const SizedBox(height: 16),
          OutlinedButton(
            onPressed: () => _native.openAppDetails(),
            child: const Text('Open Wings app details'),
          ),
          const SizedBox(height: 8),
          Text('Mic permission: $_mic', style: const TextStyle(color: Colors.white70)),
        ],
      ),
    );
  }

  Widget _step(
    String n,
    String title,
    String sub,
    String action,
    Future<void> Function() onTap,
  ) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            ListTile(
              leading: CircleAvatar(child: Text(n)),
              title: Text(title),
              subtitle: Text(sub),
            ),
            Align(
              alignment: Alignment.centerRight,
              child: FilledButton.tonal(
                onPressed: onTap,
                child: Text(action),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
