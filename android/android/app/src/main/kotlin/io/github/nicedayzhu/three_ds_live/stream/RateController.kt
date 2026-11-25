package io.github.nicedayzhu.three_ds_live.stream

/**
 * Adaptive bandwidth ceiling for the sender.
 *
 * Backs off after three weak one-second windows and probes upward only after eight
 * healthy ones, mirroring the intent of the PC controller without its full table of
 * delivery-quality steps. Returns the new rate only when it changes.
 */
class RateController(private val floor: Int = 400_000) {
    var requested = 0
        private set
    private var ceiling = 0
    private var quietWindows = 0
    private var healthyWindows = 0

    fun reset(rate: Int) {
        ceiling = rate.coerceAtLeast(floor)
        requested = ceiling
        quietWindows = 0
        healthyWindows = 0
    }

    fun setCeiling(rate: Int) {
        ceiling = rate.coerceAtLeast(floor)
        if (requested > ceiling) requested = ceiling
        if (requested < floor) requested = floor
    }

    /** Backs off quickly when audio blocks are being lost: audio is small and cheap
     *  to protect, and freeing airtime lowers loss for both streams. */
    fun notifyAudioLoss(): Int? {
        if (requested <= floor) return null
        requested = (requested * 3 / 4).coerceAtLeast(floor)
        quietWindows = 0
        healthyWindows = 0
        return requested
    }

    /**
     * @param completeFps pairs completed in the window divided by its seconds
     * @return the new requested rate when it changed, otherwise null
     */
    fun observe(completeFps: Double, targetFps: Int, seconds: Double): Int? {
        if (seconds <= 0 || targetFps <= 0 || requested <= 0) return null
        val target = targetFps.toDouble()
        if (completeFps < target * 0.8) {
            quietWindows++
            healthyWindows = 0
        } else if (completeFps >= target * 0.95) {
            healthyWindows++
            quietWindows = 0
        } else {
            quietWindows = 0
            healthyWindows = 0
        }
        if (quietWindows >= 3 && requested > floor) {
            requested = (requested * 4 / 5).coerceAtLeast(floor)
            quietWindows = 0
            return requested
        }
        if (healthyWindows >= 8 && requested < ceiling) {
            requested = (requested * 5 / 4).coerceAtMost(ceiling)
            healthyWindows = 0
            return requested
        }
        return null
    }
}
