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
 */
class WingsFgService : Service() {
    companion object {
        const val CHANNEL_ID = "wings_ble"
        const val NOTIF_ID = 170
        const val EXTRA_TEXT = "text"
        const val EXTRA_MIC = "mic"

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

        /// Text-only refresh. Do NOT startForegroundService from background
        /// (Android 12+ / screen-off will throw).
        fun updateNotification(ctx: Context, text: String) {
            val app = ctx.applicationContext
            ensureChannel(app)
            val mgr = app.getSystemService(NotificationManager::class.java)
            mgr.notify(NOTIF_ID, buildNotification(app, text))
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

        fun buildNotification(ctx: Context, text: String): Notification {
            val launch = ctx.packageManager.getLaunchIntentForPackage(ctx.packageName)
            val pi = PendingIntent.getActivity(
                ctx,
                0,
                launch,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
            )
            val b = if (Build.VERSION.SDK_INT >= 26) {
                Notification.Builder(ctx, CHANNEL_ID)
            } else {
                @Suppress("DEPRECATION")
                Notification.Builder(ctx)
            }
            return b.setContentTitle("Wings")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setContentIntent(pi)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .build()
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val text = intent?.getStringExtra(EXTRA_TEXT) ?: "Wings connected"
        val mic = intent?.getBooleanExtra(EXTRA_MIC, false) ?: false
        ensureChannel(this)
        val notif = buildNotification(this, text)
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
        return START_STICKY
    }
}
