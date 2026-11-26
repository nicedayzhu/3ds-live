#ifndef LIVE_REPAIR_H
#define LIVE_REPAIR_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* R19 caches the actual concatenated R10 payload. The arena is the same
 * size as the old two transmit buffers; a large frame can evict older ones. */
#define LIVE_REPAIR_SLOTS 8u
#define LIVE_REPAIR_MAX 16u
#define LIVE_REPAIR_HISTORY_MS 250u
#define LIVE_REPAIR_SERVICE_MS 45u
typedef struct {
	uint32_t offset, top, bottom, seq, pts, fps;
	uint64_t expires, service_until;
	uint8_t indices[LIVE_REPAIR_MAX], count, next;
	bool complete;
} LiveRepairFrame;
typedef struct {
	LiveRepairFrame frames[LIVE_REPAIR_SLOTS];
	uint32_t write_offset, next_slot;
	uint32_t missing, stale, duplicate, invalid, expired_fragments,
	    evicted_fragments;
} LiveRepairHistory;

/* Keep stream diagnostics when configuration or discovery invalidates cached
 * bytes. */
static inline void live_repair_clear(LiveRepairHistory *h)
{
	memset(h->frames, 0, sizeof h->frames);
	h->write_offset = h->next_slot = 0;
}

static inline void live_repair_evict(LiveRepairHistory *h, LiveRepairFrame *f,
				     uint64_t now)
{
	if (f->next < f->count) {
		if (now >= f->service_until)
			h->expired_fragments += f->count - f->next;
		else
			h->evicted_fragments += f->count - f->next;
	}
	*f = (LiveRepairFrame){0};
}

static inline LiveRepairFrame *
live_repair_reserve(LiveRepairHistory *h, uint32_t capacity, uint32_t top,
		    uint32_t bottom, uint32_t seq, uint32_t pts, uint32_t fps,
		    uint64_t now, uint64_t expires)
{
	if (!top || !bottom || top > capacity || bottom > capacity - top)
		return NULL;
	uint32_t length = top + bottom;
	for (unsigned i = 0; i < LIVE_REPAIR_SLOTS; ++i) {
		LiveRepairFrame *f = &h->frames[i];
		if (f->top == top && f->bottom == bottom && f->seq == seq &&
		    f->pts == pts && f->fps == fps)
			return f; /* Repeated reference key; keep its one-shot
				     request history. */
	}
	uint32_t start =
	    h->write_offset <= capacity - length ? h->write_offset : 0;
	for (unsigned i = 0; i < LIVE_REPAIR_SLOTS; ++i) {
		LiveRepairFrame *f = &h->frames[i];
		if (f->top && start < f->offset + f->top + f->bottom &&
		    f->offset < start + length)
			live_repair_evict(h, f, now);
	}
	LiveRepairFrame *slot = &h->frames[h->next_slot];
	if (slot->top)
		live_repair_evict(h, slot, now);
	*slot = (LiveRepairFrame){.offset = start,
				  .top = top,
				  .bottom = bottom,
				  .seq = seq,
				  .pts = pts,
				  .fps = fps,
				  .expires = expires};
	h->next_slot = (h->next_slot + 1) % LIVE_REPAIR_SLOTS;
	h->write_offset = start + length;
	return slot;
}

static inline bool live_repair_request(LiveRepairHistory *h, uint32_t seq,
				       uint32_t pts, const uint8_t *indices,
				       unsigned count, uint32_t chunk,
				       uint64_t now, uint64_t service_window)
{
	if (!indices || !count || count > LIVE_REPAIR_MAX || !chunk ||
	    !service_window) {
		++h->invalid;
		return false;
	}
	for (unsigned i = 0; i < LIVE_REPAIR_SLOTS; ++i) {
		LiveRepairFrame *f = &h->frames[i];
		if (!f->top || f->seq != seq || f->pts != pts)
			continue;
		if (now >= f->expires) {
			++h->stale;
			return false;
		}
		if (f->count) {
			++h->duplicate;
			return false;
		}
		uint32_t fragments = 1 + (f->top + f->bottom - 1) / chunk;
		for (unsigned j = 0; j < count; ++j) {
			if (indices[j] >= fragments ||
			    memchr(indices, indices[j], j)) {
				++h->invalid;
				return false;
			}
		}
		memcpy(f->indices, indices, count);
		f->count = (uint8_t)count;
		f->service_until = f->expires - now < service_window
				       ? f->expires
				       : now + service_window;
		return true;
	}
	++h->missing;
	return false;
}

static inline LiveRepairFrame *live_repair_next(LiveRepairHistory *h,
						uint64_t now)
{
	LiveRepairFrame *first = NULL;
	for (unsigned i = 0; i < LIVE_REPAIR_SLOTS; ++i) {
		LiveRepairFrame *f = &h->frames[i];
		if (f->next >= f->count)
			continue;
		if (now >= f->service_until) {
			h->expired_fragments += f->count - f->next;
			f->next = f->count;
		} else if (f->complete &&
			   (!first ||
			    f->service_until < first->service_until)) {
			first = f;
		}
	}
	return first;
}

static inline uint32_t live_repair_allowance(uint32_t video_rate, uint32_t fps,
					     uint32_t fresh_wire)
{
	uint32_t allowance =
	    fps ? (uint32_t)((uint64_t)video_rate * 96 / (100 * fps)) : 0;
	return allowance > fresh_wire ? allowance - fresh_wire : 0;
}
#endif
