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

    private val queue = ArrayBlockingQueue<ShortArray>(32)
    private val running = AtomicBoolean(false)

    private var thread: Thread? = null
    private var lastLeft = 0
    private var lastRight = 0
    private var carry = 0.0
    private var outputRate = OUTPUT_RATE
    private var nativeRate = false
    private var prefilled = false

    // Small reorder window: UDP can deliver R9A1 packets out of order and a missing
    // block must be concealed instead of being skipped abruptly.
    private val pending = java.util.TreeMap<Int, ShortArray>()
    // 16 blocks ~= 78 ms of jitter tolerance; the sender repeats each batch once
    // about two datagrams later, so a single lost packet fits inside this window.
    private val reorderWindow = 16
    private var expectedBlock = Int.MIN_VALUE
    private var lastPcm: ShortArray? = null
    private var fadeInNext = false
    private var gaps = 0L
    private var reordered = 0L
    private var underruns = 0L
    private var lastUnderrun = 0
    private var peakQueue = 0
    private var appliedVolume = -1f
    private var pendingSince = 0L

    @Volatile var muted = false
    @Volatile var volume = 1f
    @Volatile var pcmSink: ((ShortArray) -> Unit)? = null

    var blocksPlayed = 0L
        private set
    var droppedBlocks = 0L
        private set
    val gapBlocks: Long get() = gaps
    val lateBlocks: Long get() = reordered
    val underrunCount: Long get() = underruns

    fun start() {
        if (running.getAndSet(true)) return
        // Prefer the console's own rate so the system mixer does the resampling once.
        val track = createTrack(SOURCE_RATE.toInt()) ?: createTrack(OUTPUT_RATE)
        if (track == null) {
            running.set(false)
            return
        }
        track.play()
        AudioTrackHolder.track = track
        Logs.i(
            "3ds-live",
            "audio track rate=${track.sampleRate} frames=${track.bufferSizeInFrames} " +
                "native=$nativeRate",
        )
        thread = Thread({ pump() }, "3ds-live-audio").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    private fun createTrack(rate: Int): AudioTrack? {
        return try {
            val minBuffer = AudioTrack.getMinBufferSize(
                rate,
                AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT,
            ).coerceAtLeast(4096)
            AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(rate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .build(),
            )
            .setBufferSizeInBytes(minBuffer * 4)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build()
            .also {
                outputRate = rate
                nativeRate = rate == SOURCE_RATE.toInt()
                prefilled = false
                carry = 0.0
            }
        } catch (_: Throwable) {
            null
        }
    }

    private object AudioTrackHolder {
        @Volatile var track: AudioTrack? = null
    }

    fun stop() {
        if (!running.getAndSet(false)) return
        queue.clear()
        thread?.join(500)
        thread = null
        AudioTrackHolder.track?.let {
            try {
                it.pause()
                it.flush()
                it.stop()
            } catch (_: Throwable) {
            }
            it.release()
        }
        AudioTrackHolder.track = null
    }

    fun reset() {
        queue.clear()
        pending.clear()
        pendingSince = 0L
        expectedBlock = Int.MIN_VALUE
        lastPcm = null
        fadeInNext = false
        blocksPlayed = 0
        droppedBlocks = 0
        gaps = 0
        reordered = 0
        underruns = 0
        peakQueue = 0
        appliedVolume = -1f
        lastLeft = 0
        lastRight = 0
        carry = 0.0
    }

    /** Decodes one 167-byte IMA record into the playout queue. */
    fun submitRecord(blockSeq: Int, record: ByteArray, offset: Int, length: Int, nowNanos: Long) {
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
        if (!running.get()) return
        dispatch(blockSeq, pcm, nowNanos)
    }

    /** Orders blocks, conceals gaps and hands contiguous PCM to the player. */
    private fun dispatch(blockSeq: Int, pcm: ShortArray, nowNanos: Long) {
        if (expectedBlock == Int.MIN_VALUE) expectedBlock = blockSeq
        val delta = blockSeq - expectedBlock
        if (delta < 0) {
            reordered++
            if (reordered <= 20) {
                Logs.w("3ds-live", "audio late block=$blockSeq expected=$expectedBlock")
            }
            return
        }
        if (pending.containsKey(blockSeq)) return
        if (delta > 128) {
            // Long gap (session restart or heavy loss): jump forward.
            pending.clear()
            expectedBlock = blockSeq
            gaps++
        }
        pending[blockSeq] = pcm
        if (pendingSince == 0L && expectedBlock < pending.firstKey()) pendingSince = nowNanos
        if (pending.size > reorderWindow) {
            val target = pending.firstKey()
            if (gaps < 20) {
                Logs.w(
                    "3ds-live",
                    "audio gap expected=$expectedBlock first=$target pending=${pending.size}",
                )
            }
            while (expectedBlock < target) {
                emit(conceal())
                fadeInNext = true
                expectedBlock++
                gaps++
            }
        }
        while (true) {
            val next = pending.remove(expectedBlock) ?: break
            emit(next)
            expectedBlock++
        }
        if (pending.isEmpty()) pendingSince = 0L
    }

    /**
     * Called from the engine tick. A missing block used to be concealed only after
     * 16 later blocks had queued, so a sparse burst could stall playout long enough
     * to drain AudioTrack (the underruns seen in the device log). Conceal after
     * 80 ms even when few later blocks are buffered.
     */
    fun tick(nowNanos: Long) {
        if (!running.get() || pending.isEmpty()) {
            pendingSince = 0L
            return
        }
        if (pendingSince == 0L) pendingSince = nowNanos
        if (nowNanos - pendingSince < 80_000_000L) return
        val target = pending.firstKey()
        if (target <= expectedBlock) {
            pendingSince = 0L
            return
        }
        Logs.w("3ds-live", "audio stall expected=$expectedBlock first=$target pending=${pending.size}")
        while (expectedBlock < target) {
            emit(conceal())
            fadeInNext = true
            expectedBlock++
            gaps++
        }
        pendingSince = nowNanos
    }

    private fun emit(pcm: ShortArray) {
        val input = if (fadeInNext) {
            fadeInNext = false
            fadeIn(pcm)
        } else {
            pcm
        }
        val out = if (nativeRate) input else resample(input)
        lastLeft = input[(BLOCK_SAMPLES - 1) * 2].toInt()
        lastRight = input[(BLOCK_SAMPLES - 1) * 2 + 1].toInt()
        lastPcm = input
        pcmSink?.invoke(out)
        if (!queue.offer(out)) {
            queue.poll()
            queue.offer(out)
            droppedBlocks++
        }
        if (queue.size > peakQueue) peakQueue = queue.size
    }

    /** Fades the previous block out so a lost block does not end in a hard step. */
    private fun conceal(): ShortArray {
        val last = lastPcm ?: return ShortArray(BLOCK_SAMPLES * 2)
        val copy = last.copyOf()
        for (i in 0 until BLOCK_SAMPLES) {
            val gain = (BLOCK_SAMPLES - i).toFloat() / BLOCK_SAMPLES
            copy[i * 2] = (copy[i * 2] * gain).toInt().toShort()
            copy[i * 2 + 1] = (copy[i * 2 + 1] * gain).toInt().toShort()
        }
        return copy
    }

    private fun fadeIn(pcm: ShortArray): ShortArray {
        val frames = minOf(64, BLOCK_SAMPLES)
        for (i in 0 until frames) {
            val gain = i.toFloat() / frames
            pcm[i * 2] = (pcm[i * 2] * gain).toInt().toShort()
            pcm[i * 2 + 1] = (pcm[i * 2 + 1] * gain).toInt().toShort()
        }
        return pcm
    }

    /**
     * 160 frames at 32.728 kHz -> about 235 frames at 48 kHz.
     *
     * The phase is continuous across blocks: p may start slightly negative and is
     * interpolated against the previous block's last sample, which keeps the stream
     * free of the per-block click the old clamp produced.
     */
    private fun resample(input: ShortArray): ShortArray {
        val ratio = SOURCE_RATE / outputRate
        val out = ArrayList<Short>(300)
        var p = carry
        while (p < BLOCK_SAMPLES - 1) {
            val index = kotlin.math.floor(p).toInt()
            val frac = p - index
            val left: Int
            val right: Int
            if (index < 0) {
                val f = p + 1.0
                left = (lastLeft + (input[0].toInt() - lastLeft) * f).toInt()
                right = (lastRight + (input[1].toInt() - lastRight) * f).toInt()
            } else {
                val a = input[index * 2].toInt()
                val b = input[(index + 1) * 2].toInt()
                val c = input[index * 2 + 1].toInt()
                val d = input[(index + 1) * 2 + 1].toInt()
                left = (a + (b - a) * frac).toInt()
                right = (c + (d - c) * frac).toInt()
            }
            out.add(left.coerceIn(-32768, 32767).toShort())
            out.add(right.coerceIn(-32768, 32767).toShort())
            p += ratio
        }
        carry = p - BLOCK_SAMPLES
        return ShortArray(out.size) { out[it] }
    }

    private fun pump() {
        while (running.get()) {
            if (!prefilled) {
                if (queue.size < 4) {
                    try {
                        Thread.sleep(5)
                    } catch (_: InterruptedException) {
                        return
                    }
                    continue
                }
                prefilled = true
            }
            val pcm = try {
                queue.poll(100, java.util.concurrent.TimeUnit.MILLISECONDS)
            } catch (_: InterruptedException) {
                return
            } ?: continue
            val audioTrack = AudioTrackHolder.track ?: continue
            try {
                audioTrack.setVolume(if (muted) 0f else volume)
                audioTrack.write(pcm, 0, pcm.size, AudioTrack.WRITE_BLOCKING)
                blocksPlayed++
                if (blocksPlayed % 200L == 0L) {
                    val totalUnderruns = try { audioTrack.underrunCount } catch (_: Throwable) { 0 }
                    if (totalUnderruns > lastUnderrun) {
                        underruns += (totalUnderruns - lastUnderrun).toLong()
                        lastUnderrun = totalUnderruns
                    }
                    Logs.i(
                        "3ds-live",
                        "audio blocks=$blocksPlayed queue=${queue.size} peak=$peakQueue " +
                            "gaps=$gaps reordered=$reordered dropped=$droppedBlocks " +
                            "underruns=$underruns",
                    )
                }
            } catch (_: Throwable) {
                return
            }
        }
    }
}
