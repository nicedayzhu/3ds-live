#include "live_stream.h"
#include "live_stream_internal.h"
#include "live_config.h"
#include "live_net_report.h"
#include "live_audio_sender.h"
#include "live_control.h"
#include "live_stats.h"
#include "live_status.h"
#include "live_net.h"
#include "live_row.h"
#include "live_r20.h"
#include "live_repair_tx.h"
#include "buffers.h"
#include "capture.h"
#include "live_timing.h"
#include "pending_queue.h"
#include "packet_report.h"
#include "row_compression.h"
#include "live_jpeg.h"
#include "live_jpeg_source.h"
#include "live_jpeg_wire.h"
#include "live_jpeg_rows.h"
#include "live_row_refresh.h"
#include "live_pacing.h"
#include "live_pair.h"
#include "live_repair.h"
#include "live_audio_transport.h"
#include "live_discovery.h"
#include "live_receiver_feedback.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct live_stream stream = {
    .settings =
	{
	    .fps = 30,
	    .rate = 800000,
	    .quality = 60,
	    .screen_mask = 3,
	    .pacing_mode = 1,
	},
    .net = {.fd = -1},
};

#define SOC_ARENA_BYTES 0x100000u
/* Bit 0 = top screen, bit 1 = bottom screen. A single-screen mask skips the
 * hidden encoder entirely and sends per-screen packets, so both ARM11 time and
 * airtime are released instead of rendering a panel nobody displays. */
/* Socket-thread owned. Mode 0 is the original submission-start byte clock. */
/* Each optional reference bank holds up to 180 KiB. Larger
 * stream.counters.pairs can still travel as independent JPEGs within the
 * per-screen and receive limits.
 */
/* Only the encoder builds/reuses this dictionary. Acknowledging a new
 * reference changes stream.row.base, but never modifies either bank's bytes. */
_Static_assert(MAX_JPEG % _Alignof(LZ4_stream_t) == 0 &&
		   ROW_PAIR_BYTES % _Alignof(LZ4_stream_t) == 0,
	       "LZ4 workspaces at arena ends must be aligned");
bool live_stream_stopped(void)
{
	return __atomic_load_n(&stream.runtime.stopping, __ATOMIC_ACQUIRE);
}
bool live_stream_capture_paused(void)
{
	return __atomic_load_n(&stream.runtime.capture_paused,
			       __ATOMIC_ACQUIRE);
}

void live_stream_discard_pending(unsigned slot, LiveTimingOutcome why)
{
	(void)why;
	live_timing_drop_pending(slot, why);
	live_pending_remove(&stream.pending, slot);
}

static void clear_pending(void)
{
	while (stream.pending.count)
		live_stream_discard_pending(stream.pending.head,
					    TIMING_RECONFIGURED);
}

void live_set_capture_paused(bool paused)
{
	LightLock_Lock(&stream.runtime.lock);
	if (paused && !live_stream_capture_paused()) {
		// Reject a JPEG still being stream.buffers.encoded and forget
		// the previous display plan.
		++stream.settings.config_epoch;
		stream.runtime.plan_mask = 0;
		clear_pending();
	}
	__atomic_store_n(&stream.runtime.capture_paused, paused,
			 __ATOMIC_RELEASE);
	LightLock_Unlock(&stream.runtime.lock);
}
bool live_requested(void)
{
	struct live_config config;

	if (stream.runtime.worker)
		return stream.settings.enabled;

	if (!live_config_load(&config, LIVE_CONFIG_PATH))
		return false;

	stream.settings.enabled = config.enabled;
	stream.settings.compact_jpeg = config.compact_jpeg;
	stream.settings.audio_enabled = config.audio_enabled;
	stream.settings.combined_fec = config.combined_fec;
	stream.settings.fps = config.fps;
	stream.settings.rate = config.rate;

	memset(&stream.net.target, 0, sizeof(stream.net.target));
	stream.net.target.sin_family = AF_INET;
	stream.net.automatic_host = config.automatic_host;
	if (!stream.net.automatic_host &&
	    inet_pton(AF_INET, config.host, &stream.net.target.sin_addr) != 1)
		stream.settings.enabled = false;
	stream.net.target.sin_port = htons(config.port);

	if (!live_config_valid(&config))
		stream.settings.enabled = false;

	return stream.settings.enabled;
}

