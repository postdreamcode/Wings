package com.wings.wings_app

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import kotlin.math.PI
import kotlin.math.sin

/**
 * Tones on the call stream while headset SCO is up, otherwise media/A2DP.
 * Does not start or stop SCO.
 */
object WingsChime {
    private val lock = Any()

    fun play(ctx: Context, kind: String) {
        Thread {
            synchronized(lock) {
                try {
                    val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
                    val sco = WingsMic.scoUp || am.isBluetoothScoOn
                    when (kind) {
                        "wake" -> tones(
                            sco,
                            listOf(
                                Triple(880.0, 90, 0.50),
                                Triple(1320.0, 130, 0.55),
                            ),
                        )
                        "ok" -> tones(
                            sco,
                            listOf(
                                Triple(1400.0, 80, 0.52),
                                Triple(1760.0, 110, 0.58),
                            ),
                        )
                        else -> tones(
                            sco,
                            listOf(Triple(220.0, 200, 0.55)),
                        )
                    }
                } catch (_: Exception) {
                }
            }
        }.start()
    }

    private fun tones(sco: Boolean, notes: List<Triple<Double, Int, Double>>) {
        val sr = 16000
        val gap = sr * 30 / 1000
        val pcm = ArrayList<Short>()
        for ((hz, ms, amp) in notes) {
            val n = (sr * ms / 1000).coerceAtLeast(1)
            val fade = (n / 10).coerceAtLeast(1)
            for (i in 0 until n) {
                val env = when {
                    i < fade -> i.toDouble() / fade
                    i > n - fade -> (n - i).toDouble() / fade
                    else -> 1.0
                }.coerceIn(0.0, 1.0)
                val s = (sin(2.0 * PI * hz * i / sr) * amp * env * 32767.0)
                    .toInt()
                    .coerceIn(-32767, 32767)
                pcm.add(s.toShort())
            }
            repeat(gap) { pcm.add(0) }
        }
        val bytes = ByteArray(pcm.size * 2)
        var o = 0
        for (s in pcm) {
            val v = s.toInt()
            bytes[o++] = (v and 0xff).toByte()
            bytes[o++] = ((v shr 8) and 0xff).toByte()
        }
        val min = AudioTrack.getMinBufferSize(
            sr,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
        )
        val attrs = AudioAttributes.Builder()
            .setUsage(
                if (sco) AudioAttributes.USAGE_VOICE_COMMUNICATION
                else AudioAttributes.USAGE_MEDIA,
            )
            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
            .build()
        val fmt = AudioFormat.Builder()
            .setSampleRate(sr)
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
            .build()
        val track = AudioTrack.Builder()
            .setAudioAttributes(attrs)
            .setAudioFormat(fmt)
            .setBufferSizeInBytes(min.coerceAtLeast(bytes.size))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        try {
            track.play()
            var off = 0
            while (off < bytes.size) {
                val n = track.write(bytes, off, bytes.size - off)
                if (n <= 0) break
                off += n
            }
            val durMs = bytes.size / 2 * 1000 / sr + 60
            Thread.sleep(durMs.toLong())
        } finally {
            try {
                track.stop()
            } catch (_: Exception) {
            }
            track.release()
        }
    }
}
