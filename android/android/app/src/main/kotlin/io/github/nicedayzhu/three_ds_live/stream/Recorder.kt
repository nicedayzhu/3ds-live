package io.github.nicedayzhu.three_ds_live.stream

import android.content.ContentValues
import android.content.Context
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.view.Surface
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean

/**
 * MP4 recorder built on MediaCodec + MediaMuxer.
 *
 * Video is encoded from the same composed canvas the Texture shows (surface input,
 * so no RGB→YUV conversion per frame); audio is AAC fed with the decoded stereo PCM.
 * Sample timestamps come from System.nanoTime, shared by both tracks.
 */
class Recorder(private val context: Context) {
    companion object {
        private const val MIME_VIDEO = MediaFormat.MIMETYPE_VIDEO_AVC
        private const val MIME_AUDIO = MediaFormat.MIMETYPE_AUDIO_AAC
        private const val AUDIO_RATE = 48000
        private const val AUDIO_CHANNELS = 2
        private const val AUDIO_BITRATE = 128000
    }

    private var videoEncoder: MediaCodec? = null
    private var audioEncoder: MediaCodec? = null
    private var muxer: MediaMuxer? = null
    private var inputSurface: Surface? = null
    private var outputFile: File? = null
    private var videoTrack = -1
    private var audioTrack = -1
    private var muxerStarted = false
    private val lock = Any()
    private val running = AtomicBoolean(false)
    private var drainThread: Thread? = null
    private val audioStartNanos = AtomicLongHolder()
    private var samplesWritten = 0L
    private var startNanos = 0L
    private var width = 0
    private var height = 0

    private class AtomicLongHolder {
        @Volatile var value: Long = 0
    }

    val surface: Surface? get() = inputSurface
    val isRecording: Boolean get() = running.get()
    val path: String? get() = outputFile?.absolutePath