u32 live_fps(void)
{
	return __atomic_load_n(&stream.settings.fps, __ATOMIC_RELAXED);
}
bool live_peer_ready(void)
{
	return !live_stream_stopped() &&
	       __atomic_load_n(&stream.net.media_ready, __ATOMIC_ACQUIRE);
}
bool live_receiver_has_video(void)
{
	return !live_stream_stopped() &&
	       __atomic_load_n(&stream.runtime.receiver_video,
			       __ATOMIC_ACQUIRE);
}
u32 live_encoded_frames(unsigned sc)
{
	return sc < 2 ? __atomic_load_n(&stream.counters.jpeg_frames[sc],
					__ATOMIC_RELAXED)
		      : 0;
}
bool live_audio_active(void)
{
	return stream.settings.audio_enabled && stream.runtime.worker &&
	       !live_stream_stopped() && !live_stream_capture_paused();
}
bool live_audio_submit(const s16 *pcm, u32 pts)
{
	if (!pcm || !live_audio_active() || !live_peer_ready())
		return false;
	return live_audio_sender_push(&stream.audio, pcm, pts,
				      svcGetSystemTick());
}
void pacing_sleep_until(u64 due)
{
	u64 start = svcGetSystemTick();
	while (start < due) {
		svcSleepThread(
		    ((due - start) * 1000000000ull + SYSCLOCK_ARM11 - 1) /
		    SYSCLOCK_ARM11);
		u64 end = svcGetSystemTick();
		live_packet_observe_sleep(&stream.runtime.packet_report, start,
					  due, end);
		start = end;
	}
}

void pacing_charge(u64 *due, u64 begin, u64 returned, u32 wire,
		   enum LivePacketKind kind)
{
	u64 base = begin > *due ? begin : *due;
	u64 byteDue = live_packet_due(*due, begin, wire, stream.settings.rate,
				      SYSCLOCK_ARM11);
	*due =
	    live_packet_return_due(byteDue, returned, byteDue - base,
				   SYSCLOCK_ARM11, stream.settings.pacing_mode);
	live_packet_observe(&stream.runtime.packet_report, kind, begin,
			    returned, *due - byteDue, SYSCLOCK_ARM11);
}

/* Only the network thread calls this. Audio is serviced between video
 * datagrams, not after a whole 150 ms video send. Previous-batch redundancy
 * repairs one lost stream.audio datagram without a retransmission round trip.
 */
static void audio_pacing_charge(u64 *due, u64 begin, u64 returned, u32 wire,
				int kind)
{
	pacing_charge(due, begin, returned, wire, (enum LivePacketKind)kind);
}

void send_audio(u64 *due)
{
	live_audio_sender_send(
	    &stream.audio, stream.runtime.session,
	    stream.settings.audio_enabled, live_stream_stopped(), stream.net.fd,
	    &stream.net.target, due, pacing_sleep_until, audio_pacing_charge);
}

static size_t network_report_text(char *text, size_t capacity)
{
	struct live_net_report snapshot;

	live_net_snapshot(&stream.net, stream.runtime.session, &snapshot);
	return live_net_report_format(text, capacity, &snapshot);
}

/* The socket stream.runtime.worker owns the live counters; publish one complete
 * copy. */
void queue_network_report(void)
{
	struct live_net_report snapshot;

	live_net_snapshot(&stream.net, stream.runtime.session, &snapshot);
	live_net_report_queue(&snapshot);
	stream.net.diag.saved_at = svcGetSystemTick();
}

void live_flush_network_report(bool force)
{
	live_net_report_flush(force);
}

void reset_receiver_feedback(void)
{
	stream.runtime.receiver_feedback = (LiveReceiverFeedback){0};
	__atomic_store_n(&stream.runtime.receiver_video, false,
			 __ATOMIC_RELEASE);
}

void row_peer(bool enable, bool forget)
{
	/* Called with stream.runtime.lock held. An encode begun under the
	 * previous negotiation must not publish a delta after a peer forgets
	 * its base. */
	if (enable == stream.row.enabled && !(forget && stream.row.base))
		return;
	stream.row.enabled = enable;
	stream.row.base = stream.row.candidate = stream.row.probe = NULL;
	stream.settings.config_epoch++;
	clear_pending();
}

