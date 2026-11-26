/*
 * Optional R20 bottom delta + LZ4 dictionary encoder.
 *
 * Only the encoder thread calls r20_compress(); the network thread may only
 * acknowledge a reference through r20_ack()/r20_forget().
 */
#ifndef LIVE_R20_H
#define LIVE_R20_H

#include "live_row.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct live_r20 {
	const struct row_anchor *prepared;
	LZ4_stream_t *dictionary;
	uint64_t last_ack;
	uint64_t last_attempt;
	uint64_t encode_sum_us;
	uint64_t saved;
	uint32_t attempts;
	uint32_t panels;
	uint32_t max_us;
	uint32_t mean_us;
	uint32_t skipped;
};

void live_r20_init(struct live_r20 *r20);

bool live_r20_recent(const struct live_r20 *r20, uint64_t now);

/* Drop the cached dictionary if it belongs to anchor. */
void live_r20_forget(struct live_r20 *r20, const struct row_anchor *anchor);

/*
 * Try to replace plan->wire[1] with an R20D payload.  encoded_top and
 * encoded_bottom are the encoder work areas; both have max_bytes capacity.
 */
void live_r20_compress(struct live_r20 *r20, struct row_pair_plan *plan,
		       struct row_anchor *base, uint64_t next_encode,
		       uint8_t *encoded_top, uint8_t *encoded_bottom,
		       size_t max_bytes);

#endif /* LIVE_R20_H */
