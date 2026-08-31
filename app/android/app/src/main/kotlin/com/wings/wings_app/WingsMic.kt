package com.wings.wings_app

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Handler
import android.os.Looper
import io.flutter.plugin.common.EventChannel

/**
 * 16 kHz mono PCM16 for sherpa-onnx KWS + speaker embed.
 * PTT, enroll, or always-listen. VOICE_COMMUNICATION prefers HFP headset.
 */
class WingsMic : EventChannel.StreamHandler {
    private var rec: AudioRecord? = null
    private var thread: Thread? = null
    private var sink: EventChannel.EventSink? = null
    private val main = Handler(Looper.getMainLooper())

    override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
        sink = events
        val sr = 16000
        val min = AudioRecord.getMinBufferSize(
            sr,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
        )
        val buf = min.coerceAtLeast(sr / 5) * 2
        val r = AudioRecord(
            MediaRecorder.AudioSource.VOICE_COMMUNICATION,
            sr,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
            buf,
        )
        rec = r
        r.startRecording()
        thread = Thread {
            val chunk = ByteArray(buf)
            while (!Thread.currentThread().isInterrupted) {
                val n = r.read(chunk, 0, chunk.size)
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
    }
}
