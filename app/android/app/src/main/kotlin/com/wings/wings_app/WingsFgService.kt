package com.wings.wings_app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

/**
 * Keeps the process alive so flutter_blue_plus GATT is not torn down
 * when the Activity pauses / screen turns off.
 *
 * Start this from the visible Activity only. microphone type needs
 * RECORD_AUDIO and must be added while the UI is in front (API 34+).
 *
 * START_NOT_STICKY: do not re-arm SCO after the user swipes Recents.
 */
class WingsFgService : Service() {
    companion object {
        const val CHANNEL_ID = "wings_ble"
        const val NOTIF_ID = 170
        const val EXTRA_TEXT = "text"
        const val EXTRA_MIC = "mic"
        const val ACTION_PARK = WingsPark.ACTION_PARK
        const val ACTION_DISCONNECT = WingsPark.ACTION_DISCONNECT

        @Volatile
        private var instance: WingsFgService? = null

        @Volatile
        private var listening = false

        fun start(ctx: Context, text: String, mic: Boolean) {
            val i = Intent(ctx, WingsFgService::class.java)
                .putExtra(EXTRA_TEXT, text)
                .putExtra(EXTRA_MIC, mic)
            if (Build.VERSION.SDK_INT >= 26) {
                ctx.startForegroundService(i)
            } else {
                @Suppress("DEPRECATION")
                ctx.startService(i)
            }
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, WingsFgService::class.java))
        }

        fun demoteToConnectedDevice(ctx: Context) {
            val app = ctx.applicationContext
            val s = instance
            if (s != null) {
                s.applyForeground("Wings connected", mic = false)
                return
            }
            start(app, "Wings connected", mic = false)
        }

        /// Text-only refresh. Do NOT startForegroundService from background
        /// (Android 12+ / screen-off will throw).
        fun updateNotification(ctx: Context, text: String) {
            val app = ctx.applicationContext
            ensureChannel(app)
            val mgr = app.getSystemService(NotificationManager::class.java)
            mgr.notify(NOTIF_ID, buildNotification(app, text, listening))
        }

        fun ensureChannel(ctx: Context) {
            if (Build.VERSION.SDK_INT < 26) return
            val mgr = ctx.getSystemService(NotificationManager::class.java)
            val ch = NotificationChannel(
                CHANNEL_ID,
                "Wings link",
                NotificationManager.IMPORTANCE_LOW,
            )
            ch.setShowBadge(false)
            mgr.createNotificationChannel(ch)
        }

        private fun piFlags(): Int {
            return PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        }

        fun buildNotification(ctx: Context, text: String, listeningNow: Boolean): Notification {
            val launch = ctx.packageManager.getLaunchIntentForPackage(ctx.packageName)
                ?: Intent(ctx, MainActivity::class.java)
            launch.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP)
            val tap = PendingIntent.getActivity(ctx, 0, launch, piFlags())

            val b = if (Build.VERSION.SDK_INT >= 26) {
                Notification.Builder(ctx, CHANNEL_ID)
            } else {
                @Suppress("DEPRECATION")
                Notification.Builder(ctx)
            }
            b.setContentTitle("Wings")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setContentIntent(tap)
                .setOngoing(true)
                .setOnlyAlertOnce(true)

            val parkI = Intent(ctx, WingsFgService::class.java).setAction(ACTION_PARK)
            val parkPi = PendingIntent.getService(ctx, 1, parkI, piFlags())
            val discI = Intent(ctx, WingsFgService::class.java).setAction(ACTION_DISCONNECT)
            val discPi = PendingIntent.getService(ctx, 2, discI, piFlags())
            val resumeI = Intent(ctx, MainActivity::class.java)
                .setAction(WingsPark.ACTION_RESUME)
                .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP)
            val resumePi = PendingIntent.getActivity(ctx, 3, resumeI, piFlags())

            if (listeningNow) {
                @Suppress("DEPRECATION")
                b.addAction(0, "Park", parkPi)
                @Suppress("DEPRECATION")
                b.addAction(0, "Disconnect", discPi)
            } else {
                @Suppress("DEPRECATION")
                b.addAction(0, "Resume listening", resumePi)
                @Suppress("DEPRECATION")
                b.addAction(0, "Disconnect", discPi)
            }
            return b.build()
        }
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
    }

    override fun onDestroy() {
        if (instance === this) instance = null
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onTaskRemoved(rootIntent: Intent?) {
        WingsPark.park(applicationContext, keepBle = false, tellDart = true)
        stopSelf()
        super.onTaskRemoved(rootIntent)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_PARK -> {
                WingsPark.park(applicationContext, keepBle = true, tellDart = true)
                return START_NOT_STICKY
            }
            ACTION_DISCONNECT -> {
                WingsPark.disconnect(applicationContext)
                return START_NOT_STICKY
            }
        }
        val text = intent?.getStringExtra(EXTRA_TEXT) ?: "Wings connected"
        val mic = intent?.getBooleanExtra(EXTRA_MIC, false) ?: false
        applyForeground(text, mic)
        return START_NOT_STICKY
    }

    fun applyForeground(text: String, mic: Boolean) {
        listening = mic
        ensureChannel(this)
        val notif = buildNotification(this, text, mic)
        val type = if (mic) {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE or
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        } else {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        }
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIF_ID, notif, type)
        } else {
            @Suppress("DEPRECATION")
            startForeground(NOTIF_ID, notif)
        }
    }
}
