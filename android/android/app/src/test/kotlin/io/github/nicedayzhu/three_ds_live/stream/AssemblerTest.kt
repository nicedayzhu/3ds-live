package io.github.nicedayzhu.three_ds_live.stream

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class AssemblerTest {
    private var completed: Assembler.Pair? = null
    private var incomplete = 0
    private var repairs = 0

    private fun assembler(): Assembler = Assembler(
        onComplete = { completed = it },
        onIncomplete = { _, _ -> incomplete++ },
        onNeedRepair = { _, _, _ -> repairs++ },
    )

    @Test
    fun completesASingleChunkPair() {
        val assembler = assembler()
        val top = ByteArray(40) { (it + 1).toByte() }
        val bottom = ByteArray(30) { (it + 100).toByte() }
        val joined = top + bottom
        assertTrue(
            assembler.submit(
                dataPacket(Wire.R10P, seq = 7, pts = 1000, topLength = top.size,
                    bottomLength = bottom.size, index = 0, fps = 30, payload = joined),
                headerLength(joined.size) + joined.size,
                1_000_000,
            ),
        )
        val pair = completed
        assertNotNull(pair)
        assertEquals(7, pair!!.seq)
        assertArrayEquals(top, pair.top)
        assertArrayEquals(bottom, pair.bottom)
    }

    @Test
    fun recoversTheMissingChunkFromParity() {
        val assembler = assembler()
        val top = ByteArray(1200) { (it % 251).toByte() }
        val bottom = ByteArray(90) { (it * 3 % 251).toByte() }
        val joined = top + bottom
        val chunk0 = joined.copyOfRange(0, Wire.CHUNK)
        val chunk1 = joined.copyOfRange(Wire.CHUNK, joined.size)

        assembler.submit(
            dataPacket(Wire.R10P, seq = 9, pts = 5, topLength = top.size,
                bottomLength = bottom.size, index = 0, fps = 30, payload = chunk0),
            headerLength(chunk0.size) + chunk0.size,
            1_000_000,
        )
        // Chunk 1 lives in stripe 1; its parity packet is the padded chunk itself.
        val parity = chunk1.copyOf(Wire.CHUNK)
        assembler.submit(
            parityPacket(seq = 9, pts = 5, topLength = top.size, bottomLength = bottom.size,
                stripe = 1, fps = 30, payload = parity),
            headerLength(Wire.CHUNK) + Wire.CHUNK,
            1_100_000,
        )

        val pair = completed
        assertNotNull(pair)
        assertEquals(9, pair!!.seq)
        assertArrayEquals(top, pair.top)
        assertArrayEquals(bottom, pair.bottom)
        assertTrue(assembler.recoveredChunks >= 1)
    }

    @Test
    fun ignoresDuplicateChunks() {
        val assembler = assembler()
        val top = ByteArray(30) { 1 }
        val bottom = ByteArray(30) { 2 }
        val joined = top + bottom
        val packet = dataPacket(Wire.R10P, 3, 0, top.size, bottom.size, 0, 30, joined)
        val length = headerLength(joined.size) + joined.size
        assembler.submit(packet, length, 1_000_000)
        assembler.submit(packet, length, 1_000_000)
        assertEquals(3, completed!!.seq)
        assertEquals(1L, assembler.pairsCompleted)
    }

    @Test
    fun rejectsLengthBeyondProtocolLimit() {
        val assembler = assembler()
        val packet = dataPacket(Wire.R10P, 1, 0, 200000, 30, 0, 30, ByteArray(8))
        assertTrue(!assembler.submit(packet, 32 + 8, 1_000_000))
    }

    @Test
    fun dropsAStalledPartialAfterOneSecond() {
        val assembler = assembler()
        val top = ByteArray(1200) { 7 }
        val bottom = ByteArray(30) { 8 }
        val joined = top + bottom
        val chunk0 = joined.copyOfRange(0, Wire.CHUNK)
        assembler.submit(
            dataPacket(Wire.R10P, 11, 0, top.size, bottom.size, 0, 30, chunk0),
            headerLength(chunk0.size) + chunk0.size,
            1_000_000,
        )
        assembler.poll(2_100_000_000, 16, 100_000_000)
        assertEquals(1, incomplete)
    }

    private fun headerLength(payload: Int) = Wire.HEADER_R10

    private fun dataPacket(
        tag: String,
        seq: Int,
        pts: Long,
        topLength: Int,
        bottomLength: Int,
        index: Int,
        fps: Int,
        payload: ByteArray,
    ): ByteArray {
        val buffer = ByteBuffer.allocate(Wire.HEADER_R10 + payload.size).order(ByteOrder.LITTLE_ENDIAN)
        buffer.put(tag.toByteArray(Charsets.US_ASCII))
        buffer.putInt(42) // session
        buffer.putInt(seq)
        buffer.putInt(pts.toInt())
        buffer.putInt(topLength)
        buffer.putInt(bottomLength)
        buffer.putInt(index)
        buffer.putInt(fps)
        buffer.put(payload)
        return buffer.array()
    }

    private fun parityPacket(
        seq: Int,
        pts: Long,
        topLength: Int,
        bottomLength: Int,
        stripe: Int,
        fps: Int,
        payload: ByteArray,
    ): ByteArray {
        val buffer = ByteBuffer.allocate(Wire.HEADER_R10 + payload.size).order(ByteOrder.LITTLE_ENDIAN)
        buffer.put(Wire.R10F.toByteArray(Charsets.US_ASCII))
        buffer.putInt(42)
        buffer.putInt(seq)
        buffer.putInt(pts.toInt())
        buffer.putInt(topLength)
        buffer.putInt(bottomLength)
        buffer.putInt(stripe)
        buffer.putInt(fps)
        buffer.put(payload)
        return buffer.array()
    }
}

class WireTest {
    @Test
    fun messageIsLittleEndianAndTagPrefixed() {
        val bytes = Wire.message(Wire.R3FB, 1, 2, 3, 4, 5, 6)
        assertEquals(28, bytes.size)
        assertEquals("R3FB", Wire.tag(bytes, bytes.size))
        assertEquals(1, Wire.int(bytes, 4))
        assertEquals(6, Wire.int(bytes, 24))
    }

    @Test
    fun compactJpegFieldsAreDecoded() {
        val compact = compactJpeg(width = 320, quality = 65, rows = 15) { row ->
            byteArrayOf(row.toByte(), 0x00, 0xFF.toByte(), (0xD0 + (row and 7)).toByte())
        }
        assertTrue(Wire.isCompactJpeg(compact))
        assertEquals(320, Wire.compactWidth(compact))
        assertEquals(65, Wire.compactQuality(compact))
    }
}
