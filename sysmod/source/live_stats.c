/*
 * Formatting for live-stats.txt.  Pure file writer: all values come from the
 * snapshot, constants come from the codec headers.
 */
#include "live_stats.h"

#include "live_audio.h"
#include "live_audio_transport.h"
#include "live_pair.h"

#include <stdio.h>

#define U64(v) ((unsigned long long)(v))
#define U32(v) ((unsigned long)(v))

bool live_stats_write(const char *path, const struct live_stats *stats,
		      const char *network_text, size_t network_bytes)
{
	FILE *file;
	unsigned i;

	if (!path || !stats)
		return false;

	file = fopen(path, "w");
	if (!file)
		return false;

	if (network_text && network_bytes)
		fwrite(network_text, 1, network_bytes, file);

	if (stats->timing_present)
		fprintf(file, "timing_session=%08lx\ntiming_saved=%u\n",
			U32(stats->timing_session),
			stats->timing_saved ? 1u : 0u);

	fprintf(
	    file,
	    "version=%s\nsent_top=%lu\nsent_bottom=%lu\nreplaced=%lu\nsend_"
	    "errors=%lu\nrate=%lu\nfps=%lu\n",
	    stats->combined_fec ? "live-r10-fec4-candidate" : "live-r9-audio",
	    U32(stats->sent_top), U32(stats->sent_bottom), U32(stats->replaced),
	    U32(stats->send_errors), U32(stats->rate), U32(stats->fps));
	fprintf(
	    file,
	    "duration_ms=%llu\nexpired_pairs=%lu\nlast_send_errno=%d\nadaptive="
	    "0\npair_jpeg_budget=%lu\n",
	    U64(stats->duration_ms), U32(stats->expired_pairs),
	    stats->last_send_errno,
	    U32(live_pair_budget(
		live_av_video_rate(stats->rate, stats->audio_enabled),
		stats->fps)));
	fprintf(file, "payload_target_percent=%lu\n",
		U32(stats->payload_target_percent));
	fprintf(file,
		"r20_attempts=%lu\nr20_skipped_for_time=%lu\nr20_panels_queued="
		"%lu\nr20_bytes_saved_queued=%llu\nr20_encode_mean_us=%"
		"llu\nr20_encode_max_us=%lu\n",
		U32(stats->r20_attempts), U32(stats->r20_skipped),
		U32(stats->r20_panels), U64(stats->r20_bytes_saved),
		U64(stats->r20_attempts
			? stats->r20_encode_sum_us / stats->r20_attempts
			: 0),
		U32(stats->r20_encode_max_us));
	fprintf(
	    file,
	    "combined_fec=%u\npair_payload_budget=%lu\nquality_target=%lu\n",
	    stats->combined_fec ? 1u : 0u, U32(stats->pair_payload_budget),
	    U32(stats->quality_target));
	fprintf(file, "rate_pending_kept=%lu\nrate_encoding_kept=%lu\n",
		U32(stats->rate_pending_kept), U32(stats->rate_encoding_kept));
	fprintf(file,
		"pairs_attempted=%lu\npairs_oversize=%lu\nrequantize_passes=%"
		"lu\nquality=%lu\nfec_stripes=%u\nconfig_changes=%lu\n",
		U32(stats->pairs_attempted), U32(stats->pairs_oversize),
		U32(stats->requantize_passes), U32(stats->quality),
		stats->combined_fec ? LIVE_PAIR_PARITY : LIVE_PARITY,
		U32(stats->config_changes));
	fprintf(file,
		"manual_render_failures=%lu\nmanual_wire_budget_drops=%lu\n",
		U32(stats->manual_render_failures),
		U32(stats->manual_wire_budget_drops));
	fprintf(
	    file,
	    "repair_requests=%lu\nrepair_packets=%lu\nrepair_ipv4_bytes=%lu\n",
	    U32(stats->repair_requests), U32(stats->repair_packets),
	    U32(stats->repair_wire_bytes));
	fprintf(
	    file,
	    "repair_nack_cache_miss=%lu\nrepair_nack_stale=%lu\nrepair_"
	    "nack_duplicate=%lu\nrepair_nack_invalid=%lu\n"
	    "repair_fragments_expired=%lu\nrepair_fragments_evicted=%lu\n",
	    U32(stats->repair_nack_cache_miss), U32(stats->repair_nack_stale),
	    U32(stats->repair_nack_duplicate), U32(stats->repair_nack_invalid),
	    U32(stats->repair_fragments_expired),
	    U32(stats->repair_fragments_evicted));
	fprintf(file, "fec_scope=%s\nbudget_basis=%s\n",
		stats->combined_fec ? "pair" : "screen",
		stats->combined_fec ? "wire_payload" : "full_jpeg");
	fprintf(
	    file,
	    "send_calls=%lu\nsend_mean_us=%llu\nsend_max_us=%lu\nfeedback_"
	    "count=%lu\n",
	    U32(stats->send_calls),
	    U64(stats->send_calls ? stats->send_sum_us / stats->send_calls : 0),
	    U32(stats->send_max_us), U32(stats->feedback_count));
	fprintf(file, "video_wire_bytes=%llu\n", U64(stats->video_wire_bytes));
	fprintf(file, "send_timeout_ms=%u\n", LIVE_SEND_TIMEOUT_MS);
	fprintf(file,
		"source_layout=ycbcr420_mcu\nsnapshot_mean_us=%llu\nsnapshot_"
		"max_us=%lu\n",
		U64(stats->snapshot_count
			? stats->snapshot_sum_us / stats->snapshot_count
			: 0),
		U32(stats->snapshot_max_us));
	fprintf(file,
		"compact_jpeg=%u\ncompact_frames_queued=%lu\ncompact_bytes_"
		"saved_queued=%llu\ncompact_fallbacks=%lu\n",
		stats->compact_jpeg ? 1u : 0u, U32(stats->compact_frames),
		U64(stats->compact_saved_bytes), U32(stats->compact_fallbacks));
	fprintf(file,
		"row_keys=%lu\nrow_key_repeats=%lu\nrow_key_acks=%lu\nrow_"
		"delta_panels=%lu\nrow_bytes_saved_queued=%llu\n",
		U32(stats->row_keys), U32(stats->row_key_repeats),
		U32(stats->row_key_acks), U32(stats->row_delta_panels),
		U64(stats->row_bytes_saved));
	fprintf(
	    file,
	    "audio_enabled=%u\naudio_rate_mhz=%u\naudio_samples_per_block=%"
	    "u\naudio_blocks_queued=%lu\n"
	    "audio_queue_full=%lu\naudio_expired_blocks=%lu\naudio_packets_"
	    "sent=%lu\naudio_send_errors=%lu\n"
	    "audio_wire_bytes=%llu\naudio_encoded_blocks=%lu\naudio_encode_"
	    "mean_us=%llu\naudio_encode_max_us=%lu\n"
	    "audio_reserved_bytes_per_second=%lu\n",
	    stats->audio_enabled ? 1u : 0u, LIVE_AUDIO_RATE_MHZ,
	    LIVE_AUDIO_SAMPLES, U32(stats->audio_blocks_queued),
	    U32(stats->audio_blocks_dropped), U32(stats->audio_blocks_expired),
	    U32(stats->audio_packets_sent), U32(stats->audio_send_errors),
	    U64(stats->audio_wire_bytes), U32(stats->audio_encoded_blocks),
	    U64(stats->audio_encoded_blocks
		    ? stats->audio_encode_sum_us / stats->audio_encoded_blocks
		    : 0),
	    U32(stats->audio_encode_max_us),
	    U32(stats->audio_enabled ? LIVE_AUDIO_RESERVE_BYTES : 0u));

	for (i = 0; i < 2; i++) {
		const struct live_stats_screen *screen = &stats->screens[i];

		fprintf(
		    file,
		    "screen%u_attempts=%lu\nscreen%u_jpeg=%lu\nscreen%u_encode_"
		    "mean_us=%llu\nscreen%u_encode_max_us=%lu\n"
		    "screen%u_max_bytes=%lu\nscreen%u_encoder_core=%ld\nscreen%"
		    "u_changed_mcus=%llu\nscreen%u_source_reused=%lu\n"
		    "screen%u_dma_converted=%lu\nscreen%u_convert_mean_us=%"
		    "llu\nscreen%u_convert_max_us=%lu\nscreen%u_converted_mcus="
		    "%llu\n",
		    i, U32(screen->attempts), i, U32(screen->jpeg), i,
		    U64(screen->attempts
			    ? screen->encode_sum_us / screen->attempts
			    : 0),
		    i, U32(screen->encode_max_us), i, U32(screen->max_bytes), i,
		    (long)screen->encoder_core, i, U64(screen->changed_mcus), i,
		    U32(screen->source_reused), i, U32(screen->dma_converted),
		    i,
		    U64(screen->dma_converted
			    ? screen->convert_sum_us / screen->dma_converted
			    : 0),
		    i, U32(screen->convert_max_us), i,
		    U64(screen->converted_mcus));
	}

	return fclose(file) == 0;
}
