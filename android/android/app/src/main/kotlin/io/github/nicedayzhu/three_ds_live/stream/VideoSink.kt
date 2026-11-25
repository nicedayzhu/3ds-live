package io.github.nicedayzhu.three_ds_live.stream

import android.content.ContentValues
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.view.Surface
import io.flutter.view.TextureRegistry
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Decodes JPEG screens and composes them into a Flutter texture.
 *
 * One SurfaceProducer carries the composed canvas, so a frame crosses into Flutter
 * once as a GPU sample instead of 1.5 MB of RGBA through a platform channel.
 */
class VideoSink(
    private val context: Context,
    registry: TextureRegistry,
    private val onStats: (quality: Int, width: Int, height: Int) -> Unit,
) {
    enum class Layout(val width: Int, val height: Int) {
        STACKED(800, 960),
        SIDE_BY_SIDE(1440, 480),
        TOP_ONLY(800, 480),
        BOTTOM_ONLY(640, 480);

        companion object {
            fun from(name: String): Layout = when (name) {
                "sideBySide" -> SIDE_BY_SIDE
                "topOnly" -> TOP_ONLY
                "bottomOnly" -> BOTTOM_ONLY
                else -> STACKED
            }
        }
    }

    private val producer: TextureRegistry.SurfaceProducer = registry.createSurfaceProducer()
    private var surface: Surface? = null
    private var recordSurface: Surface? = null
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.DITHER_FLAG)
    private val options = BitmapFactory.Options().apply {
        inPreferredConfig = Bitmap.Config.RGB_565
        inMutable = false
    }

    var layout: Layout = Layout.STACKED
        private set
    private var lastTop: Bitmap? = null
    private var lastBottom: Bitmap? = null
    private var lastTopQuality = 0
    private var lastBottomQuality = 0
    private var frames = 0L
    private var lastPresentNanos = 0L

    val textureId: Long get() = producer.id()

    init {
        surface = producer.getSurface()
        producer.setSize(layout.width, layout.height)
    }

    fun setLayout(next: Layout) {
        if (layout == next) return
        layout = next
        producer.setSize(next.width, next.height)
        surface = producer.getSurface()
    }

    fun decode(jpeg: ByteArray): Bitmap? {
        return try {
            BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size, options)
        } catch (_: Throwable) {
            null
        }
    }

    /** Publishes one pair; a missing screen reuses the previous one (keep-last-frame). */
    fun present(top: Bitmap?, bottom: Bitmap?, topQuality: Int, bottomQuality: Int) {
        if (top != null) {
            lastTop = top
            lastTopQuality = topQuality
        }
        if (bottom != null) {
            lastBottom = bottom
            lastBottomQuality = bottomQuality
        }
        val target = surface ?: return
        val canvas: Canvas = try {
            target.lockCanvas(null)
        } catch (_: Throwable) {
            return
        }
        try {
            compose(canvas)
        } finally {
            try {
                target.unlockCanvasAndPost(canvas)
            } catch (_: Throwable) {
                // Surface may be gone while the activity is being destroyed.
            }
        }
        recordFrame()
        producer.scheduleFrame()
        frames++
        lastPresentNanos = System.nanoTime()
        onStats(
            if (lastTopQuality > 0) lastTopQuality else lastBottomQuality,
            lastTop?.width ?: 0,
            lastTop?.height ?: 0,
        )
    }

    fun setRecordSurface(surface: Surface?) {
        recordSurface = surface
    }

    private fun compose(canvas: Canvas) {
        canvas.drawColor(Color.BLACK)
        when (layout) {
            Layout.STACKED -> {
                draw(canvas, lastTop, 0, 0, layout.width, layout.height / 2 - 4)
                draw(canvas, lastBottom, 0, layout.height / 2 + 4, layout.width, layout.height / 2 - 4)
            }
            Layout.SIDE_BY_SIDE -> {
                draw(canvas, lastTop, 0, 0, layout.width / 2 - 4, layout.height)
                draw(canvas, lastBottom, layout.width / 2 + 4, 0, layout.width / 2 - 4, layout.height)
            }
            Layout.TOP_ONLY -> draw(canvas, lastTop, 0, 0, layout.width, layout.height)
            Layout.BOTTOM_ONLY -> draw(canvas, lastBottom, 0, 0, layout.width, layout.height)
        }
    }

    private fun recordFrame() {
        val target = recordSurface ?: return
        val canvas = try {
            target.lockCanvas(null)
        } catch (_: Throwable) {
            return
        }
        try {
            compose(canvas)
        } finally {
            try {
                target.unlockCanvasAndPost(canvas)
            } catch (_: Throwable) {
            }
        }
    }

    private fun draw(canvas: Canvas, bitmap: Bitmap?, x: Int, y: Int, width: Int, height: Int) {
        val source = bitmap ?: return
        val dst = Rect(x, y, x + width, y + height)
        canvas.drawBitmap(source, null, dst, paint)
    }

    fun clear() {
        lastTop?.recycle()
        lastBottom?.recycle()
        lastTop = null
        lastBottom = null
        frames = 0
    }

    fun release() {
        clear()
        producer.release()
        surface?.release()
        surface = null
    }

    /** Saves the currently composed frame as a PNG through MediaStore when possible. */
    fun capture(): String? {
        val bitmap = Bitmap.createBitmap(layout.width, layout.height, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        canvas.drawColor(Color.BLACK)
        when (layout) {
            Layout.STACKED -> {
                draw(canvas, lastTop, 0, 0, layout.width, layout.height / 2 - 4)
                draw(canvas, lastBottom, 0, layout.height / 2 + 4, layout.width, layout.height / 2 - 4)
            }
            Layout.SIDE_BY_SIDE -> {
                draw(canvas, lastTop, 0, 0, layout.width / 2 - 4, layout.height)
                draw(canvas, lastBottom, layout.width / 2 + 4, 0, layout.width / 2 - 4, layout.height)
            }
            Layout.TOP_ONLY -> draw(canvas, lastTop, 0, 0, layout.width, layout.height)
            Layout.BOTTOM_ONLY -> draw(canvas, lastBottom, 0, 0, layout.width, layout.height)
        }
        val name = "3DS-Live-${SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())}.png"
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val values = ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, name)
                    put(MediaStore.Images.Media.MIME_TYPE, "image/png")
                    put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/3DS Live")
                    put(MediaStore.Images.Media.IS_PENDING, 1)
                }
                val uri = context.contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                    ?: return null
                context.contentResolver.openOutputStream(uri)?.use { stream ->
                    bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream)
                }
                values.clear()
                values.put(MediaStore.Images.Media.IS_PENDING, 0)
                context.contentResolver.update(uri, values, null, null)
                uri.toString()
            } else {
                val dir = File(
                    Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES),
                    "3DS Live",
                ).apply { mkdirs() }
                val file = File(dir, name)
                FileOutputStream(file).use { stream ->
                    bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream)
                }
                file.absolutePath
            }
        } catch (_: Throwable) {
            null
        } finally {
            bitmap.recycle()
        }
    }
}
