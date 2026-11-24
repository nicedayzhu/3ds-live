#include <assert.h>
#include <stdio.h>
#include "pending_queue.h"

enum { CAPACITY = 32 };
static uint8_t arena[CAPACITY];
static LivePendingQueue queue;

static unsigned add(uint32_t seq, uint32_t bytes) {
    while (!live_pending_fits(&queue, bytes, CAPACITY)) {
        assert(queue.count);
        live_pending_remove(&queue, queue.head);
    }
    LivePendingPair next = {.length = {bytes / 2, bytes - bytes / 2}, .seq = seq,
                            .stamp = seq * 33, .epoch = 3, .queued = 1000u + seq};
    unsigned slot = live_pending_append(&queue, arena, CAPACITY, next);
    uint8_t *out = arena + queue.slots[slot].offset;
    memset(out, (uint8_t)seq, next.length[0]);
    memset(out + next.length[0], (uint8_t)~seq, next.length[1]);
    return slot;
}

static void check(unsigned slot, uint32_t seq, uint32_t size) {
    const LivePendingPair *pair = &queue.slots[slot];
    assert(pair->seq == seq && pair->stamp == seq * 33 && pair->queued == 1000u + seq);
    assert(pair->length[0] == size / 2 && pair->length[1] == size - size / 2);
    for (unsigned i = 0; i < size; ++i)
        assert(arena[pair->offset + i] == (uint8_t)(i < pair->length[0] ? seq : ~seq));
}

int main(void) {
    unsigned first = add(UINT32_MAX, 9), second = add(0, 12);
    assert(queue.count == 2 && queue.head == first && second != first);
    /* A sender stall keeps two frames in order even across a sequence wrap. */
    check(first, UINT32_MAX, 9);
    check(second, 0, 12);
    /* 15 requires compacting the 12 bytes at offset 9. Its identity and both
     * panels survive overflow, while the third frame reuses the oldest slot. */
    unsigned third = add(1, 15);
    assert(third == first && queue.count == 2 && queue.head == second);
    check(second, 0, 12);
    check(third, 1, 15);
    assert(!live_pending_expired(&queue.slots[second], 1066, 1000, 30));
    assert(live_pending_expired(&queue.slots[second], 1067, 1000, 30));
    assert(!live_pending_expired(&queue.slots[third], 1067, 1000, 30));
    live_pending_remove(&queue, second);
    unsigned fourth = add(2, 10); /* Fits before the retained tail, no move. */
    assert(queue.slots[fourth].offset == 0 && queue.slots[third].offset == 12);
    check(third, 1, 15);
    check(fourth, 2, 10);

    /* A rate change may reject just the head or just the tail. A new format,
     * pause or peer starts a new epoch: neither previous frame is eligible. */
    assert(!live_pending_compatible(&queue.slots[third], 3, 11));
    assert(live_pending_compatible(&queue.slots[fourth], 3, 11));
    live_pending_remove(&queue, third);
    unsigned fifth = add(3, 20);
    assert(live_pending_compatible(&queue.slots[fourth], 3, 11));
    assert(!live_pending_compatible(&queue.slots[fifth], 3, 11));
    live_pending_remove(&queue, fifth);
    assert(queue.count == 1 && queue.head == fourth);
    fifth = add(3, 20);
    assert(!live_pending_compatible(&queue.slots[fourth], 4, CAPACITY));
    assert(!live_pending_compatible(&queue.slots[fifth], 4, CAPACITY));
    while (queue.count) live_pending_remove(&queue, queue.head);
    /* One full-capacity frame still fits, and wins over two older frames. */
    add(4, 10);
    add(5, 18);
    unsigned large = add(6, CAPACITY);
    assert(queue.count == 1 && queue.head == large);
    check(large, 6, CAPACITY);
    assert(!live_pending_fits(&queue, CAPACITY + 1, CAPACITY));
    puts("pending FIFO, stable slots, contiguous panels, age, capacity and config OK");
    return 0;
}