void reset_peer_config(void)
{
	stream.settings.pacing_mode = 1;
	stream.runtime.packet_report.flags = 0;
	live_repair_clear(&stream.repair_history);
	stream.runtime.repair_interest = stream.runtime.repair_peer_at =
	    stream.repair_tx.credit = 0;
	stream.control.deferred_size = 0;
	stream.net.last_peer_control = 0;
	stream.row.last_ack = 0;
	stream.settings.config_sequence = 0;
	LightLock_Lock(&stream.runtime.lock);
	stream.r20.last_ack = 0;
	if (stream.row.enabled)
		row_peer(false, false);
	else {
		++stream.settings.config_epoch;
		clear_pending();
	}
	LightLock_Unlock(&stream.runtime.lock);
}

static void stream_reset(void)
{
	stream.runtime.packet_report = (LivePacketReport){0};
	__atomic_store_n(&stream.runtime.capture_paused, false,
			 __ATOMIC_RELEASE);
	memset(stream.runtime.raw_valid, 0, sizeof stream.runtime.raw_valid);
	memset(stream.counters.attempts, 0, sizeof stream.counters.attempts);
	memset(stream.counters.jpeg_frames, 0,
	       sizeof stream.counters.jpeg_frames);
	memset(stream.counters.max_us, 0, sizeof stream.counters.max_us);
	memset(stream.counters.max_bytes, 0, sizeof stream.counters.max_bytes);
	memset(stream.counters.sum_us, 0, sizeof stream.counters.sum_us);
	memset(stream.counters.convert_count, 0,
	       sizeof stream.counters.convert_count);
	memset(stream.counters.convert_max_us, 0,
	       sizeof stream.counters.convert_max_us);
	memset(stream.counters.convert_sum_us, 0,
	       sizeof stream.counters.convert_sum_us);
	memset(stream.counters.converted_mcus, 0,
	       sizeof stream.counters.converted_mcus);
	stream.counters.snapshot_count = stream.counters.snapshot_max_us = 0;
	stream.counters.snapshot_sum_us = 0;
	memset(stream.counters.changed_mcus, 0,
	       sizeof stream.counters.changed_mcus);
	memset(stream.counters.source_reuse, 0,
	       sizeof stream.counters.source_reuse);
	memset(stream.counters.last_source, 0,
	       sizeof stream.counters.last_source);
	memset(stream.counters.frames, 0, sizeof stream.counters.frames);
	memset(stream.counters.last_encode_us, 0,
	       sizeof stream.counters.last_encode_us);
	stream.runtime.sequence = stream.counters.replaced =
	    stream.counters.errors = stream.counters.expired =
		stream.counters.oversize = stream.counters.retries =
		    stream.counters.pairs = stream.counters.send_calls =
			stream.counters.send_max_us =
			    stream.counters.feedback_count = 0;
	stream.settings.config_epoch = stream.settings.config_sequence =
	    stream.counters.config_changes = stream.counters.rate_pending_kept =
		stream.counters.rate_encoding_kept = 0;
	stream.counters.send_sum_us = 0;
	stream.runtime.last_errno = 0;
	stream.settings.quality_target = 0;
	stream.settings.quality = 60;
	stream.settings.spare = 0;
	stream.settings.screen_mask = 3;
	stream.runtime.payload_control = (LivePayloadControl){.percent = 100};
	stream.counters.compact_saved_bytes = 0;
	stream.counters.compact_frames = stream.counters.compact_fallbacks = 0;
	stream.row.enabled = false;
	stream.row.base = stream.row.candidate = stream.row.probe =
	    stream.row.banks = NULL;
	live_r20_init(&stream.r20);
	stream.row.keys = stream.row.repeats = stream.row.deltas =
	    stream.row.acks = stream.row.sends = 0;
	stream.counters.manual_render_failures =
	    stream.counters.manual_wire_drops = 0;
	stream.repair_history = (LiveRepairHistory){0};
	stream.runtime.repair_requests = 0;
	stream.repair_tx = (struct live_repair_tx){0};
	stream.control.deferred_size = 0;
	stream.runtime.last_repair_poll = stream.runtime.repair_interest =
	    stream.runtime.repair_peer_at = stream.runtime.send_active_ticks =
		0;
	stream.row.probe_after = 0;
	stream.row.last_key = stream.row.last_ack = stream.row.bytes_saved = 0;
	live_audio_sender_init(&stream.audio);
	stream.counters.video_wire_bytes = 0;
	stream.runtime.session = (u32)stream.runtime.started;
	if (!stream.runtime.session)
		stream.runtime.session = 1;
	stream.runtime.last_status_tick = stream.runtime.last_control_tick =
	    stream.runtime.started;
	stream.net.discovery =
	    (LiveDiscovery){.challenge = (u32)(stream.runtime.started >> 16)};
	reset_receiver_feedback();
	stream.net.last_probe_tick = stream.net.last_peer_control = 0;
	stream.net.local_address = stream.net.local_mask = 0;
	memset(&stream.net.diag, 0, sizeof(stream.net.diag));
	__atomic_store_n(&stream.net.media_ready, false, __ATOMIC_RELEASE);
	live_net_report_init();
	queue_network_report();
	live_flush_network_report(true);
}

