#ifndef LIVE_CAPTURE_SCHEDULE_H
#define LIVE_CAPTURE_SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t next_us;
	unsigned fps; // Zero until the first successful capture of a stable
		      // VRAM buffer.
} LiveVramCadence;

static inline void live_vram_captured(LiveVramCadence *clock, uint32_t now,
				      unsigned fps)
{
	const uint32_t period = 1000000u / fps;
	// Retain phase across normal LCD polling jitter. A long scheduling
	// stall starts a new period rather than taking two copies in rapid
	// succession.
	uint32_t slot = now;
	if (clock->fps == fps && now - clock->next_us <= 8000u)
		slot = clock->next_us;
	clock->next_us = slot + period;
	clock->fps = fps;
}

// Give a transient DMA refusal one capture period. During an applet handoff the
// same unreadable framebuffer can stay selected for seconds.
static inline uint32_t live_capture_retry_us(uint32_t previous_us, unsigned fps)
{
	const uint32_t maximum = 250000u;
	if (!previous_us)
		return 1000000u / fps;
	return previous_us >= maximum / 2 ? maximum : previous_us * 2;
}

// A swap can be observed up to 8 ms early. Between swaps, wait a full frame
// and sample mid-refresh so DMA does not straddle the next game buffer reuse.
static inline bool live_capture_due(uint32_t since_capture, unsigned fps,
				    bool changed_now, bool pending_change,
				    bool precise_swap, uint32_t since_swap,
				    const LiveVramCadence *vram, uint32_t now)
{
	const uint32_t period = 1000000u / fps;
	if (since_capture < period - 8000u)
		return false;
	if (changed_now)
		return true;
	/* Games may redraw the displayed VRAM buffer in place. Once the same
	 * buffer has been stable for 100 ms, read it at the selected cadence.
	 */
	if (vram && since_swap >= 100000u) {
		return since_capture >= period ||
		       (vram->fps == fps &&
			(int32_t)(now - vram->next_us) >= 0);
	}
	const uint32_t phase = since_swap % 16713u;
	return pending_change && precise_swap && since_capture >= period &&
	       phase >= 3000u && phase <= 10713u;
}

#endif
