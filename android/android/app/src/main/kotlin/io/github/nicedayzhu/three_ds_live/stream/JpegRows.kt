package io.github.nicedayzhu.three_ds_live.stream

/**
 * Exact JPEG row reuse (protocol R17D).
 *
 * A compact frame is an 8-byte header followed by 15 entropy-coded scan rows, each
 * terminated by its own restart marker, then EOI. A delta names a base frame, a
 * 15-bit changed-row mask, and the exact bytes of every changed row.
 */
object JpegRows {
    const val COUNT = 15
    const val DELTA_HEADER = 20
    private const val HEADER = 8

    class Parsed(val compact: ByteArray, val offsets: IntArray) {
        val width: Int get() = (compact[6].toInt() and 0xFF) or ((compact[7].toInt() and 0xFF) shl 8)
        val quality: Int get() = compact[5].toInt() and 0xFF
    }

    fun parse(compact: ByteArray): Parsed? {
        if (compact.size < 10 || compact.size > Wire.MAX_SCREEN_BYTES) return null
        if (!Wire.isCompactJpeg(compact) && !Wire.isReferenceKey(compact)) return null
        if (compact[4].toInt() != 1) return null
        val quality = compact[5].toInt() and 0xFF
        if (quality !in 1..95) return null
        val width = (compact[6].toInt() and 0xFF) or ((compact[7].toInt() and 0xFF) shl 8)
        if (width != 320 && width != 400) return null
        if (compact[compact.size - 2].toInt() and 0xFF != 0xFF ||
            compact[compact.size - 1].toInt() and 0xFF != 0xD9
        ) return null

        val offsets = IntArray(COUNT + 1)
        offsets[0] = HEADER
        var p = HEADER
        val end = compact.size - 2
        for (row in 0 until COUNT) {
            var scanned: Int? = null
            while (p < end) {
                if (compact[p].toInt() and 0xFF != 0xFF) {
                    p++
                    continue
                }
                if (p + 1 >= end) return null
                val next = compact[p + 1].toInt() and 0xFF
                if (next == 0) {
                    p += 2 // stuffed 0xFF00 inside entropy data
                    continue
                }
                val expected = 0xD0 + (row and 7)
                if (next != expected || p + 2 > end) return null
                scanned = p + 2
                break
            }
            val boundary = scanned ?: return null
            if (boundary <= offsets[row]) return null
            p = boundary
            offsets[row + 1] = boundary
        }
        if (p != end) return null
        return Parsed(compact, offsets)
    }

    fun isDelta(data: ByteArray): Boolean =
        data.size >= DELTA_HEADER && Wire.tag(data, data.size) == Wire.R17D

    /** Rebuilds the compact frame referenced by [delta]; null means "use a fresh key". */
    fun restore(reference: Parsed, delta: ByteArray): ByteArray? {
        if (!isDelta(delta)) return null
        if (delta[4].toInt() != 1) return null
        val quality = delta[5].toInt() and 0xFF
        val width = Wire.u16(delta, 6)
        if (width != 320 && width != 400) return null
        if (quality != reference.quality || width != reference.width) return null
        val rebuiltSize = Wire.u32(delta, 16).toInt()
        if (rebuiltSize !in 12..(Wire.MAX_SCREEN_BYTES - 621)) return null
        val mask = Wire.u16(delta, 12)
        if (Wire.u16(delta, 14) != 0) return null

        val out = ByteArray(rebuiltSize)
        out[0] = 'R'.code.toByte()
        out[1] = '8'.code.toByte()
        out[2] = 'J'.code.toByte()
        out[3] = '1'.code.toByte()
        out[4] = 1
        out[5] = quality.toByte()
        out[6] = (width and 0xFF).toByte()
        out[7] = (width shr 8).toByte()

        var write = HEADER
        var read = DELTA_HEADER
        for (row in 0 until COUNT) {
            if (mask and (1 shl row) != 0) {
                if (read + 4 > delta.size) return null
                val length = Wire.u32(delta, read).toInt()
                read += 4
                if (length <= 2 || read + length > delta.size) return null
                val rowEnd = read + length
                val expected = 0xD0 + (row and 7)
                if (delta[rowEnd - 2].toInt() and 0xFF != 0xFF ||
                    delta[rowEnd - 1].toInt() and 0xFF != expected
                ) return null
                if (write + length > rebuiltSize - 2) return null
                System.arraycopy(delta, read, out, write, length)
                write += length
                read += length
            } else {
                val start = reference.offsets[row]
                val end = reference.offsets[row + 1]
                val length = end - start
                if (length <= 2 || write + length > rebuiltSize - 2) return null
                System.arraycopy(reference.compact, start, out, write, length)
                write += length
            }
        }
        if (read != delta.size || write != rebuiltSize - 2) return null
        out[write] = 0xFF.toByte()
        out[write + 1] = 0xD9.toByte()
        return out
    }

    /** Dictionary bytes for R20D: the reference key after its 8-byte header. */
    fun dictionary(reference: Parsed): ByteArray {
        val bytes = reference.compact.size - HEADER
        val length = if (bytes > 32768) 32768 else bytes
        return reference.compact.copyOfRange(reference.compact.size - length, reference.compact.size)
    }
}
