package io.github.nicedayzhu.three_ds_live.stream

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean

/**
 * IMA ADPCM stereo decoder and low-latency AudioTrack writer.
 *
 * The handheld sends 175-byte records carrying 160 stereo frames at 32.728 kHz; the
 * phone plays them at 48 kHz after a linear resample, matching the PC player's
 * 48 kHz speaker timeline.
 */
class AudioPlayer {
    companion object {
        const val SOURCE_RATE = 32728.5
        const val OUTPUT_RATE = 48000
        const val BLOCK_SAMPLES = 160
        private val INDEX_TABLE = intArrayOf(-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8)
        private val STEP_TABLE = intArrayOf(
            7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
            50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253,
            279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
            1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
            4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
            16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
        )
    }

    private val queue = ArrayBlockingQueue<ShortArray>(12)
    private val running = AtomicBoolean(false)
    private var track: AudioTrack? = null
    private var thread: Thread? = null
    private var lastLeft = 0
    private var lastRight = 0
    private var carry = 0.0
    private val resampled = ArrayList<Short>(512)

    @Volatile var muted = false
    @Volatile var volume = 1f
    @Volatile var pcmSink: ((ShortArray) -> Unit)? = null

    var blocksPlayed = 0L
        private set
    var droppedBlocks = 0L
        private set

    fun start() {
        if (running.getAndSet(true)) return
        val minBuffer = AudioTrack.getMinBufferSize(
            OUTPUT_RATE,
            AudioFormat.CHANNEL_OUT_STEREO,
            AudioFormat.ENCODING_PCM_16BIT,
        ).coerceAtLeast(4096)
        val audioTrack = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(OUTPUT_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .build(),
            )
            .setBufferSizeInBytes(minBuffer * 2)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build()
        track = audioTrack
        audioTrack.play()
        thread = Thread({ pump() }, "3ds-live-audio").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    fun stop() {
        if (!running.getAndSet(false)) return
        queue.clear()
        thread?.join(500)
        thread = null
        track?.let {
            try {
                it.pause()
                it.flush()
                it.stop()
            } catch (_: Throwable) {
            }
            it.release()
        }
        track = null
    }

    fun reset() {
        queue.clear()
        blocksPlayed = 0
        droppedBlocks = 0
        lastLeft = 0
        lastRight = 0
        carry = 0.0
    }

    /** Decodes one 167-byte IMA record into the playout queue. */
    fun submitRecord(record: ByteArray, offset: Int, length: Int, nowNanos: Long) {
        if (length < 167) return
        val leftSample = ((record[offset].toInt() and 0xFF) or (record[offset + 1].toInt() shl 8)).toShort().toInt()
        val leftIndex = record[offset + 2].toInt() and 0xFF
        val rightSample = ((record[offset + 4].toInt() and 0xFF) or (record[offset + 5].toInt() shl 8)).toShort().toInt()
        val rightIndex = record[offset + 6].toInt() and 0xFF
        if (leftIndex > 88 || rightIndex > 88) return

        val pcm = ShortArray(BLOCK_SAMPLES * 2)
        pcm[0] = leftSample.toShort()
        pcm[1] = rightSample.toShort()
        var l = leftSample
        var li = leftIndex
        var r = rightSample
        var ri = rightIndex
        for (i in 0 until 159) {
            val byte = record[offset + 8 + i].toInt() and 0xFF
            val ln = byte and 0x0F
            val rn = byte ushr 4

            var step = STEP_TABLE[li]
            var diff = step ushr 3
            if (ln and 4 != 0) diff += step
            if (ln and 2 != 0) diff += step ushr 1
            if (ln and 1 != 0) diff += step ushr 2
            l = if (ln and 8 != 0) l - diff else l + diff
            if (l > 32767) l = 32767
            if (l < -32768) l = -32768
            li = (li + INDEX_TABLE[ln]).coerceIn(0, 88)

            step = STEP_TABLE[ri]
            diff = step ushr 3
            if (rn and 4 != 0) diff += step
            if (rn and 2 != 0) diff += step ushr 1
            if (rn and 1 != 0) diff += step ushr 2
            r = if (rn and 8 != 0) r - diff else r + diff
            if (r > 32767) r = 32767
            if (r < -32768) r = -32768
            ri = (ri + INDEX_TABLE[rn]).coerceIn(0, 88)

            pcm[(i + 1) * 2] = l.toShort()
            pcm[(i + 1) * 2 + 1] = r.toShort()
        }
        lastLeft = l
        lastRight = r
        if (!running.get()) return
        val resampledPcm = resample(pcm)
        pcmSink?.invoke(resampledPcm)
        if (!queue.offer(resampledPcm)) {
            queue.poll()
            queue.offer(resampledPcm)
            droppedBlocks++
        }
    }

    /** 160 frames at 32.728 kHz -> about 235 frames at 48 kHz, continuous across blocks. */
    private fun resample(input: ShortArray): ShortArray {
        val ratio = SOURCE_RATE / OUTPUT_RATE
        resampled.clear()
        var t = carry
        while (true) {
            val index = t.toInt()
            if (index >= BLOCK_SAMPLES - 1) break
            val frac = t - index
            val left = input[index * 2]
            val nextLeft = input[(index + 1) * 2]
            val right = input[index * 2 + 1]
            val nextRight = input[(index + 1) * 2 + 1]
            resampled.add((left + (nextLeft - left) * frac).toInt().toShort())
            resampled.add((right + (nextRight - right) * frac).toInt().toShort())
            t += ratio
        }
        carry = t - BLOCK_SAMPLES
        if (carry < 0) carry = 0.0
        val out = ShortArray(resampled.size)
        for (i in resampled.indices) out[i] = resampled[i]
        return out
    }

    private fun pump() {
        while (running.get()) {
            val pcm = try {
                queue.poll(100, java.util.concurrent.TimeUnit.MILLISECONDS)
            } catch (_: InterruptedException) {
                return
            } ?: continue
            val audioTrack = track ?: continue
            try {
                audioTrack.setVolume(if (muted) 0f else volume)
                audioTrack.write(pcm, 0, pcm.size, AudioTrack.WRITE_BLOCKING)
                blocksPlayed++
            } catch (_: Throwable) {
                return
            }
        }
    }
}
