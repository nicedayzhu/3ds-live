package io.github.nicedayzhu.three_ds_live.stream

/**
 * Turns wire screens into decodable JPEGs and owns the acknowledged row reference.
 *
 * Kept free of Android types so the reconstruction rules can be unit-tested against
 * the exact payloads the handheld sends (R8J1, R17K, R17D, R20D).
 */
class ScreenResolver {
    class Resolved(
        val jpeg: ByteArray,
        val quality: Int,
        val isKey: Boolean,
        val parsed: JpegRows.Parsed?,
    )

    private class Reference(
        val seq: Int,
        val top: JpegRows.Parsed,
        val bottom: JpegRows.Parsed,
    )

    private var reference: Reference? = null
    var usesDictionary = false
    var requestFreshKey = false
        private set

    val referenceSeq: Int? get() = reference?.seq

    fun reset() {
        reference = null
        requestFreshKey = false
    }

    fun setDictionarySupported(supported: Boolean) {
        usesDictionary = supported
    }

    /**
     * Resolves one screen. Returns null when a delta references a frame that is not
     * cached, in which case [requestFreshKey] is raised for the caller to ack with 0.
     */
    fun resolve(data: ByteArray, screen: Int): Resolved? {
        if (!Wire.validScreen(data)) return null
        if (Wire.isReferenceKey(data)) {
            val parsed = JpegRows.parse(data) ?: return null
            return Resolved(decodeBlock(parsed, retag = true), parsed.quality, true, parsed)
        }
        if (Wire.isCompactJpeg(data)) {
            val parsed = JpegRows.parse(data) ?: return null
            return Resolved(expandCompact(data), parsed.quality, false, parsed)
        }
        if (Wire.isFullJpeg(data)) {
            return Resolved(data, 0, false, null)
        }
        if (Wire.isDelta(data)) {
            val parsedReference = referenceFor(screen) ?: return missingReference()
            val compact = JpegRows.restore(parsedReference, data) ?: return missingReference()
            val quality = data[5].toInt() and 0xFF
            return Resolved(expandCompact(compact), quality, false, null)
        }
        if (Wire.isCompressedDelta(data)) {
            if (!usesDictionary) return null
            val current = reference ?: return missingReference()
            if (Wire.int(data, 8) != current.seq) return missingReference()
            val innerLength = Wire.int(data, 12)
            if (innerLength !in 20..Wire.MAX_SCREEN_BYTES) return null
            if (Wire.int(data, 16) != 0) return null
            val dictionary = JpegRows.dictionary(current.bottom)
            val block = data.copyOfRange(20, data.size)
            val inner = Lz4.decode(block, block.size, dictionary, innerLength) ?: return null
            if (inner.size != innerLength || !Wire.isDelta(inner)) return null
            val compact = JpegRows.restore(current.bottom, inner) ?: return missingReference()
            val quality = inner[5].toInt() and 0xFF
            return Resolved(expandCompact(compact), quality, false, null)
        }
        return null
    }

    /**
     * Called after both screens of a pair resolved. Adopts a validated R17K pair as the
     * new reference and reports whether the caller should acknowledge it.
     */
    fun completePair(seq: Int, top: Resolved?, bottom: Resolved?): Boolean {
        val topParsed = top?.parsed
        val bottomParsed = bottom?.parsed
        if (top?.isKey == true && bottom?.isKey == true && topParsed != null && bottomParsed != null) {
            reference = Reference(seq, topParsed, bottomParsed)
            requestFreshKey = false
            return true
        }
        return false
    }

    fun consumeFreshKeyRequest(): Boolean {
        val pending = requestFreshKey
        requestFreshKey = false
        return pending
    }

    fun referenceSize(screen: Int): Int = referenceFor(screen)?.compact?.size ?: 0

    private fun referenceFor(screen: Int): JpegRows.Parsed? {
        val current = reference ?: return null
        return if (screen == 0) current.top else current.bottom
    }

    private fun missingReference(): Resolved? {
        requestFreshKey = true
        return null
    }

    private fun decodeBlock(parsed: JpegRows.Parsed, retag: Boolean): ByteArray {
        val compact = if (!retag) parsed.compact else {
            val copy = parsed.compact.copyOf()
            copy[0] = 'R'.code.toByte()
            copy[1] = '8'.code.toByte()
            copy[2] = 'J'.code.toByte()
            copy[3] = '1'.code.toByte()
            copy
        }
        return expandCompact(compact)
    }

    private fun expandCompact(compact: ByteArray): ByteArray = JpegHeader.expand(compact)
}
