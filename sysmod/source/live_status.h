/*
 * R7ST/R9ST status packets.
 *
 * The status burst is a small, fixed sequence of little-endian packets.  The
 * layout lives here; control() decides when the burst is due and how each
 * descriptor is paced onto the wire.
 */
#ifndef LIVE_STATUS_H
#define LIVE_STATUS_H

#include "packet_report.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIVE_STATUS_MAX_PACKETS 12

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

struct live_status_aux {
	uint32_t session;
	uint32_t duration_ms;
	uint32_t quality_target;
	uint32_t overwritten;
	uint32_t row_repeats;
	uint32_t repair_requests;
	uint32_t repair_packets;
	uint32_t repair_wire_bytes;
	uint32_t audio_dropped;
	uint32_t audio_expired;
	uint32_t send_active_us;
	uint32_t pacing_mode;
	bool compact_jpeg;
	bool rows_enabled;
	bool combined_fec;
	uint64_t audio_wire_bytes;
	uint64_t video_wire_bytes;
	uint32_t pacing_words[LIVE_PACING_WORDS];
};

struct live_status_packet {
	uint16_t offset;
	uint16_t size;
};

/* Build the main packet alone (used by the host test). */
size_t live_status_main_build(uint8_t *buffer, size_t capacity,
			      const struct live_status_main *status);

/* Returns the number of packets written to descriptors, or 0 on error. */
unsigned live_status_build_burst(uint8_t *buffer, size_t capacity,
				 const struct live_status_main *main_status,
				 const struct live_status_aux *aux,
				 struct live_status_packet *packets,
				 unsigned max_packets);

#endif /* LIVE_STATUS_H */
