package io.github.nicedayzhu.three_ds_live.stream

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class PerScreenAssemblyTest {
    private var completed: Assembler.Pair? = null

    private fun assembler() = Assembler(
        onComplete = { completed = it },
        onIncomplete = { _, _ -> },
        onNeedRepair = { _, _, _ -> },
    )

    private fun packet(
        tag: String,
        seq: Int,
        pts: Int,
        screen: Int,
        screenLength: Int,
        offset: Int,
        fps: Int,
        payload: ByteArray,
    ): ByteArray {
        val buffer = ByteBuffer.allocate(Wire.HEADER_R10 + payload.size).order(ByteOrder.LITTLE_ENDIAN)
        buffer.put(tag.toByteArray(Charsets.US_ASCII))
        buffer.putInt(42)
        buffer.putInt(seq)
        buffer.putInt(pts)
        buffer.putInt(screen)
        buffer.putInt(screenLength)
        buffer.putInt(offset)
        buffer.putInt(fps)
        buffer.put(payload)
        return buffer.array()
    }

    @Test
    fun assemblesPerScreenPairs() {
        val assembler = assembler()
        val top = ByteArray(1300) { (it % 251).toByte() }
        val bottom = ByteArray(90) { (it + 5).toByte() }
        val chunk0 = top.copyOfRange(0, Wire.CHUNK)
        val chunk1 = top.copyOfRange(Wire.CHUNK, top.size)
        assembler.submitPerScreen(packet(Wire.R3P2, 21, 500, 0, top.size, 0, 30, chunk0), 32 + chunk0.size, 1_000_000)
        assembler.submitPerScreen(packet(Wire.R3P2, 21, 500, 0, top.size, Wire.CHUNK, 30, chunk1), 32 + chunk1.size, 1_000_000)
        assertNull(completed)
        assembler.submitPerScreen(packet(Wire.R3P2, 21, 500, 1, bottom.size, 0, 30, bottom), 32 + bottom.size, 1_000_000)

        val pair = completed
        assertNotNull(pair)
        assertEquals(21, pair!!.seq)
        assertArrayEquals(top, pair.top)
        assertArrayEquals(bottom, pair.bottom)
    }

    @Test
    fun recoversAPerScreenHoleFromParity() {
        val assembler = assembler()
        val top = ByteArray(1400) { (it % 200).toByte() }
        val bottom = ByteArray(50) { 9 }
        val chunk0 = top.copyOfRange(0, Wire.CHUNK)
        val chunk1 = top.copyOfRange(Wire.CHUNK, top.size)
        assembler.submitPerScreen(packet(Wire.R3P2, 22, 0, 0, top.size, 0, 30, chunk0), 32 + chunk0.size, 1_000_000)
        assembler.submitPerScreen(packet(Wire.R3F2, 22, 0, 0, top.size, 1, 30, chunk1.copyOf(Wire.CHUNK)), 32 + Wire.CHUNK, 1_000_000)
        assembler.submitPerScreen(packet(Wire.R3P2, 22, 0, 1, bottom.size, 0, 30, bottom), 32 + bottom.size, 1_100_000)

        val pair = completed
        assertNotNull(pair)
        assertArrayEquals(top, pair!!.top)
        assertArrayEquals(bottom, pair.bottom)
        assertTrue(assembler.recoveredChunks >= 1)
    }

    @Test
    fun rejectsMisalignedOffsets() {
        val assembler = assembler()
        val data = ByteArray(64) { 1 }
        assertTrue(
            !assembler.submitPerScreen(
                packet(Wire.R3P2, 1, 0, 0, 100, 10, 30, data),
                32 + data.size,
                1_000_000,
            ),
        )
    }
}

class RateControllerTest {
    @Test
    fun backsOffAfterThreeWeakSeconds() {
        val controller = RateController()
        controller.reset(2_000_000)
        assertNull(controller.observe(10.0, 30, 1.0))
        assertNull(controller.observe(10.0, 30, 1.0))
        assertEquals(1_600_000, controller.observe(10.0, 30, 1.0))
        assertEquals(1_600_000, controller.requested)
    }

    @Test
    fun probesUpOnlyAfterEightHealthySeconds() {
        val controller = RateController()
        controller.reset(2_000_000)
        repeat(3) { controller.observe(10.0, 30, 1.0) }
        repeat(7) { assertNull(controller.observe(30.0, 30, 1.0)) }
        assertEquals(2_000_000, controller.observe(30.0, 30, 1.0))
    }

