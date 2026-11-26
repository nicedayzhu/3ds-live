/*
 * live-stats.txt writer.
 *
 * The streaming threads keep counters in their own modules; live_stop()
 * copies them into one snapshot so this writer has no dependency on socket,
 * encoder or thread state.
 */
#ifndef LIVE_STATS_H
#define LIVE_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct live_stats_screen {
	uint32_t attempts;
	uint32_t jpeg;
	uint64_t encode_sum_us;
	uint32_t encode_max_us;
	uint32_t max_bytes;
	int32_t encoder_core;
	uint64_t changed_mcus;
	uint32_t source_reused;
	uint32_t dma_converted;
	uint64_t convert_sum_us;
	uint32_t convert_max_us;
	uint64_t converted_mcus;
};

struct live_stats {
	bool combined_fec;
	uint32_t sent_top;
	uint32_t sent_bottom;
	uint32_t replaced;
	uint32_t send_errors;
	uint32_t rate;
	uint32_t fps;
	uint64_t duration_ms;
	uint32_t expired_pairs;
	int last_send_errno;
	uint32_t payload_target_percent;
	uint32_t quality_target;
	uint32_t pair_payload_budget;
	uint32_t rate_pending_kept;
	uint32_t rate_encoding_kept;
	uint32_t pairs_attempted;
	uint32_t pairs_oversize;
	uint32_t requantize_passes;
	uint32_t quality;
	uint32_t config_changes;
	uint32_t manual_render_failures;
	uint32_t manual_wire_budget_drops;

	uint32_t repair_requests;
	uint32_t repair_packets;
	uint32_t repair_wire_bytes;
	uint32_t repair_nack_cache_miss;
	uint32_t repair_nack_stale;
	uint32_t repair_nack_duplicate;
	uint32_t repair_nack_invalid;
	uint32_t repair_fragments_expired;
	uint32_t repair_fragments_evicted;

	uint32_t send_calls;
	uint64_t send_sum_us;
	uint32_t send_max_us;
	uint32_t feedback_count;
	uint64_t video_wire_bytes;
	uint32_t snapshot_count;
	uint64_t snapshot_sum_us;
	uint32_t snapshot_max_us;

	bool compact_jpeg;
	uint32_t compact_frames;
	uint64_t compact_saved_bytes;
	uint32_t compact_fallbacks;
	uint32_t row_keys;
	uint32_t row_key_repeats;
	uint32_t row_key_acks;
	uint32_t row_delta_panels;
	uint64_t row_bytes_saved;

	uint32_t r20_attempts;
	uint32_t r20_skipped;
	uint32_t r20_panels;
	uint64_t r20_bytes_saved;
	uint64_t r20_encode_sum_us;
	uint32_t r20_encode_max_us;

	bool audio_enabled;
	uint32_t audio_blocks_queued;
	uint32_t audio_blocks_dropped;
	uint32_t audio_blocks_expired;
	uint32_t audio_packets_sent;
	uint32_t audio_send_errors;
	uint64_t audio_wire_bytes;
	uint32_t audio_encoded_blocks;
	uint64_t audio_encode_sum_us;
	uint32_t audio_encode_max_us;

	bool timing_present;
	uint32_t timing_session;
	bool timing_saved;

	struct live_stats_screen screens[2];
};

/*
 * Write one complete live-stats.txt.  network_text/network_bytes are the
 * already formatted network-status block that historically prefixes the file.
 * Returns false when the file cannot be opened or written.
 */
bool live_stats_write(const char *path, const struct live_stats *stats,
		      const char *network_text, size_t network_bytes);

#endif /* LIVE_STATS_H */
