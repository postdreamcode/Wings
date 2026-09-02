package com.wings.wings_app

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.PowerManager
import android.provider.Settings
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private val channelName = "wings/native"
    private val prefsName = "wings"

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        EventChannel(flutterEngine.dartExecutor.binaryMessenger, "wings/mic")
            .setStreamHandler(WingsMic(this))
        val ch = MethodChannel(flutterEngine.dartExecutor.binaryMessenger, channelName)
        WingsPark.dartChannel = ch
        ch.setMethodCallHandler { call, result ->
                when (call.method) {
                    "dataDir" -> {
                        result.success(filesDir.absolutePath)
                    }
                    "startFg" -> {
                        val text = call.argument<String>("text") ?: "Wings connected"
                        val mic = call.argument<Boolean>("mic") ?: false
                        WingsFgService.start(this, text, mic)
                        result.success(null)
                    }
                    "stopFg" -> {
                        WingsFgService.stop(this)
                        result.success(null)
                    }
                    "updateFg" -> {
                        val text = call.argument<String>("text") ?: "Wings connected"
                        WingsFgService.updateNotification(this, text)
                        result.success(null)
                    }
                    "saveLastId" -> {
                        val id = call.argument<String>("id") ?: ""
                        getSharedPreferences(prefsName, Context.MODE_PRIVATE)
                            .edit().putString("lastId", id).apply()
                        result.success(null)
                    }
                    "getLastId" -> {
                        val id = getSharedPreferences(prefsName, Context.MODE_PRIVATE)
                            .getString("lastId", null)
                        result.success(id)
                    }
                    "ignoringBattery" -> {
                        result.success(isIgnoringBattery())
                    }
                    "openBatteryExemption" -> {
                        openBatteryExemption()
                        result.success(null)
                    }
                    "openSamsungNeverSleep" -> {
                        openSamsungNeverSleep()
                        result.success(null)
                    }
                    "openAppDetails" -> {
                        startActivity(
                            Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
                                .setData(Uri.parse("package:$packageName"))
                                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                        )
                        result.success(null)
                    }
                    "openNotificationSettings" -> {
                        openNotificationSettings()
                        result.success(null)
                    }
                    "openBluetoothSettings" -> {
                        startActivity(
                            Intent(Settings.ACTION_BLUETOOTH_SETTINGS)
                                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                        )
                        result.success(null)
                    }
                    "chime" -> {
                        val kind = call.argument<String>("kind") ?: "no"
                        WingsChime.play(this, kind)
                        result.success(null)
                    }
                    "micRoute" -> {
                        result.success(WingsMic.route)
                    }
                    "park" -> {
                        WingsPark.park(applicationContext, keepBle = false, tellDart = false)
                        result.success(null)
                    }
                    else -> result.notImplemented()
                }
            }
        maybeResume(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        maybeResume(intent)
    }

    override fun onDestroy() {
        if (WingsPark.dartChannel != null) {
            // Channel dies with the engine; Recents swipe parks from the FGS.
            WingsPark.dartChannel = null
        }
        super.onDestroy()
    }

    private fun maybeResume(intent: Intent?) {
        if (intent?.action == WingsPark.ACTION_RESUME) {
            WingsPark.tell("resumeListen")
        }
    }

    private fun isIgnoringBattery(): Boolean {
        if (Build.VERSION.SDK_INT < 23) return true
        val pm = getSystemService(PowerManager::class.java)
        return pm.isIgnoringBatteryOptimizations(packageName)
    }

    private fun openBatteryExemption() {
        // One-shot Samsung/Android dialog. If denied, fall back to the list.
        try {
            val req = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
                .setData(Uri.parse("package:$packageName"))
            startActivity(req)
            return
        } catch (_: Exception) {
        }
        startActivity(
            Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
        )
    }

    private fun openSamsungNeverSleep() {
        // One UI: Device care → Battery → Background usage limits → Never sleeping.
        val tries = listOf(
            Intent().setComponent(
                ComponentName(
                    "com.samsung.android.lool",
                    "com.samsung.android.sm.battery.ui.BatteryActivity",
                ),
            ),
            Intent().setComponent(
                ComponentName(
                    "com.samsung.android.lool",
                    "com.samsung.android.sm.ui.battery.BatteryActivity",
                ),
            ),
            Intent("com.samsung.android.sm.ACTION_BATTERY"),
        )
        for (i in tries) {
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            try {
                startActivity(i)
                return
            } catch (_: Exception) {
            }
        }
        openBatteryExemption()
    }

    private fun openNotificationSettings() {
        val i = if (Build.VERSION.SDK_INT >= 26) {
            Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                .putExtra(Settings.EXTRA_APP_PACKAGE, packageName)
        } else {
            Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
                .setData(Uri.parse("package:$packageName"))
        }
        startActivity(i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }
}
