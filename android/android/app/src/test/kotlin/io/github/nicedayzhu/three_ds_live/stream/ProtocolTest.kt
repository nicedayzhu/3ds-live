package io.github.nicedayzhu.three_ds_live.stream

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

class Lz4Test {
    @Test
    fun decodesLiteralsAndMatch() {
        // token: 5 literals, match length field 6 (= length 10); "hello" + copy(offset 5, 10)
        val block = byteArrayOf(
            0x56, 'h'.code.toByte(), 'e'.code.toByte(), 'l'.code.toByte(),
            'l'.code.toByte(), 'o'.code.toByte(),
            0x05, 0x00, 0x06,
        )
        val out = Lz4.decode(block, block.size, ByteArray(0), 64)
        assertNotNull(out)
        assertEquals("hellohellohello", String(out!!, Charsets.US_ASCII))
    }

    @Test
    fun decodesMatchReachingIntoDictionary() {
        val dictionary = "abc".toByteArray(Charsets.US_ASCII)
        // 1 literal 'X', match length 4 referencing offset 4 (dictionary tail).
        val block = byteArrayOf(0x14, 'X'.code.toByte(), 0x04, 0x00)
        val out = Lz4.decode(block, block.size, dictionary, 64)
        assertNotNull(out)
        assertEquals("XabcXabcX", String(out!!, Charsets.US_ASCII))
    }

    @Test
    fun rejectsInvalidOffset() {
        val block = byteArrayOf(0x14, 'X'.code.toByte(), 0x20, 0x00)
        assertNull(Lz4.decode(block, block.size, ByteArray(0), 64))
    }

    @Test
    fun rejectsOutputOverflow() {
        val block = byteArrayOf(0x56, 'h'.code.toByte(), 'e'.code.toByte(), 'l'.code.toByte(),
            'l'.code.toByte(), 'o'.code.toByte(), 0x05, 0x00, 0x06)
        assertNull(Lz4.decode(block, block.size, ByteArray(0), 7))
    }
}

class JpegHeaderTest {
    @Test
    fun q50At400KeepsThePinnedTemplate() {
        val header = JpegHeader.build(400, 50)
        assertEquals(JpegHeader.FULL_SIZE, header.size)
        assertEquals(0x10, header[25].toInt() and 0xFF)
        assertEquals(0x11, header[94].toInt() and 0xFF)
        assertEquals(0, header[162].toInt())
        assertEquals(25, header[163].toInt() and 0xFF)
        assertEquals(1, header[171].toInt() and 0xFF)
        assertEquals(144, header[172].toInt() and 0xFF) // 400
    }

    @Test
    fun widthAndQualityArePatched() {
        val narrow = JpegHeader.build(320, 50)
        assertEquals(20, narrow[163].toInt() and 0xFF)
        assertEquals(64, narrow[172].toInt() and 0xFF) // 320

        val q80 = JpegHeader.build(400, 80)
        // scale = 200 - 2*80 = 40; (0x10 * 40 + 50) / 100 = 6
        assertEquals(6, q80[25].toInt() and 0xFF)
        assertEquals(0xFF, q80[0].toInt() and 0xFF)
        assertEquals(0xD8, q80[1].toInt() and 0xFF)
    }

    @Test
    fun expandPrependsTheHeader() {
        val compact = compactJpeg(400, 70, rows = 15) { row -> byteArrayOf(0x11, 0x22, 0xFF.toByte(), (0xD0 + (row and 7)).toByte()) }
        val full = JpegHeader.expand(compact)
        assertEquals(JpegHeader.FULL_SIZE + compact.size - 8, full.size)
        assertArrayEquals(compact.copyOfRange(8, compact.size), full.copyOfRange(JpegHeader.FULL_SIZE, full.size))
    }
}

