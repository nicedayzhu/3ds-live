package io.github.nicedayzhu.three_ds_live.stream

/**
 * Minimal LZ4 block decoder with an external dictionary.
 *
 * The handheld sends `R20D` as one raw LZ4 block whose matches may reach back into
 * the last 32 KiB of the matched reference key. We therefore decode into a buffer
 * prefixed with the dictionary and return the tail.
 */
object Lz4 {
    fun decode(block: ByteArray, blockLength: Int, dictionary: ByteArray, maxOutput: Int): ByteArray? {
        if (blockLength <= 0 || maxOutput <= 0) return null
        val dict = if (dictionary.size > 32768) dictionary.copyOfRange(dictionary.size - 32768, dictionary.size) else dictionary
        val out = ByteArray(dict.size + maxOutput)
        System.arraycopy(dict, 0, out, 0, dict.size)
        var src = 0
        var dst = dict.size
        val end = minOf(blockLength, block.size)
        val limit = out.size
        try {
            while (src < end) {
                val token = block[src++].toInt() and 0xFF
                var literals = token ushr 4
                if (literals == 15) {
                    var value: Int
                    do {
                        if (src >= end) return null
                        value = block[src++].toInt() and 0xFF
                        literals += value
                    } while (value == 255)
                }
                if (src + literals > end || dst + literals > limit) return null
                System.arraycopy(block, src, out, dst, literals)
                src += literals
                dst += literals
                if (src >= end) break
                if (src + 2 > end) return null
                val offset = (block[src].toInt() and 0xFF) or ((block[src + 1].toInt() and 0xFF) shl 8)
                src += 2
                if (offset <= 0 || offset > dst) return null
                // The low token nibble is matchLength-4; extension bytes follow the offset.
                var match = token and 0x0F
                if (match == 15) {
                    var value: Int
                    do {
                        if (src >= end) return null
                        value = block[src++].toInt() and 0xFF
                        match += value
                    } while (value == 255)
                }
                val length = match + 4
                if (dst + length > limit) return null
                var copy = dst - offset
                repeat(length) {
                    out[dst++] = out[copy++]
                }
            }
        } catch (_: IndexOutOfBoundsException) {
            return null
        }
        val produced = dst - dict.size
        if (produced <= 0) return null
        return out.copyOfRange(dict.size, dst)
    }
}
