#include <assert.h>
#include <stdio.h>
#include "capture_schedule.h"

static void test_fixed_vram(unsigned fps, uint32_t origin)
{
    LiveVramCadence clock = {0};
    uint32_t last = origin, captures = 0;
    const uint32_t period = 1000000u / fps;
    live_vram_captured(&clock, origin, fps);
    for (uint32_t elapsed = 2000; elapsed <= 10000000u; elapsed += 2000) {
        uint32_t now = origin + elapsed;
        if (!live_capture_due(now-last, fps, false, false, true, 200000, &clock, now)) continue;
        assert(now-last >= period-2000 && now-last <= period+2000);
        // A continuously running clock remains within one polling tick of its original phase.
        ++captures;
        assert(elapsed >= captures*period && elapsed-captures*period < 2000);
        live_vram_captured(&clock, now, fps);
        last = now;
    }
    assert(captures == 10u * fps);

    uint32_t late = clock.next_us + 3*period + 2000u;
    assert(live_capture_due(late-last, fps, false, false, true, 200000, &clock, late));
    live_vram_captured(&clock, late, fps);
    assert(clock.next_us == late+period);
    assert(!live_capture_due(2000, fps, false, false, true, 200000, &clock, late+2000));
    assert(live_capture_due(period, fps, false, false, true, 200000, &clock, late+period));
}

int main(void)
{
    const LiveVramCadence initial = {0};
    assert(!live_capture_due(16500, 30, true, true, true, 0, NULL, 16500));
    assert(live_capture_due(33500, 30, true, true, true, 0, NULL, 33500));
    assert(!live_capture_due(34000, 30, false, true, true, 16000, NULL, 34000));
    assert(!live_capture_due(34000, 30, false, true, false, 6000, NULL, 34000));
    assert(live_capture_due(34000, 30, false, true, true, 6000, NULL, 34000));
    assert(!live_capture_due(100000, 30, false, false, true, 100000, NULL, 100000));
    assert(live_capture_due(100000, 30, false, false, true, 100000, &initial, 100000));
    // Even with a continuously swapping, unreadable source, an LCD poll every
    // 2 ms must not turn a failed capture into hundreds of DMA attempts a second.
    const unsigned rates[] = {15, 20, 24, 30};
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; ++i) {
        uint32_t retry_at = 0, delay = 0;
        uint32_t period = 1000000u / rates[i];
        /* A single VRAM buffer can be redrawn without a buffer swap. */
        assert(!live_capture_due(period-1, rates[i], false, false, true, 200000, &initial, period-1));
        assert(live_capture_due(period, rates[i], false, false, true, 200000, &initial, period));
        assert(!live_capture_due(period, rates[i], false, false, true, 99000, &initial, period));
        assert(!live_capture_due(period, rates[i], false, false, true, 200000, NULL, period));
        test_fixed_vram(rates[i],0);
        test_fixed_vram(rates[i],UINT32_MAX-65000u);
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
            if (live_capture_due(t-last_capture, fps, changed, pending, true, t-last_swap, NULL, t)) {
                last_capture = t;
                pending = false;
                ++samples;
            }
        }
        assert(samples >= 4 * fps - 2 && samples <= 4 * fps + 1);
    }
    // A slower rate takes effect from the real last copy; switching out of stable VRAM restores swap gating.
    LiveVramCadence clock = {0};
    live_vram_captured(&clock, 200000, 30);
    assert(!live_capture_due(38000, 24, false, false, true, 200000, &clock, 238000));
    assert(live_capture_due(42000, 24, false, false, true, 200000, &clock, 242000));
    live_vram_captured(&clock, 242000, 24);
    assert(clock.next_us == 242000u+1000000u/24);
    assert(!live_capture_due(43000, 24, false, false, true, 2000, &clock, 285000));
    assert(live_capture_due(35000, 24, true, true, true, 0, &clock, 277000));
    puts("LCD capture cadence OK");
    return 0;
}