class JpegRowsTest {
    @Test
    fun parsesFifteenRowsAndRestoresDelta() {
        val compact = compactJpeg(400, 70, rows = 15) { row ->
            byteArrayOf((row * 3).toByte(), 0x01, 0xFF.toByte(), (0xD0 + (row and 7)).toByte())
        }
        val parsed = JpegRows.parse(compact)
        assertNotNull(parsed)
        assertEquals(400, parsed!!.width)
        assertEquals(70, parsed.quality)
        assertEquals(15, parsed.offsets.size - 1)
        assertEquals(compact.size - 2, parsed.offsets[15])

        // Change rows 0 and 14; everything else is copied from the reference.
        val newRow0 = byteArrayOf(0x7A, 0x02, 0xFF.toByte(), 0xD0.toByte())
        val newRow14 = byteArrayOf(0x55, 0x66, 0xFF.toByte(), 0xD6.toByte())
        val delta = deltaHeader(width = 400, quality = 70, baseSeq = 5, mask = (1 shl 0) or (1 shl 14), size = 0) +
            rowBlock(newRow0) + rowBlock(newRow14)
        val expectedSize = 8 + 13 * 4 + newRow0.size + newRow14.size + 2
        val rebuilt = JpegRows.restore(parsed, delta.copyOf().also {
            // patch the reconstructed size at offset 16
            it[16] = (expectedSize and 0xFF).toByte()
            it[17] = ((expectedSize shr 8) and 0xFF).toByte()
            it[18] = ((expectedSize shr 16) and 0xFF).toByte()
            it[19] = ((expectedSize shr 24) and 0xFF).toByte()
        })
        assertNotNull(rebuilt)
        assertEquals(expectedSize, rebuilt!!.size)
        assertArrayEquals(newRow0, rebuilt.copyOfRange(8, 8 + newRow0.size))
        assertEquals(0xFF, rebuilt[rebuilt.size - 2].toInt() and 0xFF)
        assertEquals(0xD9, rebuilt[rebuilt.size - 1].toInt() and 0xFF)
    }

    @Test
    fun rejectsWrongRestartMarker() {
        val compact = compactJpeg(320, 60, rows = 15) { row ->
            byteArrayOf((row).toByte(), 0x01, 0xFF.toByte(), (0xD0 + (row and 7)).toByte())
        }.copyOf()
        compact[10] = 0x00
        compact[11] = 0xD3.toByte() // wrong marker for row 0
        assertNull(JpegRows.parse(compact))
    }

    @Test
    fun rejectsDeltaWithMismatchedQuality() {
        val reference = JpegRows.parse(compactJpeg(400, 70, rows = 15) { row ->
            byteArrayOf((row).toByte(), 0x01, 0xFF.toByte(), (0xD0 + (row and 7)).toByte())
        })!!
        val delta = deltaHeader(width = 400, quality = 60, baseSeq = 1, mask = 1, size = 30) +
            rowBlock(byteArrayOf(1, 2, 0xFF.toByte(), 0xD0.toByte()))
        assertNull(JpegRows.restore(reference, delta))
    }
}

internal fun compactJpeg(
    width: Int,
    quality: Int,
    rows: Int,
    rowBytes: (Int) -> ByteArray,
): ByteArray {
    val out = ArrayList<Byte>()
    out.addAll(listOf('R'.code.toByte(), '8'.code.toByte(), 'J'.code.toByte(), '1'.code.toByte(), 1, quality.toByte()))
    out.add((width and 0xFF).toByte())
    out.add((width shr 8).toByte())
    for (row in 0 until rows) out.addAll(rowBytes(row).toList())
    out.add(0xFF.toByte())
    out.add(0xD9.toByte())
    return out.toByteArray()
}

internal fun deltaHeader(width: Int, quality: Int, baseSeq: Int, mask: Int, size: Int): ByteArray {
    val out = ByteArray(20)
    val tag = "R17D".toByteArray(Charsets.US_ASCII)
    System.arraycopy(tag, 0, out, 0, 4)
    out[4] = 1
    out[5] = quality.toByte()
    out[6] = (width and 0xFF).toByte()
    out[7] = (width shr 8).toByte()
    putWord(out, 8, baseSeq)
    out[12] = (mask and 0xFF).toByte()
    out[13] = ((mask shr 8) and 0xFF).toByte()
    putWord(out, 16, size)
    return out
}

internal fun rowBlock(row: ByteArray): ByteArray {
    val out = ByteArray(4 + row.size)
    putWord(out, 0, row.size)
    System.arraycopy(row, 0, out, 4, row.size)
    return out
}

private fun putWord(target: ByteArray, offset: Int, value: Int) {
    target[offset] = (value and 0xFF).toByte()
    target[offset + 1] = ((value shr 8) and 0xFF).toByte()
    target[offset + 2] = ((value shr 16) and 0xFF).toByte()
    target[offset + 3] = ((value shr 24) and 0xFF).toByte()
}
