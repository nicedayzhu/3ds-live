package io.github.nicedayzhu.three_ds_live.stream

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ScreenResolverTest {
    private val topRows = rows(400, 70)
    private val bottomRows = rows(320, 60)

    @Test
    fun adoptsAKeyPairAndRestoresTopDelta() {
        val resolver = ScreenResolver()
        val keyTop = keyJpeg(400, 70, topRows)
        val keyBottom = keyJpeg(320, 60, bottomRows)
        assertNotNull("top parse", JpegRows.parse(keyTop))
        assertNotNull("bottom parse", JpegRows.parse(keyBottom))
        assertNotNull("top valid", if (Wire.validScreen(keyTop)) keyTop else null)
        val top = resolver.resolve(keyTop, 0)
        val bottom = resolver.resolve(keyBottom, 1)
        assertNotNull(top)
        assertTrue(top!!.isKey)
        assertTrue(bottom!!.isKey)
        assertTrue(resolver.completePair(5, top, bottom))
        assertEquals(5, resolver.referenceSeq)

        val changed = byteArrayOf(0x11, 0x22, 0x33, 0xFF.toByte(), 0xD2.toByte())
        val expected = compact(width = 400, quality = 70, rows = topRows, replacements = mapOf(2 to changed))
        val delta = delta(400, 70, baseSeq = 5, changes = mapOf(2 to changed), size = expected.size)
        val resolved = resolver.resolve(delta, 0)
        assertNotNull(resolved)
        assertArrayEquals(JpegHeader.expand(expected), resolved!!.jpeg)
        assertEquals(70, resolved.quality)
    }

    @Test
    fun restoresBottomDeltaAndReportsQuality() {
        val resolver = ScreenResolver()
        resolver.completePair(
            1,
            resolver.resolve(keyJpeg(400, 70, topRows), 0),
            resolver.resolve(keyJpeg(320, 60, bottomRows), 1),
        )
        val changed = byteArrayOf(0x44, 0x55, 0xFF.toByte(), 0xD5.toByte())
        val expected = compact(320, 60, bottomRows, mapOf(13 to changed))
        val delta = delta(320, 60, baseSeq = 1, changes = mapOf(13 to changed), size = expected.size)
        val resolved = resolver.resolve(delta, 1)
        assertNotNull(resolved)
        assertArrayEquals(JpegHeader.expand(expected), resolved!!.jpeg)
        assertEquals(60, resolved.quality)
    }

    @Test
    fun asksForAKeyWhenADeltaHasNoReference() {
        val resolver = ScreenResolver()
        val changed = byteArrayOf(0x01, 0x02, 0xFF.toByte(), 0xD0.toByte())
        val delta = delta(400, 70, baseSeq = 9, changes = mapOf(0 to changed), size = 60)
        assertNull(resolver.resolve(delta, 0))
        assertTrue(resolver.requestFreshKey)
        assertTrue(resolver.consumeFreshKeyRequest())
        assertFalse(resolver.requestFreshKey)
    }

    @Test
    fun restoresCompressedLowerDelta() {
        val resolver = ScreenResolver()
        resolver.setDictionarySupported(true)
        resolver.completePair(
            4,
            resolver.resolve(keyJpeg(400, 70, topRows), 0),
            resolver.resolve(keyJpeg(320, 60, bottomRows), 1),
        )
        val changed = byteArrayOf(0x77, 0x66, 0x55, 0xFF.toByte(), 0xD4.toByte())
        val innerCompact = compact(320, 60, bottomRows, mapOf(4 to changed))
        val inner = delta(320, 60, baseSeq = 4, changes = mapOf(4 to changed), size = innerCompact.size)

        val reference = JpegRows.parse(keyJpeg(320, 60, bottomRows))!!
        val dictionary = JpegRows.dictionary(reference)
        val block = literalOnlyBlock(inner)
        val outer = ByteArray(20 + block.size)
        System.arraycopy("R20D".toByteArray(Charsets.US_ASCII), 0, outer, 0, 4)
        outer[4] = 1
        outer[5] = 60
        outer[6] = 0x40 // 320
        outer[7] = 0x01
        put(outer, 8, 4)
        put(outer, 12, inner.size)
        put(outer, 16, 0)
        System.arraycopy(block, 0, outer, 20, block.size)

        val resolved = resolver.resolve(outer, 1)
        assertNotNull(resolved)
        assertArrayEquals(JpegHeader.expand(innerCompact), resolved!!.jpeg)
        assertEquals(60, resolved.quality)
    }

    @Test
    fun rejectsCompressedDeltaWithoutDictionarySupport() {
        val resolver = ScreenResolver()
        resolver.completePair(
            2,
            resolver.resolve(keyJpeg(400, 70, topRows), 0),
            resolver.resolve(keyJpeg(320, 60, bottomRows), 1),
        )
        val changed = byteArrayOf(0x01, 0x02, 0xFF.toByte(), 0xD0.toByte())
        val inner = delta(320, 60, baseSeq = 2, changes = mapOf(0 to changed), size = 40)
        val block = literalOnlyBlock(inner)
        val outer = ByteArray(20 + block.size)
        System.arraycopy("R20D".toByteArray(Charsets.US_ASCII), 0, outer, 0, 4)
        outer[4] = 1
        outer[5] = 60
        outer[6] = 0x40
        outer[7] = 0x01
        put(outer, 8, 2)
        put(outer, 12, inner.size)
        System.arraycopy(block, 0, outer, 20, block.size)
        assertNull(resolver.resolve(outer, 1))
    }

    // ------------------------------------------------------------ fixtures

    private fun rows(width: Int, quality: Int): List<ByteArray> =
        (0 until JpegRows.COUNT).map { row ->
            ByteArray(6 + row) { index ->
                // JPEG entropy data never contains a bare 0xFF; keep the fixture realistic.
                ((row * 31 + index * 7 + quality) % 0x7F + 1).toByte()
            } + byteArrayOf(0xFF.toByte(), (0xD0 + (row and 7)).toByte())
        }

    private fun keyJpeg(width: Int, quality: Int, rows: List<ByteArray>): ByteArray =
        compact(width, quality, rows).also {
            it[0] = 'R'.code.toByte()
            it[1] = '1'.code.toByte()
            it[2] = '7'.code.toByte()
            it[3] = 'K'.code.toByte()
        }

    private fun compact(
        width: Int,
        quality: Int,
        rows: List<ByteArray>,
        replacements: Map<Int, ByteArray> = emptyMap(),
    ): ByteArray {
        val out = ArrayList<Byte>(256)
        out.addAll(listOf('R'.code.toByte(), '8'.code.toByte(), 'J'.code.toByte(), '1'.code.toByte()))
        out.add(1)
        out.add(quality.toByte())
        out.add((width and 0xFF).toByte())
        out.add((width shr 8).toByte())
        for (row in rows.indices) {
            val data = replacements[row] ?: rows[row]
            out.addAll(data.toList())
        }
        out.add(0xFF.toByte())
        out.add(0xD9.toByte())
        return out.toByteArray()
    }

    private fun delta(
        width: Int,
        quality: Int,
        baseSeq: Int,
        changes: Map<Int, ByteArray>,
        size: Int,
    ): ByteArray {
        val out = ArrayList<Byte>(128)
        out.addAll("R17D".toByteArray(Charsets.US_ASCII).toList())
        out.add(1)
        out.add(quality.toByte())
        out.add((width and 0xFF).toByte())
        out.add((width shr 8).toByte())
        val base = ByteArray(4)
        put(base, 0, baseSeq)
        out.addAll(base.toList())
        var mask = 0
        for (row in changes.keys) mask = mask or (1 shl row)
        out.add((mask and 0xFF).toByte())
        out.add(((mask shr 8) and 0xFF).toByte())
        out.add(0)
        out.add(0)
        val sizeBytes = ByteArray(4)
        put(sizeBytes, 0, size)
        out.addAll(sizeBytes.toList())
        for (row in changes.keys.sorted()) {
            val data = changes.getValue(row)
            val length = ByteArray(4)
            put(length, 0, data.size)
            out.addAll(length.toList())
            out.addAll(data.toList())
        }
        return out.toByteArray()
    }

    private fun literalOnlyBlock(data: ByteArray): ByteArray {
        val out = ArrayList<Byte>(data.size + 8)
        if (data.size < 15) {
            out.add(((data.size shl 4) and 0xF0).toByte())
        } else {
            out.add(0xF0.toByte())
            var rest = data.size - 15
            while (rest >= 255) {
                out.add(255.toByte())
                rest -= 255
            }
            out.add(rest.toByte())
        }
        out.addAll(data.toList())
        return out.toByteArray()
    }

    private fun put(target: ByteArray, offset: Int, value: Int) {
        target[offset] = (value and 0xFF).toByte()
        target[offset + 1] = ((value shr 8) and 0xFF).toByte()
        target[offset + 2] = ((value shr 16) and 0xFF).toByte()
        target[offset + 3] = ((value shr 24) and 0xFF).toByte()
    }
}

class Lz4DictionaryFixtureTest {
    /** Compressed by liblz4 with a 400-byte dictionary; verifies our decoder matches it. */
    private val block = "0f3e002b0f720021023400fc0f7172737475767778797a6162636465666768696a6b6c6d6e6f7071727374a6000234000fda001b0f3400ff0f504e55424950"

    private fun expected(): ByteArray {
        val dict = ByteArray(400) { ('A' + (it * 7) % 26).code.toByte() }
        val src = ByteArray(512)
        for (i in 0 until 512) src[i] = ('A' + (i * 7) % 26).code.toByte()
        for (i in 200 until 230) src[i] = ('0' + (i % 10)).code.toByte()
        System.arraycopy(dict, 0, src, 0, 400)
        for (i in 120 until 150) src[i] = ('a' + (i % 26)).code.toByte()
        return src
    }

    @Test
    fun decodesWhatLiblz4Produced() {
        val bytes = block.chunked(2).map { it.toInt(16).toByte() }.toByteArray()
        val out = Lz4.decode(bytes, bytes.size, ByteArray(400) { ('A' + (it * 7) % 26).code.toByte() }, 512)
        assertNotNull(out)
        assertArrayEquals(expected(), out!!)
    }
}
