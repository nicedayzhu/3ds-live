/*
 * R20 delta/dictionary stage.
 */
#include "live_r20.h"

#include "live_discovery.h"
#include "live_jpeg_rows.h"
#include "row_compression.h"

#include <3ds.h>
#include <string.h>

void live_r20_init(struct live_r20 *r20)
{
	memset(r20, 0, sizeof(*r20));
}

void live_r20_forget(struct live_r20 *r20, const struct row_anchor *anchor)
{
	if (r20->prepared != anchor)
		return;

	r20->prepared = NULL;
	r20->dictionary = NULL;
}

bool live_r20_recent(const struct live_r20 *r20, uint64_t now)
{
	return r20->last_ack && now >= r20->last_ack &&
	       now - r20->last_ack <
		   (uint64_t)LIVE_PEER_LEASE_SECONDS * SYSCLOCK_ARM11;
}

void live_r20_compress(struct live_r20 *r20, struct row_pair_plan *plan,
		       struct row_anchor *base, uint64_t next_encode,
		       uint8_t *encoded_top, uint8_t *encoded_bottom,
		       size_t max_bytes)
{
	uint32_t len = plan->wire[1];
	uint32_t compact = plan->compact[1];
	uint64_t began;
	uint8_t *delta;
	uint8_t *out;
	LZ4_stream_t *work;
	size_t bytes;
	uint32_t us;

	if (!base || len >= compact || len <= LIVE_JPEG_DELTA_HEADER + 1 ||
	    base->length[0] + base->length[1] >
		ROW_PAIR_BYTES - sizeof(LZ4_stream_t) ||
	    compact + len > max_bytes - sizeof(LZ4_stream_t) ||
	    max_bytes - plan->compact[0] <= 20)
		return;

	began = svcGetSystemTick();
	if (!live_r20_time_available(began, next_encode, SYSCLOCK_ARM11,
				     r20->last_attempt, r20->mean_us)) {
		r20->skipped++;
		return;
	}

	delta = encoded_bottom + compact;
	if (live_jpeg_rows_pack(delta, len, encoded_bottom, compact,
				&plan->rows[1], row_anchor_panel(base, 1),
				base->length[1], &base->rows[1],
				base->seq) != len)
		return;
	plan->bottom_delta = delta;
	plan->bottom_delta_bytes = len;

	if (r20->prepared != base) {
		uint8_t *bank_tail =
		    base->bytes + ROW_PAIR_BYTES - sizeof(LZ4_stream_t);

		r20->dictionary = live_r20_dictionary(
		    bank_tail, sizeof(LZ4_stream_t), row_anchor_panel(base, 1),
		    base->length[1]);
		r20->prepared = base;
	}
	if (!r20->dictionary)
		return;

	out = encoded_top + plan->compact[0];
	work =
	    (LZ4_stream_t *)(encoded_bottom + max_bytes - sizeof(LZ4_stream_t));
	bytes = live_r20_pack(out, max_bytes - plan->compact[0], delta, len,
			      base->seq, r20->dictionary, work);
	us =
	    (uint32_t)((svcGetSystemTick() - began) * 1000000 / SYSCLOCK_ARM11);
	r20->mean_us = r20->last_attempt
			   ? (uint32_t)((3ull * r20->mean_us + us + 2) / 4)
			   : us;
	r20->last_attempt = began;
	r20->attempts++;
	r20->encode_sum_us += us;
	if (us > r20->max_us)
		r20->max_us = us;
	if (bytes) {
		plan->bottom_compressed = out;
		plan->wire[1] = (uint32_t)bytes;
	}
}