    fun start(width: Int, height: Int, bitrate: Int, withAudio: Boolean): String? {
        if (running.get()) return outputFile?.absolutePath
        this.width = width
        this.height = height
        val dir = File(
            context.getExternalFilesDir(Environment.DIRECTORY_MOVIES) ?: context.filesDir,
            "3DS Live",
        ).apply { mkdirs() }
        val name = "3DS-Live-${SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())}.mp4"
        val file = File(dir, name)
        return try {
            val mux = MediaMuxer(file.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
            muxer = mux
            val videoFormat = MediaFormat.createVideoFormat(MIME_VIDEO, width, height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
                setInteger(MediaFormat.KEY_FRAME_RATE, 30)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            }
            val video = MediaCodec.createEncoderByType(MIME_VIDEO).apply {
                configure(videoFormat, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                inputSurface = createInputSurface()
                start()
            }
            videoEncoder = video
            if (withAudio) {
                val audioFormat = MediaFormat.createAudioFormat(MIME_AUDIO, AUDIO_RATE, AUDIO_CHANNELS).apply {
                    setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
                    setInteger(MediaFormat.KEY_BIT_RATE, AUDIO_BITRATE)
                    setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384)
                }
                audioEncoder = MediaCodec.createEncoderByType(MIME_AUDIO).apply {
                    configure(audioFormat, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                    start()
                }
            }
            outputFile = file
            startNanos = System.nanoTime()
            audioStartNanos.value = 0
            samplesWritten = 0
            videoTrack = -1
            audioTrack = -1
            muxerStarted = false
            running.set(true)
            drainThread = Thread({ drainLoop() }, "3ds-live-mux").apply { start() }
            file.absolutePath
        } catch (error: Throwable) {
            releaseQuietly()
            null
        }
    }

    fun stop(): String? {
        if (!running.getAndSet(false)) return null
        try {
            videoEncoder?.signalEndOfInputStream()
        } catch (_: Throwable) {
        }
        try {
            drainThread?.join(1500)
        } catch (_: InterruptedException) {
        }
        drainThread = null
        releaseQuietly()
        val file = outputFile ?: return null
        return publish(file) ?: file.absolutePath
    }

    /** Publishes the finished file to the system gallery on Android 10+. */
    private fun publish(file: File): String? {
        if (!file.exists() || file.length() == 0L) return null
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return null
        return try {
            val values = ContentValues().apply {
                put(MediaStore.Video.Media.DISPLAY_NAME, file.name)
                put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
                put(MediaStore.Video.Media.RELATIVE_PATH, Environment.DIRECTORY_MOVIES + "/3DS Live")
                put(MediaStore.Video.Media.IS_PENDING, 1)
            }
            val resolver = context.contentResolver
            val uri = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values) ?: return null
            resolver.openOutputStream(uri)?.use { stream ->
                file.inputStream().use { input -> input.copyTo(stream) }
            } ?: return null
            values.clear()
            values.put(MediaStore.Video.Media.IS_PENDING, 0)
            resolver.update(uri, values, null, null)
            file.delete()
            uri.toString()
        } catch (_: Throwable) {
            null
        }
    }

    /** Feeds decoded stereo PCM (48 kHz) into the AAC encoder. */
    fun writeAudio(pcm: ShortArray) {
        val encoder = audioEncoder ?: return
        if (!running.get()) return
        try {
            val index = encoder.dequeueInputBuffer(0)
            if (index < 0) return
            val buffer = encoder.getInputBuffer(index) ?: return
            buffer.clear()
            val bytes = ByteArray(pcm.size * 2)
            for (i in pcm.indices) {
                val value = pcm[i].toInt()
                bytes[i * 2] = (value and 0xFF).toByte()
                bytes[i * 2 + 1] = ((value shr 8) and 0xFF).toByte()
            }
            val chunk = minOf(buffer.remaining(), bytes.size)
            buffer.put(bytes, 0, chunk)
            val now = System.nanoTime()
            if (audioStartNanos.value == 0L) audioStartNanos.value = now
            val ptsUs = (now - audioStartNanos.value) / 1000
            encoder.queueInputBuffer(index, 0, chunk, ptsUs, 0)
            samplesWritten += chunk
        } catch (_: Throwable) {
        }
    }

    private fun drainLoop() {
        val info = MediaCodec.BufferInfo()
        while (running.get()) {
            var didWork = false
            for (encoder in listOfNotNull(videoEncoder, audioEncoder)) {
                try {
                    when (val index = encoder.dequeueOutputBuffer(info, 5_000)) {
                        MediaCodec.INFO_TRY_AGAIN_LATER -> {}
                        MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            val format = encoder.outputFormat
                            synchronized(lock) {
                                if (!muxerStarted) {
                                    if (encoder === videoEncoder) {
                                        videoTrack = muxer?.addTrack(format) ?: -1
                                    } else {
                                        audioTrack = muxer?.addTrack(format) ?: -1
                                    }
                                    maybeStartMuxer()
                                }
                            }
                            didWork = true
                        }
                        else -> {
                            if (index >= 0) {
                                val buffer = encoder.getOutputBuffer(index)
                                if (buffer != null && info.size > 0 &&
                                    (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0
                                ) {
                                    synchronized(lock) {
                                        val mux = muxer
                                        val track = if (encoder === videoEncoder) videoTrack else audioTrack
                                        if (muxerStarted && mux != null && track >= 0) {
                                            buffer.position(info.offset)
                                            buffer.limit(info.offset + info.size)
                                            try {
                                                mux.writeSampleData(track, buffer, info)
                                            } catch (_: Throwable) {
                                            }
                                        }
                                    }
                                }
                                encoder.releaseOutputBuffer(index, false)
                                didWork = true
                            }
                        }
                    }
                } catch (_: Throwable) {
                }
            }
            if (!didWork) {
                try {
                    Thread.sleep(2)
                } catch (_: InterruptedException) {
                    return
                }
            }
        }
        drainRemaining()
    }

    private fun maybeStartMuxer() {
        val mux = muxer ?: return
        if (muxerStarted) return
        val hasVideo = videoTrack >= 0
        val hasAudio = audioEncoder == null || audioTrack >= 0
        if (!hasVideo || !hasAudio) return
        try {
            mux.start()
            muxerStarted = true
        } catch (_: Throwable) {
        }
    }

    private fun drainRemaining() {
        val info = MediaCodec.BufferInfo()
        val deadline = System.nanoTime() + 500_000_000L
        while (System.nanoTime() < deadline) {
            var pending = false
            for (encoder in listOfNotNull(videoEncoder, audioEncoder)) {
                try {
                    when (val index = encoder.dequeueOutputBuffer(info, 2_000)) {
                        MediaCodec.INFO_TRY_AGAIN_LATER -> {}
                        MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                            synchronized(lock) {
                                if (!muxerStarted) {
                                    if (encoder === videoEncoder) {
                                        videoTrack = muxer?.addTrack(encoder.outputFormat) ?: -1
                                    } else {
                                        audioTrack = muxer?.addTrack(encoder.outputFormat) ?: -1
                                    }
                                    maybeStartMuxer()
                                }
                            }
                        }
                        else -> if (index >= 0) {
                            pending = true
                            val buffer = encoder.getOutputBuffer(index)
                            if (buffer != null && info.size > 0 &&
                                (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0
                            ) {
                                synchronized(lock) {
                                    val mux = muxer
                                    val track = if (encoder === videoEncoder) videoTrack else audioTrack
                                    if (muxerStarted && mux != null && track >= 0) {
                                        buffer.position(info.offset)
                                        buffer.limit(info.offset + info.size)
                                        try {
                                            mux.writeSampleData(track, buffer, info)
                                        } catch (_: Throwable) {
                                        }
                                    }
                                }
                            }
                            encoder.releaseOutputBuffer(index, false)
                        }
                    }
                } catch (_: Throwable) {
                }
            }
            if (!pending) {
                try {
                    Thread.sleep(2)
                } catch (_: InterruptedException) {
                    return
                }
            }
        }
    }

    private fun releaseQuietly() {
        synchronized(lock) {
            if (muxerStarted) {
                try {
                    muxer?.stop()
                } catch (_: Throwable) {
                }
            }
            muxerStarted = false
            try {
                muxer?.release()
            } catch (_: Throwable) {
            }
            muxer = null
        }
        for (encoder in listOfNotNull(videoEncoder, audioEncoder)) {
            try {
                encoder.stop()
            } catch (_: Throwable) {
            }
            try {
                encoder.release()
            } catch (_: Throwable) {
            }
        }
        videoEncoder = null
        audioEncoder = null
        try {
            inputSurface?.release()
        } catch (_: Throwable) {
        }
        inputSurface = null
        running.set(false)
    }
}
