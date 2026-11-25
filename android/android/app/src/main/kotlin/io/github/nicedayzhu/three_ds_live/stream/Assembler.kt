package io.github.nicedayzhu.three_ds_live.stream

/**
 * Reassembles R10 combined pairs, recovers a single hole per XOR stripe, and asks
 * for selective repair when a pair stays incomplete inside its playout budget.
 */
class Assembler(
    private val onComplete: (Pair) -> Unit,
    private val onIncomplete: (seq: Int, missing: Int) -> Unit,
    private val onNeedRepair: (seq: Int, pts: Long, indices: IntArray) -> Unit,
) {
    class Pair(
        val seq: Int,
        val pts: Long,
        val fps: Int,
        val top: ByteArray,
        val bottom: ByteArray,
        val usedRepair: Boolean,
        val completeAtNanos: Long,
    )

    private class Partial(
        val seq: Int,
        val pts: Long,
        val fps: Int,
        val topLength: Int,
        val bottomLength: Int,
        val total: Int,
        val chunkCount: Int,
        val now: Long,
    ) {
        val chunks = arrayOfNulls<ByteArray>(chunkCount)
        val parity = arrayOfNulls<ByteArray>(Wire.STRIPES)
        val parityLength = IntArray(Wire.STRIPES)
        var received = 0
        var repairRequested = false
        var usedRepair = false
        var lastProgress = now
        var seen = false
    }

    private val partials = LinkedHashMap<Int, Partial>()
    private val completedRing = ArrayDeque<Int>()
    var combined = false
        private set
    var pairsCompleted = 0L
        private set
    var pairsFailed = 0L
        private set
    var recoveredChunks = 0L
        private set

    fun reset() {
        partials.clear()
        completedRing.clear()
        combined = false
        pairsCompleted = 0
        pairsFailed = 0
        recoveredChunks = 0
    }

    fun submit(data: ByteArray, length: Int, now: Long): Boolean {
        if (length < Wire.HEADER_R10) return false
        val tag = Wire.tag(data, length)
        if (tag != Wire.R10P && tag != Wire.R10F && tag != Wire.R19P) return false
        val session = Wire.int(data, 4)
        val seq = Wire.int(data, 8)
        val pts = Wire.u32(data, 12)
        val topLength = Wire.int(data, 16)
        val bottomLength = Wire.int(data, 20)
        val index = Wire.int(data, 24)
        val fps = Wire.int(data, 28)
        if (topLength !in 12..Wire.MAX_SCREEN_BYTES) return false
        if (bottomLength !in 12..Wire.MAX_SCREEN_BYTES) return false
        if (topLength + bottomLength > 2 * Wire.MAX_SCREEN_BYTES) return false
        if (fps !in intArrayOf(15, 20, 24, 30, 35, 40)) return false
        if (index < 0) return false

        val total = topLength + bottomLength
        val chunks = (total + Wire.CHUNK - 1) / Wire.CHUNK
        if (index >= chunks && tag != Wire.R10F) return false
        // A repeated datagram for an already published pair must not create a duplicate frame.
        if (completedRing.contains(seq)) return false

        var partial = partials[seq]
        if (partial == null) {
            if (tag == Wire.R10F) return false // parity without data header: nothing to attach to
            partial = Partial(seq, pts, fps, topLength, bottomLength, total, chunks, now)
            partials[seq] = partial
            trim(now)
        } else if (partial.total != total || partial.topLength != topLength) {
            return false
        }

        if (tag == Wire.R10F) {
            if (index >= Wire.STRIPES) return false
            val payload = length - Wire.HEADER_R10
            if (payload <= 0) return false
            if (partial.parity[index] == null) {
                partial.parity[index] = ByteArray(Wire.CHUNK)
                partial.parityLength[index] = payload
                System.arraycopy(data, Wire.HEADER_R10, partial.parity[index]!!, 0, payload)
                tryRecover(partial, index, now)
            }
        } else {
            if (partial.chunks[index] == null) {
                val payload = length - Wire.HEADER_R10
                if (payload <= 0) return false
                val copy = ByteArray(payload)
                System.arraycopy(data, Wire.HEADER_R10, copy, 0, payload)
                partial.chunks[index] = copy
                partial.received++
                partial.lastProgress = now
                if (tag == Wire.R19P) partial.usedRepair = true
                tryRecover(partial, index % Wire.STRIPES, now)
            }
        }
        finishIfComplete(partial, now)
        return true
    }

    private fun tryRecover(partial: Partial, stripe: Int, now: Long) {
        if (partial.parity[stripe] == null) return
        var missing = -1
        for (i in partial.chunks.indices) {
            if (i % Wire.STRIPES != stripe) continue
            if (partial.chunks[i] == null) {
                if (missing >= 0) return
                missing = i
            }
        }
        if (missing < 0) return
        val parity = partial.parity[stripe] ?: return
        val recovered = parity.copyOf()
        for (i in partial.chunks.indices) {
            if (i == missing || i % Wire.STRIPES != stripe) continue
            val chunk = partial.chunks[i] ?: continue
            for (b in chunk.indices) recovered[b] = (recovered[b].toInt() xor chunk[b].toInt()).toByte()
        }
        val expected = expectedChunkLength(partial, missing)
        if (expected <= 0 || expected > Wire.CHUNK) return
        partial.chunks[missing] = recovered.copyOf(expected)
        partial.received++
        recoveredChunks++
        partial.lastProgress = now
    }

    private fun expectedChunkLength(partial: Partial, index: Int): Int {
        val remaining = partial.total - index * Wire.CHUNK
        return if (remaining > Wire.CHUNK) Wire.CHUNK else remaining
    }

    private fun finishIfComplete(partial: Partial, now: Long) {
        if (partial.received < partial.chunkCount) return
        val joined = ByteArray(partial.total)
        var offset = 0
        for (i in partial.chunks.indices) {
            val chunk = partial.chunks[i] ?: return
            System.arraycopy(chunk, 0, joined, offset, chunk.size)
            offset += chunk.size
        }
        if (offset != partial.total) return
        val top = joined.copyOfRange(0, partial.topLength)
        val bottom = joined.copyOfRange(partial.topLength, partial.total)
        pairsCompleted++
        partials.remove(partial.seq)
        remember(partial.seq)
        onComplete(Pair(partial.seq, partial.pts, partial.fps, top, bottom, partial.usedRepair, now))
    }

    private fun remember(seq: Int) {
        completedRing.addLast(seq)
        while (completedRing.size > 32) completedRing.removeFirst()
    }

    private fun trim(now: Long) {
        val deadline = now - 1_000_000_000L
        val iterator = partials.entries.iterator()
        while (iterator.hasNext()) {
            val entry = iterator.next()
            if (entry.value.lastProgress < deadline) {
                pairsFailed++
                onIncomplete(entry.key, entry.value.chunkCount - entry.value.received)
                iterator.remove()
            }
        }
        while (partials.size > 4) {
            val oldest = partials.entries.minByOrNull { it.value.lastProgress } ?: break
            pairsFailed++
            onIncomplete(oldest.key, oldest.value.chunkCount - oldest.value.received)
            partials.remove(oldest.key)
        }
    }

    /** Called once per engine tick; requests repair for partials that stopped making progress. */
    fun poll(now: Long, maxIndices: Int, budgetNanos: Long) {
        val expired = partials.entries.filter { now - it.value.lastProgress > 1_000_000_000L }
        for (entry in expired) {
            pairsFailed++
            onIncomplete(entry.key, entry.value.chunkCount - entry.value.received)
            partials.remove(entry.key)
        }
        for (partial in partials.values) {
            if (partial.repairRequested || maxIndices <= 0) continue
            if (partial.received >= partial.chunkCount) continue
            val quiet = now - partial.lastProgress
            if (quiet < 3_000_000L) continue
            if (now - partial.lastProgress > budgetNanos) continue
            val missing = ArrayList<Int>()
            for (i in partial.chunks.indices) {
                if (partial.chunks[i] == null) missing.add(i)
                if (missing.size > maxIndices) break
            }
            if (missing.isEmpty() || missing.size > maxIndices) continue
            partial.repairRequested = true
            onNeedRepair(partial.seq, partial.pts, missing.toIntArray())
        }
    }

    fun hasRecent(seq: Int): Boolean = completedRing.contains(seq)
}