    @Test
    fun neverDropsBelowTheFloor() {
        val controller = RateController()
        controller.reset(500_000)
        repeat(40) {
            controller.observe(1.0, 30, 1.0)
        }
        assertEquals(400_000, controller.requested)
    }

    @Test
    fun ceilingChangeClampsRequestedRate() {
        val controller = RateController()
        controller.reset(2_000_000)
        controller.setCeiling(800_000)
        assertEquals(800_000, controller.requested)
    }
}

/**
 * Feeds a synthetic session through the exact receive path: R10 packets -> assembler
 * (with XOR) -> resolver (row reference, delta) and compares the decodable JPEG bytes.
 */
class SessionPipelineTest {
    private var pair: Assembler.Pair? = null
    private val resolver = ScreenResolver()

    private fun assembler() = Assembler(
        onComplete = { pair = it },
        onIncomplete = { _, _ -> },
        onNeedRepair = { _, _, _ -> },
    )

    private fun rows(width: Int, seed: Int): List<ByteArray> =
        (0 until JpegRows.COUNT).map { row ->
            ByteArray(8 + row) { index -> (((seed + row * 13 + index * 5) % 0x70) + 1).toByte() } +
                byteArrayOf(0xFF.toByte(), (0xD0 + (row and 7)).toByte())
        }

    private fun compact(width: Int, quality: Int, data: List<ByteArray>): ByteArray {
        val out = ArrayList<Byte>()
        out.addAll(listOf('R'.code.toByte(), '8'.code.toByte(), 'J'.code.toByte(), '1'.code.toByte(), 1, quality.toByte()))
        out.add((width and 0xFF).toByte())
        out.add((width shr 8).toByte())
        for (row in data) out.addAll(row.toList())
        out.add(0xFF.toByte())
        out.add(0xD9.toByte())
        return out.toByteArray()
    }

    private fun key(width: Int, quality: Int, data: List<ByteArray>): ByteArray =
        compact(width, quality, data).also {
            it[0] = 'R'.code.toByte()
            it[1] = '1'.code.toByte()
            it[2] = '7'.code.toByte()
            it[3] = 'K'.code.toByte()
        }

    private fun packet(seq: Int, top: ByteArray, bottom: ByteArray): ByteArray {
        val joined = top + bottom
        val buffer = java.nio.ByteBuffer.allocate(Wire.HEADER_R10 + joined.size)
            .order(java.nio.ByteOrder.LITTLE_ENDIAN)
        buffer.put(Wire.R10P.toByteArray(Charsets.US_ASCII))
        buffer.putInt(42)
        buffer.putInt(seq)
        buffer.putInt(1000 + seq)
        buffer.putInt(top.size)
        buffer.putInt(bottom.size)
        buffer.putInt(0)
        buffer.putInt(30)
        buffer.put(joined)
        return buffer.array()
    }

    @Test
    fun publishesAKeyPairThenADeltaPair() {
        val assembler = assembler()
        val topRows = rows(400, 1)
        val bottomRows = rows(320, 90)
        val keyTop = key(400, 70, topRows)
        val keyBottom = key(320, 60, bottomRows)
        val first = packet(1, keyTop, keyBottom)
        assembler.submit(first, first.size, 1_000_000)

        val completed = pair!!
        val top = resolver.resolve(completed.top, 0)
        val bottom = resolver.resolve(completed.bottom, 1)
        assertTrue(resolver.completePair(completed.seq, top, bottom))

        val changed = byteArrayOf(0x21, 0x31, 0xFF.toByte(), 0xD5.toByte())
        val expectedCompact = compact(320, 60, bottomRows.mapIndexed { index, row -> if (index == 5) changed else row })
        val delta = ByteArray(20 + 4 + changed.size)
        val header = java.nio.ByteBuffer.wrap(delta).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        header.put(Wire.R17D.toByteArray(Charsets.US_ASCII))
        header.put(1)
        header.put(60)
        header.putShort(320)
        header.putInt(1)
        header.putShort((1 shl 5).toShort())
        header.putShort(0)
        header.putInt(expectedCompact.size)
        header.putInt(changed.size)
        header.put(changed)

        val second = packet(2, compact(400, 70, topRows), delta)
        assembler.submit(second, second.size, 2_000_000)
        val deltaPair = pair!!
        assertEquals(2, deltaPair.seq)
        val resolvedBottom = resolver.resolve(deltaPair.bottom, 1)
        assertNotNull(resolvedBottom)
        assertArrayEquals(JpegHeader.expand(expectedCompact), resolvedBottom!!.jpeg)
    }
}
