#ifndef LIVE_CAPTURE_SCHEDULE_H
#define LIVE_CAPTURE_SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>

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
