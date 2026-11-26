/*
 * R7ST/R9ST status packet builder.
 *
 * The packet is a fixed little-endian array of counters.  Keeping the layout
 * here makes the wire format testable and keeps control() focused on timing
 * and ancillary packets.
 */
#ifndef LIVE_STATUS_H
#define LIVE_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct live_status_main {
	uint32_t session;
	uint32_t duration_ms;
	uint32_t rate;
	uint32_t video_rate;
	uint32_t fps;
	uint32_t encoded_top;
	uint32_t encoded_bottom;
	uint32_t sent_top;
	uint32_t sent_bottom;
	uint32_t send_errors;
	uint32_t expired_pairs;
	uint32_t feedback_count;
	int32_t encoder_core_top;
	int32_t encoder_core_bottom;
	uint32_t last_encode_us_top;
	uint32_t last_encode_us_bottom;
	bool audio_enabled;
	uint32_t audio_blocks_queued;
	uint32_t audio_blocks_dropped;
	uint32_t audio_packets_sent;
	uint32_t audio_queue_depth;
};

/* Returns the packet length (76 or 108), or 0 when the buffer is too small. */
size_t live_status_main_build(uint8_t *buffer, size_t capacity,
			      const struct live_status_main *status);

#endif /* LIVE_STATUS_H */
