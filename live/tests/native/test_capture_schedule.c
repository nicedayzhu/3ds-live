#include <assert.h>
#include <stdio.h>
#include "capture_schedule.h"

int main(void)
{
    assert(!live_capture_due(16500, 30, true, true, true, 0, false));
    assert(live_capture_due(33500, 30, true, true, true, 0, false));
    assert(!live_capture_due(34000, 30, false, true, true, 16000, false));
    assert(!live_capture_due(34000, 30, false, true, false, 6000, false));
    assert(live_capture_due(34000, 30, false, true, true, 6000, false));
    assert(!live_capture_due(100000, 30, false, false, true, 100000, false));
    assert(live_capture_due(100000, 30, false, false, true, 100000, true));
    // Even with a continuously swapping, unreadable source, an LCD poll every
    // 2 ms must not turn a failed capture into hundreds of DMA attempts a second.
    const unsigned rates[] = {15, 20, 24, 30};
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; ++i) {
        uint32_t retry_at = 0, delay = 0;
        unsigned attempts = 0;
        for (uint32_t now = 0; now < 1000000u; now += 2000u) {
            if (now < retry_at) continue;
            delay = live_capture_retry_us(delay, rates[i]);
            if (!attempts) assert(delay == 1000000u / rates[i]);
            retry_at = now + delay;
            ++attempts;
        }
        assert(attempts <= 7 && delay == 250000u);
    }
    // The game swaps at 59.8 Hz. Every second sampled frame can have the same address as the previous one.
    for (unsigned fps = 15; fps <= 30; fps += 15) {
        unsigned samples = 0, previous = 0, last_swap = 0, last_capture = 0;
        bool pending = false;
        for (unsigned t = 2000; t <= 4000000; t += 2000) {
            unsigned selection = (t / 16713u) & 1u;
            bool changed = selection != previous;
            if (changed) { pending = true; last_swap = t; }
            previous = selection;
            if (live_capture_due(t-last_capture, fps, changed, pending, true, t-last_swap, false)) {
                last_capture = t;
                pending = false;
                ++samples;
            }
        }
        assert(samples >= 4 * fps - 2 && samples <= 4 * fps + 1);
    }
    puts("LCD capture cadence OK");
    return 0;
}
