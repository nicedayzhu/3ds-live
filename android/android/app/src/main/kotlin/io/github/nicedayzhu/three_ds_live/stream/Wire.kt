package io.github.nicedayzhu.three_ds_live.stream

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Wire helpers for the 3DS Live protocol.
 *
 * All integers are little-endian (live/docs/PROTOCOL.md). Lengths are checked by
 * callers; these helpers only decode already-validated datagrams.
 */
object Wire {
    const val CHUNK = 1168
    const val MAX_SCREEN_BYTES = 128000
    const val STRIPES = 4
    const val ROWS = 15
    const val HEADER_R10 = 32
    const val HEADER_R9A1 = 24
    const val RECORD_R9A1 = 175

    const val DISCOVERY_PROBE = "3DLP"
    const val DISCOVERY_PROBE_REPLY = "3DLS"
    const val DISCOVERY_INVITE_REPLY = "3DLA"

    const val R10P = "R10P"
    const val R10F = "R10F"
    const val R3P2 = "R3P2"
    const val R3F2 = "R3F2"
    const val R17S = "R17S"
    const val R17A = "R17A"
    const val R17K = "R17K"
    const val R17D = "R17D"
    const val R19S = "R19S"
    const val R19A = "R19A"
    const val R19N = "R19N"
    const val R19P = "R19P"
    const val R20S = "R20S"
    const val R20A = "R20A"
    const val R20D = "R20D"
    const val R9A1 = "R9A1"
    const val R7ST = "R7ST"
    const val R9ST = "R9ST"
    const val R16S = "R16S"
    const val R16Q = "R16Q"
    const val R17T = "R17T"
    const val R19T = "R19T"
    const val R21S = "R21S"
    const val R21T = "R21T"
    const val R6CF = "R6CF"
    const val R10C = "R10C"
    const val R15C = "R15C"
    const val R16C = "R16C"
    const val R23C = "R23C"
    const val R3FB = "R3FB"

    fun tag(data: ByteArray, length: Int): String =
        if (length < 4) "" else String(data, 0, 4, Charsets.US_ASCII)

    fun u32(data: ByteArray, offset: Int): Long =
        (data[offset].toLong() and 0xFF) or
            ((data[offset + 1].toLong() and 0xFF) shl 8) or
            ((data[offset + 2].toLong() and 0xFF) shl 16) or
            ((data[offset + 3].toLong() and 0xFF) shl 24)

    fun u16(data: ByteArray, offset: Int): Int =
        (data[offset].toInt() and 0xFF) or ((data[offset + 1].toInt() and 0xFF) shl 8)

    fun int(data: ByteArray, offset: Int): Int = u32(data, offset).toInt()

    fun putU32(buffer: ByteBuffer, value: Int) {
        buffer.order(ByteOrder.LITTLE_ENDIAN).putInt(value)
    }

    fun message(tag: String, vararg words: Int): ByteArray {
        val buffer = ByteBuffer.allocate(4 + words.size * 4).order(ByteOrder.LITTLE_ENDIAN)
        buffer.put(tag.toByteArray(Charsets.US_ASCII))
        for (word in words) buffer.putInt(word)
        return buffer.array()
    }

    /** The four-screen layout constants used when composing decoded frames. */
    const val TOP_WIDTH = 400
    const val TOP_HEIGHT = 240
    const val BOTTOM_WIDTH = 320
    const val BOTTOM_HEIGHT = 240

    /** Compact JPEG header emitted by the handheld (629-byte JPEGENC header omitted). */
    fun isCompactJpeg(data: ByteArray): Boolean =
        data.size >= 8 && data[0] == 'R'.code.toByte() && data[1] == '8'.code.toByte() &&
            data[2] == 'J'.code.toByte() && data[3] == '1'.code.toByte()

    fun compactWidth(data: ByteArray): Int = u16(data, 6)

    fun compactQuality(data: ByteArray): Int = data[5].toInt() and 0xFF

    fun isReferenceKey(data: ByteArray): Boolean =
        data.size >= 8 && data[0] == 'R'.code.toByte() && data[1] == '1'.code.toByte() &&
            data[2] == '7'.code.toByte() && data[3] == 'K'.code.toByte()

    fun isFullJpeg(data: ByteArray): Boolean =
        data.size > 4 && (data[0].toInt() and 0xFF) == 0xFF && (data[1].toInt() and 0xFF) == 0xD8

    fun isDelta(data: ByteArray): Boolean =
        data.size >= 20 && tag(data, data.size) == R17D

    fun isCompressedDelta(data: ByteArray): Boolean =
        data.size >= 20 && tag(data, data.size) == R20D

    /** Keep memory bounded: a JPEG larger than the protocol limit is never valid. */
    fun validScreen(data: ByteArray?): Boolean =
        data != null && data.size in 12..MAX_SCREEN_BYTES
}