static int stream_open_resources(void *(*allocate)(u32 bytes))
{
	for (unsigned i = 0; i < 2; i++) {
		live_jpeg_reset(i);
		stream.buffers.raw_bytes[i] =
		    live_jpeg_frame_size(i ? 320 : 400);
		size_t sourceBytes =
		    live_jpeg_source_storage_size(i ? 320 : 400);
		void *arena =
		    allocate((u32)(sourceBytes + stream.buffers.raw_bytes[i]));
		stream.buffers.raw_sources[i] =
		    live_jpeg_source_init(arena, sourceBytes, i ? 320 : 400);
		stream.buffers.raw_working[i] =
		    arena ? (LiveJpegFrame *)((u8 *)arena + sourceBytes) : NULL;
		if (!stream.buffers.raw_sources[i] ||
		    !stream.buffers.raw_working[i]) {
			live_stop();
			return -7;
		}
	}
	u32 *socStorage = allocate(SOC_ARENA_BYTES);
	if (!socStorage) {
		live_stop();
		return -7;
	}
	Result rc = socInit(socStorage, SOC_ARENA_BYTES);
	if (R_FAILED(rc)) {
		live_stop();
		return rc;
	}
	stream.runtime.soc_ready = true;
	/* DMA buffers are mapped later when the LCD modes become known.
	 * Leave room for both at maximum stride before taking any
	 * optional memory. */
	const u32 rowPages = (2 * sizeof(*stream.row.banks) + 4095u) & ~4095u;
	if (stream.settings.compact_jpeg &&
	    live_pages_available(rowPages + LIVE_CAPTURE_MAX_DMA_BYTES))
		stream.row.banks = allocate(rowPages);
	for (int i = 1; i >= 0; i--) {
		stream.runtime.encoder_cores[i] = i + 2;
		stream.runtime.encoders[i] =
		    threadCreate(encoder, (void *)(uintptr_t)i, 8192, 0x3E,
				 stream.runtime.encoder_cores[i], false);
		if (!stream.runtime.encoders[i]) {
			stream.runtime.encoder_cores[i] = 1;
			stream.runtime.encoders[i] =
			    threadCreate(encoder, (void *)(uintptr_t)i, 8192,
					 0x3E, 1, false);
		}
		if (!stream.runtime.encoders[i]) {
			live_stop();
			return -6;
		}
	}
	stream.runtime.worker =
	    threadCreate(sender, NULL, 16384, 0x3E, 1, false);
	if (!stream.runtime.worker) {
		live_stop();
		return -5;
	}
	return 0;
}

