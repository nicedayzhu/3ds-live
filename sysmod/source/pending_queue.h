#ifndef LIVE_PENDING_QUEUE_H
#define LIVE_PENDING_QUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The encoder and sender hold the pair lock when accessing the queue. The
 * stable slot index is also the identity of its optional timing record. */
typedef struct {
    uint32_t offset, length[2], seq, stamp, epoch;
    uint64_t queued;
    bool r20;
} LivePendingPair;

typedef struct {
    LivePendingPair slots[2];
    unsigned head, count;
} LivePendingQueue;

static inline uint32_t live_pending_bytes(const LivePendingPair *pair) {
    return pair->length[0] + pair->length[1];
}

static inline bool live_pending_compatible(const LivePendingPair *pair, uint32_t epoch, uint32_t budget) {
    return pair->epoch == epoch && live_pending_bytes(pair) <= budget;
}

static inline void live_pending_remove(LivePendingQueue *queue, unsigned slot) {
    if (slot == queue->head) queue->head ^= 1u;
    --queue->count;
}

static inline bool live_pending_fits(const LivePendingQueue *queue, uint32_t bytes, uint32_t capacity) {
    return bytes <= capacity && queue->count < 2 &&
           (!queue->count || live_pending_bytes(&queue->slots[queue->head]) <= capacity - bytes);
}

/* Call after evicting oldest pairs until live_pending_fits(). Both panels
 * stay adjacent. Move the one live pair only if free space is fragmented. */
static inline unsigned live_pending_append(LivePendingQueue *queue, uint8_t *arena,
                                           uint32_t capacity, LivePendingPair pair) {
    pair.offset = 0;
    if (queue->count) {
        LivePendingPair *head = &queue->slots[queue->head];
        uint32_t kept = live_pending_bytes(head), end = head->offset + kept;
        uint32_t bytes = live_pending_bytes(&pair);
        if (bytes <= capacity - end) {
            pair.offset = end;
        } else if (bytes > head->offset) {
            memmove(arena, arena + head->offset, kept);
            head->offset = 0;
            pair.offset = kept;
        }
    }
    unsigned slot = queue->head ^ queue->count;
    queue->slots[slot] = pair;
    ++queue->count;
    return slot;
}

static inline bool live_pending_expired(const LivePendingPair *pair, uint64_t now,
                                        uint64_t clockHz, uint32_t fps) {
    return now - pair->queued > clockHz / fps * 2;
}

#endif
