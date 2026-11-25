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