Result live_start(void *(*allocate)(u32 bytes))
{
	if (!allocate || stream.runtime.worker || stream.runtime.encoders[0] ||
	    stream.runtime.encoders[1])
		return -1;
	LightLock_Init(&stream.runtime.lock);
	for (unsigned i = 0; i < 2; i++)
		LightLock_Init(&stream.runtime.raw_locks[i]);
	for (unsigned i = 0; i < 3; i++)
		LightEvent_Init(&stream.runtime.events[i], RESET_ONESHOT);
	stream.runtime.started = svcGetSystemTick();
	stream.runtime.stopping = false;
	stream.runtime.job_done = 1;
	stream.pending = (LivePendingQueue){0};
	stream.runtime.plan_mask = 0;
	stream.settings.pacing_mode = 1;
	stream_reset();
	return stream_open_resources(allocate);
}
static void stream_write_stats(bool timing_saved)
{
	char network_text[LIVE_NET_REPORT_BYTES];
	size_t network_bytes;
	struct live_stats stats = {
	    .combined_fec = stream.settings.combined_fec,
	    .sent_top = stream.counters.frames[0],
	    .sent_bottom = stream.counters.frames[1],
	    .replaced = stream.counters.replaced,
	    .send_errors = stream.counters.errors,
	    .rate = stream.settings.rate,
	    .fps = stream.settings.fps,
	    .duration_ms = (svcGetSystemTick() - stream.runtime.started) *
			   1000 / SYSCLOCK_ARM11,
	    .expired_pairs = stream.counters.expired,
	    .last_send_errno = stream.runtime.last_errno,
	    .payload_target_percent = stream.runtime.payload_control.percent,
	    .quality_target = stream.settings.quality_target,
	    .pair_payload_budget = live_stream_pair_budget(
		stream.settings.rate, stream.settings.fps,
		stream.settings.combined_fec, stream.settings.quality_target),
	    .rate_pending_kept = stream.counters.rate_pending_kept,
	    .rate_encoding_kept = stream.counters.rate_encoding_kept,
	    .pairs_attempted = stream.counters.pairs,
	    .pairs_oversize = stream.counters.oversize,
	    .requantize_passes = stream.counters.retries,
	    .quality = stream.settings.quality,
	    .config_changes = stream.counters.config_changes,
	    .manual_render_failures = stream.counters.manual_render_failures,
	    .manual_wire_budget_drops = stream.counters.manual_wire_drops,
	    .repair_requests = stream.runtime.repair_requests,
	    .repair_packets = stream.repair_tx.packets,
	    .repair_wire_bytes = stream.repair_tx.wire_bytes,
	    .repair_nack_cache_miss = stream.repair_history.missing,
	    .repair_nack_stale = stream.repair_history.stale,
	    .repair_nack_duplicate = stream.repair_history.duplicate,
	    .repair_nack_invalid = stream.repair_history.invalid,
	    .repair_fragments_expired = stream.repair_history.expired_fragments,
	    .repair_fragments_evicted = stream.repair_history.evicted_fragments,
	    .send_calls = stream.counters.send_calls,
	    .send_sum_us = stream.counters.send_sum_us,
	    .send_max_us = stream.counters.send_max_us,
	    .feedback_count = stream.counters.feedback_count,
	    .video_wire_bytes = stream.counters.video_wire_bytes,
	    .snapshot_count = stream.counters.snapshot_count,
	    .snapshot_sum_us = stream.counters.snapshot_sum_us,
	    .snapshot_max_us = stream.counters.snapshot_max_us,
	    .compact_jpeg = stream.settings.compact_jpeg,
	    .compact_frames = stream.counters.compact_frames,
	    .compact_saved_bytes = stream.counters.compact_saved_bytes,
	    .compact_fallbacks = stream.counters.compact_fallbacks,
	    .row_keys = stream.row.keys,
	    .row_key_repeats = stream.row.repeats,
	    .row_key_acks = stream.row.acks,
	    .row_delta_panels = stream.row.deltas,
	    .row_bytes_saved = stream.row.bytes_saved,
	    .r20_attempts = stream.r20.attempts,
	    .r20_skipped = stream.r20.skipped,
	    .r20_panels = stream.r20.panels,
	    .r20_bytes_saved = stream.r20.saved,
	    .r20_encode_sum_us = stream.r20.encode_sum_us,
	    .r20_encode_max_us = stream.r20.max_us,
	    .audio_enabled = stream.settings.audio_enabled,
	    .audio_blocks_queued = stream.audio.queue.queued,
	    .audio_blocks_dropped = stream.audio.queue.dropped,
	    .audio_blocks_expired = stream.audio.queue.expired,
	    .audio_packets_sent = stream.audio.packets_sent,
	    .audio_send_errors = stream.audio.send_errors,
	    .audio_wire_bytes = stream.audio.wire_bytes,
	    .audio_encoded_blocks = stream.audio.encoded_blocks,
	    .audio_encode_sum_us = stream.audio.encode_sum_us,
	    .audio_encode_max_us = stream.audio.encode_max_us,
	};
	for (unsigned i = 0; i < 2; i++) {
		stats.screens[i] = (struct live_stats_screen){
		    .attempts = stream.counters.attempts[i],
		    .jpeg = stream.counters.jpeg_frames[i],
		    .encode_sum_us = stream.counters.sum_us[i],
		    .encode_max_us = stream.counters.max_us[i],
		    .max_bytes = stream.counters.max_bytes[i],
		    .encoder_core = stream.runtime.encoder_cores[i],
		    .changed_mcus = stream.counters.changed_mcus[i],
		    .source_reused = stream.counters.source_reuse[i],
		    .dma_converted = stream.counters.convert_count[i],
		    .convert_sum_us = stream.counters.convert_sum_us[i],
		    .convert_max_us = stream.counters.convert_max_us[i],
		    .converted_mcus = stream.counters.converted_mcus[i],
		};
	}
#ifdef LIVE_FRAME_TIMING
	stats.timing_present = true;
	stats.timing_session = stream.runtime.session;
	stats.timing_saved = timingSaved;
#endif
	network_bytes = network_report_text(network_text, sizeof(network_text));
	live_stats_write("sdmc:/3ds/3ds-live/live-stats.txt", &stats,
			 network_text, network_bytes);
#ifndef LIVE_FRAME_TIMING
	(void)timing_saved;
#endif
}

