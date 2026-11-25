package io.github.nicedayzhu.three_ds_live.stream

import android.content.ContentValues
import android.content.Context
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.util.Log
import io.github.nicedayzhu.three_ds_live.BuildConfig
import java.io.File
import java.text.SimpleDateFormat
import java.util.ArrayDeque
import java.util.Date
import java.util.Locale

/**
 * Small in-memory log ring that can be exported to Downloads so a device test can be
 * diagnosed without adb. Android's logcat is still written as usual.
 */
object Logs {
    private const val MAX_LINES = 4000
    private val ring = ArrayDeque<String>(MAX_LINES)
    private val clock = SimpleDateFormat("HH:mm:ss.SSS", Locale.US)

    @Synchronized
    private fun add(level: Char, tag: String, message: String, error: Throwable?) {
        when (level) {
            'E' -> Log.e(tag, message, error)
            'W' -> Log.w(tag, message, error)
            'D' -> Log.d(tag, message)
            else -> Log.i(tag, message)
        }
        val line = buildString {
            append(clock.format(Date())).append(' ').append(level).append('/').append(tag)
                .append(": ").append(message)
            if (error != null) {
                append(" | ").append(error.javaClass.simpleName).append(": ").append(error.message)
            }
        }
        if (ring.size >= MAX_LINES) ring.removeFirst()
        ring.addLast(line)
    }

    fun i(tag: String, message: String) = add('I', tag, message, null)
    fun w(tag: String, message: String, error: Throwable? = null) = add('W', tag, message, error)
    fun e(tag: String, message: String, error: Throwable? = null) = add('E', tag, message, error)
    fun d(tag: String, message: String) = add('D', tag, message, null)

    @Synchronized
    fun snapshot(): String = buildString {
        appendLine("3DS Live ${BuildConfig.VERSION_NAME} (${BuildConfig.VERSION_CODE})")
        appendLine("device=${Build.MANUFACTURER} ${Build.MODEL}, android=${Build.VERSION.SDK_INT}")
        appendLine("lines=${ring.size}")
        appendLine("----")
        for (line in ring) appendLine(line)
    }

    /** Writes the ring to Downloads/3DS Live and returns a user-visible path. */
    fun export(context: Context): String {
        val name = "3ds-live-log-" +
            SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date()) + ".txt"
        val text = snapshot()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, name)
                put(MediaStore.Downloads.MIME_TYPE, "text/plain")
                put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/3DS Live")
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
            val uri = context.contentResolver.insert(
                MediaStore.Downloads.EXTERNAL_CONTENT_URI,
                values,
            ) ?: throw IllegalStateException("无法创建下载文件")
            context.contentResolver.openOutputStream(uri)?.use { it.write(text.toByteArray()) }
                ?: throw IllegalStateException("无法写入下载文件")
            val done = ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }
            context.contentResolver.update(uri, done, null, null)
            return "下载/3DS Live/$name"
        }
        val dir = File(
            context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS) ?: context.filesDir,
            "logs",
        ).apply { mkdirs() }
        val file = File(dir, name)
        file.writeText(text)
        return file.absolutePath
    }
}
