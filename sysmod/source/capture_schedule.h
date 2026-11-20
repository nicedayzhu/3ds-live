#ifndef LIVE_CAPTURE_SCHEDULE_H
#define LIVE_CAPTURE_SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>

// Give a transient DMA refusal one capture period. During an applet handoff the
// same unreadable framebuffer can stay selected for seconds.
static inline uint32_t live_capture_retry_us(uint32_t previous_us, unsigned fps)
{
    const uint32_t maximum = 250000u;
    if (!previous_us) return 1000000u / fps;
    return previous_us >= maximum / 2 ? maximum : previous_us * 2;
}

// A swap can be observed up to 8 ms early. Between swaps, wait a full frame
// and sample mid-refresh so DMA does not straddle the next game buffer reuse.
static inline bool live_capture_due(uint32_t since_capture, unsigned fps, bool changed_now,
                                    bool pending_change, bool precise_swap, uint32_t since_swap,
                                    bool vram)
{
    const uint32_t period = 1000000u / fps;
    if (since_capture < period - 8000u) return false;
    if (changed_now) return true;
    if (vram && since_capture >= 100000u && since_swap >= 100000u) return true;
    const uint32_t phase = since_swap % 16713u;
    return pending_change && precise_swap && since_capture >= period && phase >= 3000u && phase <= 10713u;
}

#endif
