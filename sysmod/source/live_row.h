/*
 * Row-reuse reference banks and the encoder's per-pair row plan.
 *
 * A bank is writable only by the encoder at publication time.  The network
 * thread may acknowledge a bank, but never modifies its bytes.
 */
#ifndef LIVE_ROW_H
#define LIVE_ROW_H

#include "live_jpeg_rows.h"

#include <3ds.h>
#include <lz4.h>

/* Each optional reference bank holds up to 180 KiB. Larger pairs can still
 * travel as independent JPEGs within the per-screen and receive limits. */
#define ROW_PAIR_BYTES (180u * 1024u)

struct row_anchor {
	u32 seq;
	u32 stamp;
	u32 length[2];
	LiveJpegRows rows[2];
	_Alignas(LZ4_stream_t) u8 bytes[ROW_PAIR_BYTES];
};

struct row_pair_plan {
	u32 compact[2];
	u32 wire[2];
	u32 trial_size;
	u32 bottom_delta_bytes;
	const u8 *bottom_delta;
	const u8 *bottom_compressed;
	LiveJpegRows rows[2];
	bool valid;
};

static inline const u8 *row_anchor_panel(const struct row_anchor *anchor,
					 unsigned screen)
{
	return anchor->bytes + (screen ? anchor->length[0] : 0);
}

#endif /* LIVE_ROW_H */
