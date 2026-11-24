#include <assert.h>
#include <stdio.h>
#include "live_repair.h"

/* Test time is in milliseconds. The ARM caller converts these windows to ticks. */
static LiveRepairFrame *reserve(LiveRepairHistory *h, unsigned cap, unsigned size,
                                unsigned seq, uint64_t now) {
    return live_repair_reserve(h, cap, size / 2, size - size / 2, seq, 100 + seq,
                               30, now, now + LIVE_REPAIR_HISTORY_MS);
}

static bool request(LiveRepairHistory *h, unsigned seq, const uint8_t *holes,
                    unsigned count, uint64_t now) {
    return live_repair_request(h, seq, 100 + seq, holes, count, 16, now, LIVE_REPAIR_SERVICE_MS);
}

static void test_bounded_requests(void) {
    LiveRepairHistory h = {0};
    const uint8_t holes[] = {3, 0, 1};
    LiveRepairFrame *f = reserve(&h, 256, 64, 9, 0);
    assert(f && f->offset == 0);
    assert(!live_repair_request(&h, 9, 110, holes, 2, 16, 20, 45));
    assert(!request(&h, 9, (uint8_t[]){0, 4}, 2, 20));
    assert(!request(&h, 9, (uint8_t[]){1, 1}, 2, 20));
    assert(!request(&h, 9, holes, 17, 20));
    assert(h.missing == 1 && h.invalid == 3);
    assert(request(&h, 9, holes, 3, 20));
    assert(!live_repair_next(&h, 30)); /* Its original is still being transmitted. */
    f->complete = true;
    assert(live_repair_next(&h, 30) == f && f->indices[f->next++] == 3);
    assert(!request(&h, 9, holes, 3, 31) && h.duplicate == 1);
    assert(reserve(&h, 256, 64, 9, 50) == f && f->next == 1 && f->expires == 250);
    assert(live_repair_next(&h, 64) == f && !live_repair_next(&h, 65));
    assert(h.expired_fragments == 2 && !live_repair_next(&h, 200));
    assert(!request(&h, 9, holes, 3, 250) && h.stale == 1);
    assert(h.expired_fragments == 2);
}

static void test_late_feedback_and_send_order(void) {
    LiveRepairHistory h = {0};
    LiveRepairFrame *f[8];
    const uint8_t hole[] = {0};
    for (unsigned i = 0; i < 7; ++i) {
        f[i] = reserve(&h, 256000, 23080, i, 33 * i);
        assert(f[i]);
        f[i]->complete = true;
    }
    /* The real motion trace included first-packet delays of 78..141 ms plus
     * 26..68 ms before the PC's request. Earlier frames are still resident. */
    assert(request(&h, 2, hole, 1, 208));
    assert(request(&h, 1, hole, 1, 210));
    assert(request(&h, 0, hole, 1, 230));
    assert(live_repair_next(&h, 232) == f[0]); /* absolute retention ends at 250 */
    ++f[0]->next;
    assert(live_repair_next(&h, 245) == f[2]); /* accepted earlier than sequence 1 */
    ++f[2]->next;
    assert(live_repair_next(&h, 254) == f[1] && !live_repair_next(&h, 255));
    assert(h.expired_fragments == 1);
    assert(!request(&h, 1, hole, 1, 283) && h.stale == 1);
    /* A lost NACK cannot spontaneously retransmit. */
    assert(!live_repair_next(&h, 300));
}

static void test_byte_wrap_and_metadata_capacity(void) {
    LiveRepairHistory h = {0};
    const uint8_t hole[] = {0};
    LiveRepairFrame *first = reserve(&h, 128, 64, 1, 0);
    LiveRepairFrame *next = reserve(&h, 128, 48, 2, 1);
    assert(first->offset == 0 && next->offset == 64);
    first->complete = next->complete = true;
    assert(request(&h, 1, hole, 1, 2) && request(&h, 2, hole, 1, 3));
    assert(reserve(&h, 128, 32, 3, 4)->offset == 0 && h.evicted_fragments == 1);
    assert(live_repair_next(&h, 6) == next && !request(&h, 1, hole, 1, 6));
    assert(!reserve(&h, 128, 129, 4, 7));
    assert(reserve(&h, 128, 128, 4, 52)->offset == 0 && h.expired_fragments == 1);
    assert(!live_repair_next(&h, 53));
    live_repair_clear(&h);
    for (unsigned i = 0; i < LIVE_REPAIR_SLOTS + 1; ++i) {
        LiveRepairFrame *f = reserve(&h, 256000, 2000, UINT32_MAX - 3 + i, i);
        assert(f);
        f->complete = true;
    }
    assert(!request(&h, UINT32_MAX - 3, hole, 1, 10));
    for (unsigned i = 1; i <= LIVE_REPAIR_SLOTS; ++i)
        assert(request(&h, UINT32_MAX - 3 + i, hole, 1, 11 + i));
    assert(h.evicted_fragments == 1 && h.expired_fragments == 1);
}

int main(void) {
    test_bounded_requests();
    test_late_feedback_and_send_order();
    test_byte_wrap_and_metadata_capacity();
    assert(live_repair_allowance(1900000,30,34800) == 26000);
    assert(live_repair_allowance(900000,30,30000) == 0);
    puts("bounded selective repair cache passed");
    return 0;
}