static void stream_join_threads(void)
{
	for (unsigned i = 0; i < 2; i++) {
		if (!stream.runtime.encoders[i])
			continue;
		threadJoin(stream.runtime.encoders[i], U64_MAX);
		threadFree(stream.runtime.encoders[i]);
		stream.runtime.encoders[i] = NULL;
	}
	if (stream.runtime.worker) {
		threadJoin(stream.runtime.worker, U64_MAX);
		threadFree(stream.runtime.worker);
		stream.runtime.worker = NULL;
	}
}

static void stream_close_resources(void)
{
	queue_network_report();
	stream.row.base = stream.row.candidate = stream.row.probe =
	    stream.row.banks = NULL;
	live_net_close(&stream.net);
	if (stream.runtime.soc_ready) {
		socExit();
		stream.runtime.soc_ready = false;
	}
	for (unsigned i = 0; i < 2; i++) {
		live_jpeg_reset(i);
		live_jpeg_source_destroy(stream.buffers.raw_sources[i]);
		stream.buffers.raw_sources[i] = NULL;
		stream.buffers.raw_working[i] =
		    NULL; /* Caller releases the SYSTEM arena. */
	}
}

void live_stop(void)
{
	__atomic_store_n(&stream.runtime.stopping, true, __ATOMIC_RELEASE);
	for (unsigned i = 0; i < 3; i++)
		LightEvent_Signal(&stream.runtime.events[i]);
	stream_join_threads();
#ifdef LIVE_FRAME_TIMING
	bool timingSaved =
	    live_timing_export(stream.runtime.session, stream.runtime.started);
#endif
	stream_close_resources();
#ifdef LIVE_FRAME_TIMING
	stream_write_stats(timingSaved);
#else
	stream_write_stats(false);
#endif
}
bool live_submit(int sc, const u8 *frame, u32 fmt, u32 stride, u32 pts,
		 bool wide)
{
	if (sc < 0 || sc > 1 || !stream.runtime.worker ||
	    live_stream_stopped() || live_stream_capture_paused() || wide)
		return false;
	u64 begin = svcGetSystemTick();
	u32 w = sc ? 320 : 400;
	/* Exact native-source comparison precedes color conversion.
	 * Only changed MCUs enter the compact YCbCr420 snapshot; no
	 * RGB565 staging frame. */
	LightLock_Lock(&stream.runtime.raw_locks[sc]);
	int converted =
	    live_jpeg_source_update(stream.buffers.raw_sources[sc], frame,
				    (size_t)w * stride, fmt, stride);
	if (converted >= 0) {
		stream.runtime.raw_pts[sc] = pts;
		stream.runtime.raw_valid[sc] = true;
	}
	LightLock_Unlock(&stream.runtime.raw_locks[sc]);
	if (converted < 0)
		return false;
	stream.counters.converted_mcus[sc] += (unsigned)converted;
	LightEvent_Signal(&stream.runtime.events[0]);
	u32 us = (u32)((svcGetSystemTick() - begin) * 1000000 / SYSCLOCK_ARM11);
	stream.counters.convert_count[sc]++;
	stream.counters.convert_sum_us[sc] += us;
	if (us > stream.counters.convert_max_us[sc])
		stream.counters.convert_max_us[sc] = us;
	return true;
}
void live_schedule(u32 stamp, u32 top, u32 bottom, u32 mask)
{
	if (!stream.runtime.worker || live_stream_stopped())
		return;
	LightLock_Lock(&stream.runtime.lock);
	stream.runtime.plan_stamp = stamp;
	stream.runtime.plan_pts[0] = top;
	stream.runtime.plan_pts[1] = bottom;
	stream.runtime.plan_mask = mask;
	LightLock_Unlock(&stream.runtime.lock);
}
