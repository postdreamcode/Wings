package com.wings.wings_app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Build
import android.os.Handler
import android.os.Looper
import io.flutter.plugin.common.EventChannel
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * 16 kHz mono PCM16. A2DP cannot record — the earpiece mic is HFP/SCO only.
 * Start SCO with the recorder and pin AudioRecord to that input.
 * Do not setCommunicationDevice / MODE_IN_COMMUNICATION (disconnects buds).
 * Stop SCO only when the EventChannel is cancelled, not on chime or motion.
 */
class WingsMic(private val ctx: Context) : EventChannel.StreamHandler {
    companion object {
        @Volatile
        var route: String = "phone"

        @Volatile
        var scoUp: Boolean = false
    }

    private var rec: AudioRecord? = null
    private var thread: Thread? = null
    private var sink: EventChannel.EventSink? = null
    private var scoReceiver: BroadcastReceiver? = null
    private val main = Handler(Looper.getMainLooper())

    override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
        sink = events
        val app = ctx.applicationContext
        thread = Thread {
            val am = app.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            startSco(app, am)
            val sr = 16000
            val min = AudioRecord.getMinBufferSize(
                sr,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
            )
            val frameBytes = sr / 50 * 2
            val recBuf = min.coerceAtLeast(frameBytes * 8)
            val r = AudioRecord(
                MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                sr,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                recBuf,
            )
            rec = r
            bindHeadsetInput(am, r)
            try {
                r.startRecording()
            } catch (_: Exception) {
            }
            try {
                Thread.sleep(200)
            } catch (_: InterruptedException) {
                return@Thread
            }
            bindHeadsetInput(am, r)
            val chunk = ByteArray(frameBytes)
            while (!Thread.currentThread().isInterrupted) {
                val n = try {
                    r.read(chunk, 0, chunk.size)
                } catch (_: Exception) {
                    break
                }
                if (n > 0) {
                    val copy = chunk.copyOf(n)
                    main.post { sink?.success(copy) }
                }
            }
        }.also { it.start() }
    }

    override fun onCancel(arguments: Any?) {
        thread?.interrupt()
        thread = null
        try {
            rec?.stop()
        } catch (_: Exception) {
        }
        rec?.release()
        rec = null
        sink = null
        stopSco(ctx.applicationContext)
        route = "phone"
        scoUp = false
    }

    private fun startSco(app: Context, am: AudioManager) {
        if (!am.isBluetoothScoAvailableOffCall) return
        val latch = CountDownLatch(1)
        val recv = object : BroadcastReceiver() {
            override fun onReceive(c: Context?, i: Intent?) {
                val st = i?.getIntExtra(AudioManager.EXTRA_SCO_AUDIO_STATE, -1)
                    ?: return
                if (st == AudioManager.SCO_AUDIO_STATE_CONNECTED) latch.countDown()
            }
        }
        scoReceiver = recv
        val filter = IntentFilter(AudioManager.ACTION_SCO_AUDIO_STATE_UPDATED)
        try {
            if (Build.VERSION.SDK_INT >= 33) {
                app.registerReceiver(recv, filter, Context.RECEIVER_NOT_EXPORTED)
            } else {
                @Suppress("DEPRECATION")
                app.registerReceiver(recv, filter)
            }
        } catch (_: Exception) {
        }
        try {
            am.startBluetoothSco()
            am.isBluetoothScoOn = true
            latch.await(1500, TimeUnit.MILLISECONDS)
        } catch (_: Exception) {
        }
    }

    private fun stopSco(app: Context) {
        try {
            scoReceiver?.let { app.unregisterReceiver(it) }
        } catch (_: Exception) {
        }
        scoReceiver = null
        try {
            val am = app.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            if (am.isBluetoothScoOn) {
                am.isBluetoothScoOn = false
                am.stopBluetoothSco()
            }
        } catch (_: Exception) {
        }
    }

    private fun bindHeadsetInput(am: AudioManager, r: AudioRecord) {
        if (Build.VERSION.SDK_INT < 23) {
            route = "phone"
            scoUp = am.isBluetoothScoOn
            return
        }
        val ins = am.getDevices(AudioManager.GET_DEVICES_INPUTS)
        val pick = ins.firstOrNull { it.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO }
            ?: ins.firstOrNull {
                Build.VERSION.SDK_INT >= 31 && it.type == AudioDeviceInfo.TYPE_BLE_HEADSET
            }
            ?: ins.firstOrNull { it.type == AudioDeviceInfo.TYPE_WIRED_HEADSET }
        if (pick != null) {
            try {
                r.setPreferredDevice(pick)
            } catch (_: Exception) {
            }
            val name = pick.productName?.toString()?.trim().orEmpty()
            route = if (name.isEmpty()) "headset" else name
            scoUp = pick.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO ||
                (Build.VERSION.SDK_INT >= 31 && pick.type == AudioDeviceInfo.TYPE_BLE_HEADSET)
        } else {
            route = if (am.isBluetoothScoOn) "headset" else "phone"
            scoUp = am.isBluetoothScoOn
        }
    }
}
