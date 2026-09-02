package com.wings.wings_app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Log
import io.flutter.plugin.common.EventChannel
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * 16 kHz mono PCM16. A2DP cannot record — the earpiece mic is HFP/SCO only.
 * Start SCO with the recorder and pin AudioRecord to that input.
 * Do not setCommunicationDevice / MODE_IN_COMMUNICATION (disconnects buds).
 * Stop SCO only when listening fully stops (park / EventChannel cancel),
 * not on chime or motion.
 */
class WingsMic(private val ctx: Context) : EventChannel.StreamHandler {
    companion object {
        private const val TAG = "WingsMic"

        @Volatile
        var route: String = "phone"

        @Volatile
        var scoUp: Boolean = false

        @Volatile
        private var live: WingsMic? = null

        @Volatile
        private var boundName: String = ""

        @Volatile
        private var boundId: Int = -1

        /** Idempotent. Safe if the EventChannel never got onCancel (process death). */
        fun releaseCapture(app: Context) {
            live?.shutdown()
            forceStopSco(app)
            route = "phone"
            scoUp = false
            boundName = ""
            boundId = -1
        }

        fun forceStopSco(app: Context) {
            try {
                val am = app.getSystemService(Context.AUDIO_SERVICE) as AudioManager
                am.isBluetoothScoOn = false
                am.stopBluetoothSco()
            } catch (_: Exception) {
            }
        }

        private fun isBtAudio(t: Int): Boolean {
            if (t == AudioDeviceInfo.TYPE_BLUETOOTH_SCO) return true
            if (t == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP) return true
            if (Build.VERSION.SDK_INT >= 31 && t == AudioDeviceInfo.TYPE_BLE_HEADSET) {
                return true
            }
            return false
        }
    }

    private var rec: AudioRecord? = null
    private var thread: Thread? = null
    private var sink: EventChannel.EventSink? = null
    private var scoReceiver: BroadcastReceiver? = null
    private var pcmThread: HandlerThread? = null
    private var pcmHandler: Handler? = null
    private var deviceCb: AudioDeviceCallback? = null
    private val main = Handler(Looper.getMainLooper())
    private val shutLock = Any()
    @Volatile
    private var down = false

    override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
        shutdown()
        down = false
        live = this
        sink = events
        val app = ctx.applicationContext
        val ht = HandlerThread("wings-mic-pcm")
        ht.start()
        pcmThread = ht
        val pcmH = Handler(ht.looper)
        pcmHandler = pcmH
        thread = Thread {
            val am = app.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            startSco(app, am)
            if (down) return@Thread
            val sr = 16000
            val min = AudioRecord.getMinBufferSize(
                sr,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
            )
            val frameBytes = sr / 50 * 2
            val recBuf = min.coerceAtLeast(frameBytes * 8)
            val sources = intArrayOf(
                MediaRecorder.AudioSource.VOICE_RECOGNITION,
                MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                MediaRecorder.AudioSource.UNPROCESSED,
                MediaRecorder.AudioSource.MIC,
            )
            var r: AudioRecord? = null
            for (src in sources) {
                try {
                    val cand = AudioRecord(
                        src,
                        sr,
                        AudioFormat.CHANNEL_IN_MONO,
                        AudioFormat.ENCODING_PCM_16BIT,
                        recBuf,
                    )
                    if (cand.state == AudioRecord.STATE_INITIALIZED) {
                        r = cand
                        break
                    }
                    cand.release()
                } catch (_: Exception) {
                }
            }
            if (r == null || down) return@Thread
            rec = r
            bindHeadsetInput(am, r)
            watchDevices(app, am)
            try {
                r.startRecording()
            } catch (_: Exception) {
            }
            try {
                Thread.sleep(200)
            } catch (_: InterruptedException) {
                return@Thread
            }
            if (down) return@Thread
            bindHeadsetInput(am, r)
            val chunk = ByteArray(frameBytes)
            while (!Thread.currentThread().isInterrupted && !down) {
                val n = try {
                    r.read(chunk, 0, chunk.size)
                } catch (_: Exception) {
                    break
                }
                if (n > 0) {
                    val copy = chunk.copyOf(n)
                    pcmH.post { sink?.success(copy) }
                }
            }
        }.also { it.start() }
    }

    override fun onCancel(arguments: Any?) {
        shutdown()
    }

    fun shutdown() {
        synchronized(shutLock) {
            if (down && rec == null && thread == null) return
            down = true
            if (live === this) live = null
            thread?.interrupt()
            thread = null
            pcmHandler?.removeCallbacksAndMessages(null)
            pcmThread?.quitSafely()
            pcmHandler = null
            pcmThread = null
            try {
                rec?.stop()
            } catch (_: Exception) {
            }
            rec?.release()
            rec = null
            sink = null
            unwatchDevices()
            stopSco(ctx.applicationContext)
            forceStopSco(ctx.applicationContext)
            route = "phone"
            scoUp = false
        }
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
            am.isBluetoothScoOn = false
            am.stopBluetoothSco()
        } catch (_: Exception) {
        }
    }

    private fun watchDevices(app: Context, am: AudioManager) {
        if (Build.VERSION.SDK_INT < 23) return
        val cb = object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>) {
                maybeParkNewBt(am, added)
            }

            override fun onAudioDevicesRemoved(removed: Array<out AudioDeviceInfo>) {
                val id = boundId
                if (id < 0) return
                if (removed.any { it.id == id }) {
                    Log.i(TAG, "auto-park: bound SCO gone id=$id name=$boundName")
                    WingsPark.park(app, keepBle = true, tellDart = true)
                }
            }
        }
        deviceCb = cb
        try {
            am.registerAudioDeviceCallback(cb, main)
        } catch (_: Exception) {
        }
    }

    private fun unwatchDevices() {
        val cb = deviceCb ?: return
        deviceCb = null
        try {
            val am = ctx.applicationContext
                .getSystemService(Context.AUDIO_SERVICE) as AudioManager
            if (Build.VERSION.SDK_INT >= 23) {
                am.unregisterAudioDeviceCallback(cb)
            }
        } catch (_: Exception) {
        }
    }

    private fun maybeParkNewBt(am: AudioManager, added: Array<out AudioDeviceInfo>) {
        val saved = boundName
        if (saved.isEmpty() || down) return
        for (d in added) {
            if (!isBtAudio(d.type)) continue
            val name = d.productName?.toString()?.trim().orEmpty()
            if (name.isEmpty()) continue
            if (name.equals(saved, ignoreCase = true)) continue
            Log.i(TAG, "auto-park: new BT audio '$name' (saved headset '$saved')")
            WingsPark.park(ctx.applicationContext, keepBle = true, tellDart = true)
            return
        }
        if (Build.VERSION.SDK_INT >= 31) {
            val comm = try {
                am.communicationDevice
            } catch (_: Exception) {
                null
            } ?: return
            if (!isBtAudio(comm.type)) return
            val name = comm.productName?.toString()?.trim().orEmpty()
            if (name.isEmpty() || name.equals(saved, ignoreCase = true)) return
            Log.i(TAG, "auto-park: comm device '$name' (saved headset '$saved')")
            WingsPark.park(ctx.applicationContext, keepBle = true, tellDart = true)
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
            boundName = route
            boundId = pick.id
            scoUp = pick.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO ||
                (Build.VERSION.SDK_INT >= 31 && pick.type == AudioDeviceInfo.TYPE_BLE_HEADSET)
        } else {
            route = if (am.isBluetoothScoOn) "headset" else "phone"
            scoUp = am.isBluetoothScoOn
        }
    }
}
