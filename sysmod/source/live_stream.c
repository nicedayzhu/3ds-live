#include "live_stream.h"
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

#define MAX_JPEG LIVE_JPEG_MAX_BYTES

/* Encoder 0 coordinates stream.counters.pairs, encoder 1 handles bottom. The
 * socket stream.runtime.worker owns the socket and claims queued
 * stream.counters.pairs in capture order. */
struct stream_counters {
	u32 attempts[2], jpeg_frames[2], max_us[2], max_bytes[2];
	u32 convert_count[2], convert_max_us[2];
	u64 sum_us[2], convert_sum_us[2], changed_mcus[2], converted_mcus[2];
	u64 snapshot_sum_us;
	u32 snapshot_count, snapshot_max_us;
	u32 source_reuse[2], last_source[2], frames[2];
	u32 replaced, errors, expired, oversize, retries, pairs;
	u32 send_calls, send_max_us, last_encode_us[2];
	u32 feedback_count, config_changes, rate_pending_kept,
	    rate_encoding_kept;
	u64 send_sum_us, video_wire_bytes, compact_saved_bytes;
	u32 compact_frames, compact_fallbacks;
	u32 manual_render_failures, manual_wire_drops;
};
struct stream_settings {
	bool enabled;
	bool compact_jpeg;
	bool audio_enabled;
	bool combined_fec;
	u32 fps;
	u32 rate;
	u32 config_epoch;
	u32 config_sequence;
	unsigned quality_target;
	unsigned quality;
	unsigned spare;
	u32 screen_mask;
	unsigned pacing_mode;
};
struct stream_buffers {
	LiveJpegSource *raw_sources[2];
	LiveJpegFrame *raw_working[2];
	size_t raw_bytes[2];
	_Alignas(LZ4_stream_t) u8 encoded[2][MAX_JPEG];
	u8 pending_bytes[2 * MAX_JPEG];
	u8 send_arena[2 * MAX_JPEG];
};

struct stream_runtime {
	bool raw_valid[2];
	bool soc_ready;
	bool receiver_video;
	bool capture_paused;
	volatile bool stopping;
	u32 raw_pts[2];
	u32 plan_pts[2];
	u32 plan_stamp;
	u32 plan_mask;
	u32 sequence;
	u32 session;
	u64 started;
	LightLock lock;
	LightLock raw_locks[2];
	LightEvent events[3];
	Thread worker;
	Thread encoders[2];
	s32 encoder_cores[2];
	u32 job_kind;
	u32 job_quality;
	u32 job_done;
	u32 job_size[2];
	u32 job_changed[2];
	u32 job_us[2];
	LivePayloadControl payload_control;
	LivePacketReport packet_report;
	u64 last_status_tick;
	u64 last_control_tick;
	u64 send_active_ticks;
	u32 repair_requests;
	u64 repair_interest;
	u64 repair_peer_at;
	u64 last_repair_poll;
	LiveReceiverFeedback receiver_feedback;
	int last_errno;
};

struct row_state {
	struct row_anchor *banks;
	struct row_anchor *base;
	struct row_anchor *candidate;
	struct row_anchor *probe;
	LiveRowTrial trial;
	u64 probe_after;
	bool enabled;
	unsigned sends;
	u64 last_key;
	u64 last_ack;
	u64 bytes_saved;
	u32 keys;
	u32 repeats;
	u32 deltas;
	u32 acks;
};

struct control_state {
	int deferred_size;
	struct sockaddr_in deferred_from;
	u8 deferred_bytes[64];
};

struct live_stream {
	struct stream_buffers buffers;
	struct row_state row;
	struct control_state control;
	struct stream_runtime runtime;
	struct stream_settings settings;
	struct stream_counters counters;
	struct live_net net;
	struct live_audio_sender audio;
	struct live_r20 r20;
	struct live_repair_tx repair_tx;
	LiveRepairHistory repair_history;
	LivePendingQueue pending;
};

static struct live_stream stream = {
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
static u32 get32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static void put32(u8 *p, u32 n)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (u8)(n >> (8 * i));
}
static bool stopped(void)
{
	return __atomic_load_n(&stream.runtime.stopping, __ATOMIC_ACQUIRE);
}
static bool capture_paused(void)
{
	return __atomic_load_n(&stream.runtime.capture_paused,
			       __ATOMIC_ACQUIRE);
}

static void discard_pending(unsigned slot, LiveTimingOutcome why)
{
	(void)why;
	live_timing_drop_pending(slot, why);
	live_pending_remove(&stream.pending, slot);
}

static void clear_pending(void)
{
	while (stream.pending.count)
		discard_pending(stream.pending.head, TIMING_RECONFIGURED);
}

void live_set_capture_paused(bool paused)
{
	LightLock_Lock(&stream.runtime.lock);
	if (paused && !capture_paused()) {
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
	return !stopped() &&
	       __atomic_load_n(&stream.net.media_ready, __ATOMIC_ACQUIRE);
}
bool live_receiver_has_video(void)
{
	return !stopped() && __atomic_load_n(&stream.runtime.receiver_video,
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
	       !stopped() && !capture_paused();
}
bool live_audio_submit(const s16 *pcm, u32 pts)
{
	if (!pcm || !live_audio_active() || !live_peer_ready())
		return false;
	return live_audio_sender_push(&stream.audio, pcm, pts,
				      svcGetSystemTick());
}
static void encode_job(unsigned sc, unsigned kind, unsigned q)
{
	u64 begin = svcGetSystemTick();
	if (kind == 1) {
		int changed =
		    live_jpeg_prepare_yuv(sc, stream.buffers.raw_working[sc],
					  stream.buffers.raw_bytes[sc]);
		if (changed < 0) {
			stream.runtime.job_changed[sc] = 0;
			stream.runtime.job_size[sc] = 0;
			return;
		}
		stream.runtime.job_changed[sc] = (u32)changed;
	}
	stream.runtime.job_size[sc] =
	    (u32)live_jpeg_render(sc, q, stream.buffers.encoded[sc], MAX_JPEG);
	stream.runtime.job_us[sc] +=
	    (u32)((svcGetSystemTick() - begin) * 1000000 / SYSCLOCK_ARM11);
}
static bool run_single_job(unsigned kind, unsigned q, unsigned sc)
{
	stream.runtime.job_kind = kind;
	stream.runtime.job_quality = q;
	if (sc == 0) {
		encode_job(0, kind, q);
		return !stopped();
	}
	__atomic_store_n(&stream.runtime.job_done, 0, __ATOMIC_RELEASE);
	LightEvent_Signal(&stream.runtime.events[1]);
	while (!__atomic_load_n(&stream.runtime.job_done, __ATOMIC_ACQUIRE) &&
	       !stopped())
		LightEvent_WaitTimeout(&stream.runtime.events[2], 1000000);
	return !stopped();
}

static bool run_pair_job(unsigned kind, unsigned q)
{
	stream.runtime.job_kind = kind;
	stream.runtime.job_quality = q;
	__atomic_store_n(&stream.runtime.job_done, 0, __ATOMIC_RELEASE);
	LightEvent_Signal(&stream.runtime.events[1]);
	encode_job(0, kind, q);
	while (!__atomic_load_n(&stream.runtime.job_done, __ATOMIC_ACQUIRE) &&
	       !stopped())
		LightEvent_WaitTimeout(&stream.runtime.events[2], 1000000);
	return !stopped();
}
static u32 payload_size(unsigned screen, unsigned q, bool combined)
{
	u32 size = stream.runtime.job_size[screen];
	/* Budget exactly what packing will send, including a full-JPEG
	 * fallback. */
	if (combined && stream.settings.compact_jpeg &&
	    live_jpeg_wire_size(size) &&
	    stream.buffers.encoded[screen][size - 2] == 0xff &&
	    stream.buffers.encoded[screen][size - 1] == 0xd9 &&
	    live_jpeg_wire_prefix_valid(stream.buffers.encoded[screen],
					screen ? 320 : 400, q))
		return (u32)live_jpeg_wire_size(size);
	return size ? size : MAX_JPEG + 1;
}
static u32 pack_panel(unsigned sc, unsigned q, u8 *out, u32 capacity)
{
	size_t packed =
	    stream.settings.compact_jpeg
		? live_jpeg_wire_pack(out, capacity, stream.buffers.encoded[sc],
				      stream.runtime.job_size[sc],
				      sc ? 320 : 400, q)
		: 0;
	if (packed) {
		stream.counters.compact_frames++;
		stream.counters.compact_saved_bytes +=
		    stream.runtime.job_size[sc] - packed;
		return (u32)packed;
	}
	memcpy(out, stream.buffers.encoded[sc], stream.runtime.job_size[sc]);
	if (stream.settings.compact_jpeg)
		stream.counters.compact_fallbacks++;
	return stream.runtime.job_size[sc];
}

static u32 row_size(const struct row_pair_plan *plan, unsigned sc,
		    const struct row_anchor *ref)
{
	size_t delta = live_jpeg_rows_pack(
	    NULL, 0, stream.buffers.encoded[sc], plan->compact[sc],
	    &plan->rows[sc], row_anchor_panel(ref, sc), ref->length[sc],
	    &ref->rows[sc], ref->seq);
	return delta ? (u32)delta : plan->compact[sc];
}

/* Only the encoder writes the two anchor banks, at publication. The network
 * thread can acknowledge a bank during an encode but cannot change its bytes.
 */
static void measure_pair(struct row_pair_plan *plan, unsigned q, bool combined,
			 bool useRows, const struct row_anchor *base,
			 const struct row_anchor *probe, bool bottomRows)
{
	*plan = (struct row_pair_plan){.valid = useRows};
	for (unsigned sc = 0; sc < 2; sc++) {
		unsigned qs = q;
		if (!useRows) {
			plan->wire[sc] = payload_size(sc, q, combined);
			continue;
		}
		plan->compact[sc] = live_jpeg_wire_pack(
		    stream.buffers.encoded[sc], MAX_JPEG,
		    stream.buffers.encoded[sc], stream.runtime.job_size[sc],
		    sc ? 320 : 400, qs);
		plan->wire[sc] = plan->compact[sc]
				     ? plan->compact[sc]
				     : (stream.runtime.job_size[sc]
					    ? stream.runtime.job_size[sc]
					    : MAX_JPEG + 1);
		if (!plan->compact[sc] ||
		    !live_jpeg_rows_parse(&plan->rows[sc],
					  stream.buffers.encoded[sc],
					  plan->compact[sc]))
			plan->valid = false;
	}
	if (plan->valid)
		for (unsigned sc = 0; sc < 2; sc++) {
			/* A decimated bottom must be byte-identical between due
			 * stream.counters.frames so the PC can skip decoding
			 * it; row deltas would carry a new
			 * stream.runtime.sequence each pair.
			 */
			if (sc == 1 && !bottomRows)
				continue;
			if (base)
				plan->wire[sc] = row_size(plan, sc, base);
			if (probe)
				plan->trial_size += row_size(plan, sc, probe);
		}
}

static void store_row_anchor(struct row_anchor *ref,
			     const struct row_pair_plan *plan, u32 seq,
			     u32 stamp)
{
	live_r20_forget(&stream.r20, ref);
	ref->seq = seq;
	ref->stamp = stamp;
	for (unsigned sc = 0; sc < 2; sc++) {
		ref->length[sc] = plan->compact[sc];
		ref->rows[sc] = plan->rows[sc];
		memcpy(ref->bytes + (sc ? ref->length[0] : 0),
		       stream.buffers.encoded[sc], ref->length[sc]);
	}
}

/* Called under the publication stream.runtime.lock. A probe uses the existing
 * stream.settings.spare bank and never goes on the network. The acknowledged
 * base remains immutable until the PC acknowledges a real candidate. */
static bool needs_key(const struct row_pair_plan *plan, u64 now, u32 seq,
		      u32 stamp, u32 budget)
{
	u32 full = plan->compact[0] + plan->compact[1];
	if (!plan->valid || full > ROW_PAIR_BYTES || full > budget)
		return false;
	if (stream.row.candidate &&
	    now - stream.row.last_key < SYSCLOCK_ARM11 / 2)
		return false;
	if (!stream.row.base)
		return true; /* Bootstrap and explicit receiver reset. */
	if (now - stream.row.last_key < SYSCLOCK_ARM11 ||
	    now < stream.row.probe_after)
		return false;
	u32 wire = plan->wire[0] + plan->wire[1];
	if (!stream.row.probe) {
		/* Two chunks or less of remaining JPEG offer little to reclaim.
		 */
		if (wire <= 2 * LIVE_CHUNK)
			return false;
		stream.row.candidate = NULL; /* The previous unacknowledged
					offer has stream.counters.expired. */
		stream.row.probe = stream.row.base == stream.row.banks
				       ? stream.row.banks + 1
				       : stream.row.banks;
		store_row_anchor(stream.row.probe, plan, seq, stamp);
		stream.row.trial = (LiveRowTrial){0};
		return false;
	}
	if (!plan->trial_size)
		return false;
	int decision = live_row_trial_observe(
	    &stream.row.trial, live_combined_wire(wire),
	    live_combined_wire(plan->trial_size), live_combined_wire(full));
	if (!decision)
		return false;
	stream.row.probe = NULL;
	stream.row.probe_after = now + SYSCLOCK_ARM11 / 4;
	return decision > 0;
}

/* Called with stream.runtime.lock held. Full keys are eligible only when the
 * complete wire representation also fits the requested budget. Stale candidates
 * are stream.counters.replaced with a fresh capture rather than freezing the
 * stream for an ACK. */
static void publish_rows(const struct row_pair_plan *plan,
			 const struct row_anchor *base, LivePendingPair *pair,
			 bool key, u64 now)
{
	if (key) {
		stream.row.candidate = stream.row.base == stream.row.banks
					   ? stream.row.banks + 1
					   : stream.row.banks;
		store_row_anchor(stream.row.candidate, plan, pair->seq,
				 pair->stamp);
		for (unsigned sc = 0; sc < 2; sc++)
			memcpy(stream.row.candidate->bytes +
				   (sc ? stream.row.candidate->length[0] : 0),
			       "R17K", 4);
		stream.row.sends = 0;
		stream.row.last_key = now;
		stream.row.keys++;
	}
	for (unsigned sc = 0; sc < 2; sc++) {
		u8 *out = stream.buffers.pending_bytes + pair->offset +
			  (sc ? pair->length[0] : 0);
		if (plan->compact[sc]) {
			stream.counters.compact_frames++;
			stream.counters.compact_saved_bytes +=
			    stream.runtime.job_size[sc] - plan->compact[sc];
		} else
			stream.counters.compact_fallbacks++;
		if (key) {
			memcpy(out, row_anchor_panel(stream.row.candidate, sc),
			       pair->length[sc]);
		} else if (sc == 1 && plan->bottom_delta) {
			memcpy(out,
			       plan->bottom_compressed ? plan->bottom_compressed
						       : plan->bottom_delta,
			       pair->length[sc]);
			stream.row.deltas++;
			stream.row.bytes_saved +=
			    plan->compact[sc] - plan->bottom_delta_bytes;
			if (plan->bottom_compressed) {
				stream.r20.panels++;
				stream.r20.saved +=
				    plan->bottom_delta_bytes - pair->length[sc];
			}
		} else if (base && plan->compact[sc] &&
			   plan->wire[sc] < plan->compact[sc]) {
			pair->length[sc] = live_jpeg_rows_pack(
			    out, pair->length[sc], stream.buffers.encoded[sc],
			    plan->compact[sc], &plan->rows[sc],
			    row_anchor_panel(base, sc), base->length[sc],
			    &base->rows[sc], base->seq);
			stream.row.deltas++;
			stream.row.bytes_saved +=
			    plan->compact[sc] - pair->length[sc];
		} else {
			memcpy(out, stream.buffers.encoded[sc],
			       pair->length[sc]);
		}
	}
}

static u32 pair_payload_budget(u32 byteRate, u32 hz, bool combined,
			       unsigned targetQ)
{
	u32 videoRate =
	    live_av_video_rate(byteRate, stream.settings.audio_enabled);
	return live_quality_budget(videoRate, hz, combined, targetQ);
}

/* Called by the network thread with stream.runtime.lock held, between frame
 * sends. A byte ceiling changes the allowable JPEG size, not the meaning of its
 * fragments. */
static void configure_stream(u32 hz, u32 bps, bool fec, unsigned targetQ)
{
	bool formatChanged = stream.settings.fps != hz ||
			     stream.settings.combined_fec != fec ||
			     stream.settings.quality_target != targetQ;
	if (!formatChanged && stream.settings.rate == bps)
		return;
	if (formatChanged) {
		stream.settings.config_epoch++;
		stream.row.base = stream.row.candidate = stream.row.probe =
		    NULL;
		live_repair_clear(&stream.repair_history);
		stream.repair_tx.credit = 0;
	}
	u32 budget = pair_payload_budget(bps, hz, fec, targetQ);
	unsigned count = stream.pending.count, head = stream.pending.head;
	for (unsigned i = 0; i < count; i++) {
		unsigned slot = head ^ i;
		if (!live_pending_compatible(&stream.pending.slots[slot],
					     stream.settings.config_epoch,
					     budget))
			discard_pending(slot, TIMING_RECONFIGURED);
		else
			stream.counters.rate_pending_kept++;
	}
	__atomic_store_n(&stream.settings.fps, hz, __ATOMIC_RELAXED);
	stream.settings.rate = bps;
	stream.settings.combined_fec = fec;
	stream.settings.quality_target = targetQ;
	stream.counters.config_changes++;
	/* A new stream.settings.quality/stream.settings.rate invalidates the
	 * reused bottom JPEG. */
}

struct pair_encode {
	u32 seq;
	u32 stamp;
	u32 epoch;
	u32 initial_rate;
	u32 budget;
	u32 target_bytes;
	u32 job_rate;
	u32 job_fps;
	unsigned target_q;
	unsigned q;
	bool combined;
	bool use_rows;
	bool bottom_rows;
	bool use_r20;
	struct row_anchor *base;
	struct row_anchor *probe;
	u64 next;
};

static void encode_single_screen(u32 seq, u32 stamp, u32 epoch, u32 budget,
				 unsigned q)
{
	const unsigned sc = (stream.settings.screen_mask & 1) ? 0 : 1;
	size_t packed;
	u32 wire;
	LivePendingPair incoming;
	u32 bytes;
	unsigned slot;
	LivePendingPair *pair;

	if (!run_single_job(1, q, sc))
		return;
	live_timing_encoded();
	packed = stream.settings.compact_jpeg
		     ? live_jpeg_wire_pack(stream.buffers.encoded[sc], MAX_JPEG,
					   stream.buffers.encoded[sc],
					   stream.runtime.job_size[sc],
					   sc ? 320 : 400, q)
		     : 0;
	wire = packed ? (u32)packed : stream.runtime.job_size[sc];
	if (!stream.runtime.job_size[sc] || wire > budget) {
		stream.counters.oversize++;
		return;
	}
	incoming = (LivePendingPair){
	    .length = {sc == 0 ? wire : 0, sc == 1 ? wire : 0},
	    .seq = seq,
	    .stamp = stamp,
	    .epoch = epoch,
	    .r20 = false};
	bytes = live_pending_bytes(&incoming);
	while (!live_pending_fits(&stream.pending, bytes,
				  sizeof stream.buffers.pending_bytes)) {
		discard_pending(stream.pending.head, TIMING_REPLACED);
		stream.counters.replaced++;
	}
	slot =
	    live_pending_append(&stream.pending, stream.buffers.pending_bytes,
				sizeof stream.buffers.pending_bytes, incoming);
	pair = &stream.pending.slots[slot];
	memcpy(stream.buffers.pending_bytes + pair->offset,
	       stream.buffers.encoded[sc], wire);
}

static bool pair_requantize(struct pair_encode *e, struct row_pair_plan *plan,
			    unsigned *q_out, uint32_t *budget_out,
			    uint32_t *target_out)
{
	unsigned q = *q_out;
	uint32_t budget = *budget_out;
	uint32_t target_bytes = *target_out;

	if (!run_pair_job(1, q))
		return false;
	live_timing_encoded();
	/* Bounded requantization: reuse DCT, at most 3 entropy passes. */
	for (unsigned retry = 0;; retry++) {
		uint32_t current_rate;
		uint32_t total;
		unsigned next_q;

		LightLock_Lock(&stream.runtime.lock);
		current_rate = stream.settings.rate;
		LightLock_Unlock(&stream.runtime.lock);
		if (current_rate != e->job_rate) {
			e->job_rate = current_rate;
			budget = pair_payload_budget(e->job_rate, e->job_fps,
						     e->combined, e->target_q);
			target_bytes = budget *
				       stream.runtime.payload_control.percent /
				       100;
		}
		measure_pair(plan, q, e->combined || e->target_q, e->use_rows,
			     e->base, e->probe, e->bottom_rows);
		if (e->use_r20 && plan->valid)
			live_r20_compress(&stream.r20, plan, e->base, e->next,
					  stream.buffers.encoded[0],
					  stream.buffers.encoded[1], MAX_JPEG);
		total = plan->wire[0] + plan->wire[1];
		if (stream.runtime.job_size[0] && stream.runtime.job_size[1] &&
		    total <= budget)
			break;
		next_q =
		    live_retry_quality(e->target_q, q, retry, total, budget);
		if (next_q == q || capture_paused())
			break;
		stream.counters.retries++;
		q = next_q;
		if (!run_pair_job(2, q))
			break;
		live_timing_encoded();
	}
	if (stopped())
		return false;
	*q_out = q;
	*budget_out = budget;
	*target_out = target_bytes;
	return true;
}

static void pair_publish(struct pair_encode *e, struct row_pair_plan *plan,
			 unsigned q, uint32_t budget, uint32_t target_bytes)
{
	uint32_t payload_total = plan->wire[0] + plan->wire[1];
	bool fits = stream.runtime.job_size[0] && stream.runtime.job_size[1] &&
		    payload_total <= budget;
	uint32_t current_budget;

	for (unsigned i = 0; i < 2; i++) {
		stream.counters.attempts[i]++;
		stream.counters.sum_us[i] += stream.runtime.job_us[i];
		stream.counters.changed_mcus[i] +=
		    stream.runtime.job_changed[i];
		if (stream.runtime.job_us[i] > stream.counters.max_us[i])
			stream.counters.max_us[i] = stream.runtime.job_us[i];
		__atomic_store_n(&stream.counters.last_encode_us[i],
				 stream.runtime.job_us[i], __ATOMIC_RELAXED);
		if (fits) {
			__atomic_add_fetch(&stream.counters.jpeg_frames[i], 1,
					   __ATOMIC_RELAXED);
			if (stream.runtime.job_size[i] >
			    stream.counters.max_bytes[i])
				stream.counters.max_bytes[i] =
				    stream.runtime.job_size[i];
		}
	}
	stream.settings.quality = q;
	if (!fits) {
		if (e->target_q) {
			if (!stream.runtime.job_size[0] ||
			    !stream.runtime.job_size[1])
				stream.counters.manual_render_failures++;
			else
				stream.counters.manual_wire_drops++;
		}
		live_timing_drop_encoded(TIMING_OVERSIZE);
		stream.counters.oversize++;
		stream.settings.spare = 0;
		return;
	}
	if (!e->target_q) {
		if (payload_total > target_bytes) {
			stream.settings.quality =
			    live_lower_quality(q, payload_total, target_bytes);
			stream.settings.spare = 0;
		} else if (payload_total < target_bytes * 9 / 10) {
			if (++stream.settings.spare >= 4) {
				stream.settings.quality =
				    stream.settings.quality < 83
					? stream.settings.quality + 2
					: 85;
				stream.settings.spare = 0;
			}
		} else {
			stream.settings.spare = 0;
		}
	}
	LightLock_Lock(&stream.runtime.lock);
	current_budget = pair_payload_budget(stream.settings.rate, e->job_fps,
					     e->combined, e->target_q);
	if (plan->bottom_compressed &&
	    !live_r20_recent(&stream.r20, svcGetSystemTick())) {
		plan->bottom_compressed = NULL;
		plan->wire[1] = plan->bottom_delta_bytes;
		payload_total = plan->wire[0] + plan->wire[1];
	}
	if (e->epoch == stream.settings.config_epoch &&
	    (!e->base || e->base == stream.row.base) &&
	    payload_total <= current_budget &&
	    payload_total <= sizeof stream.buffers.pending_bytes) {
		u64 now;
		bool key;
		LivePendingPair incoming;
		uint32_t bytes;
		bool overflow;
		unsigned slot;
		LivePendingPair *pair;

		if (e->initial_rate != stream.settings.rate)
			stream.counters.rate_encoding_kept++;
		now = svcGetSystemTick();
		key = e->use_rows &&
		      needs_key(plan, now, e->seq, e->stamp, current_budget);
		incoming = (LivePendingPair){
		    .length = {key ? plan->compact[0] : plan->wire[0],
			       key ? plan->compact[1] : plan->wire[1]},
		    .seq = e->seq,
		    .stamp = e->stamp,
		    .epoch = e->epoch,
		    .r20 = !key && plan->bottom_compressed};
		bytes = live_pending_bytes(&incoming);
		overflow =
		    !live_pending_fits(&stream.pending, bytes,
				       sizeof stream.buffers.pending_bytes);
		while (
		    !live_pending_fits(&stream.pending, bytes,
				       sizeof stream.buffers.pending_bytes)) {
			discard_pending(stream.pending.head, TIMING_REPLACED);
			stream.counters.replaced++;
		}
		live_payload_observe(&stream.runtime.payload_control,
				     e->job_fps, overflow);
		slot = live_pending_append(
		    &stream.pending, stream.buffers.pending_bytes,
		    sizeof stream.buffers.pending_bytes, incoming);
		pair = &stream.pending.slots[slot];
		if (e->use_rows) {
			publish_rows(plan, e->base, pair, key, now);
		} else {
			u8 *out = stream.buffers.pending_bytes + pair->offset;

			for (unsigned i = 0; i < 2; i++) {
				pair->length[i] =
				    pack_panel(i, q, out, pair->length[i]);
				out += pair->length[i];
			}
		}
		pair->queued = svcGetSystemTick();
		live_timing_publish(slot, pair->queued);
	} else {
		live_timing_drop_encoded(TIMING_RECONFIGURED);
	}
	LightLock_Unlock(&stream.runtime.lock);
}

static void encode_pair(struct pair_encode *e)
{
	unsigned q = e->q;
	uint32_t budget = e->budget;
	uint32_t target_bytes = e->target_bytes;
	struct row_pair_plan plan;

	if (!pair_requantize(e, &plan, &q, &budget, &target_bytes))
		return;
	pair_publish(e, &plan, q, budget, target_bytes);
}

static void encoder_bottom_worker(void)
{
	while (!stopped()) {
		LightEvent_WaitTimeout(&stream.runtime.events[1], 100000000);
		if (stopped())
			break;
		if (__atomic_load_n(&stream.runtime.job_done, __ATOMIC_ACQUIRE))
			continue;
		encode_job(1, stream.runtime.job_kind,
			   stream.runtime.job_quality);
		__atomic_store_n(&stream.runtime.job_done, 1, __ATOMIC_RELEASE);
		LightEvent_Signal(&stream.runtime.events[2]);
	}
}

struct encoder_plan {
	u32 stamp;
	u32 wanted[2];
	u32 job_rate;
	u32 job_fps;
	u32 epoch;
	unsigned target_q;
	bool combined;
	bool use_rows;
	bool bottom_rows;
	bool use_r20;
	struct row_anchor *base;
	struct row_anchor *probe;
};

/* Called with the stream stream.runtime.lock held only inside this helper. */
static bool encoder_read_plan(struct encoder_plan *plan,
			      LiveConsumedPlan *consumed, u64 now)
{
	LightLock_Lock(&stream.runtime.lock);
	plan->stamp = stream.runtime.plan_stamp;
	plan->wanted[0] = stream.runtime.plan_pts[0];
	plan->wanted[1] = stream.runtime.plan_pts[1];
	plan->job_rate = stream.settings.rate;
	plan->job_fps = stream.settings.fps;
	plan->epoch = stream.settings.config_epoch;
	plan->combined =
	    stream.settings.combined_fec && stream.settings.screen_mask == 3;
	plan->target_q = stream.settings.quality_target;
	plan->use_rows = stream.row.enabled && plan->combined &&
			 stream.settings.compact_jpeg;
	plan->bottom_rows = plan->combined;
	plan->use_r20 = plan->use_rows && plan->bottom_rows &&
			live_r20_recent(&stream.r20, now);
	plan->base = plan->use_rows ? stream.row.base : NULL;
	plan->probe = plan->use_rows ? stream.row.probe : NULL;
	bool ready = stream.runtime.plan_mask == 3 &&
		     live_plan_available(consumed, plan->stamp);
	LightLock_Unlock(&stream.runtime.lock);
	return ready;
}

static bool encoder_snapshot_raw(const struct encoder_plan *plan, u32 *source)
{
	bool ready = true;

	for (unsigned i = 0; ready && i < 2; i++) {
		LightLock_Lock(&stream.runtime.raw_locks[i]);
		if (!stream.runtime.raw_valid[i] ||
		    stream.runtime.raw_pts[i] != plan->wanted[i]) {
			ready = false;
		} else {
			memcpy(stream.buffers.raw_working[i],
			       live_jpeg_source_frame(
				   stream.buffers.raw_sources[i]),
			       stream.buffers.raw_bytes[i]);
			source[i] = stream.runtime.raw_pts[i];
		}
		LightLock_Unlock(&stream.runtime.raw_locks[i]);
	}
	return ready;
}

static void encoder(void *arg)
{
	if ((uintptr_t)arg) {
		encoder_bottom_worker();
		return;
	}
	u64 next = svcGetSystemTick();
	LiveConsumedPlan consumed = {0};
	u32 budgetEpoch = 0, budgetRate = 0;
	unsigned previousTarget = 0;
	while (!stopped()) {
		if (!live_peer_ready() || capture_paused()) {
			svcSleepThread(10000000);
			continue;
		}
		u64 now = svcGetSystemTick();
		if (now < next) {
			svcSleepThread((next - now) * 1000000000ull /
				       SYSCLOCK_ARM11);
			continue;
		}
		struct encoder_plan plan;
		u32 source[2] = {0, 0};

		if (!encoder_read_plan(&plan, &consumed, now)) {
			LightEvent_WaitTimeout(&stream.runtime.events[0],
					       1000000);
			continue;
		}
		if (stopped())
			break;
		u64 snapshot_begin = svcGetSystemTick();

		if (!encoder_snapshot_raw(&plan, source)) {
			LightEvent_WaitTimeout(&stream.runtime.events[0],
					       1000000);
			continue;
		}
		u32 stamp = plan.stamp, jobRate = plan.job_rate,
		    jobFps = plan.job_fps, epoch = plan.epoch;
		bool combined = plan.combined;
		unsigned targetQ = plan.target_q;
		bool useRows = plan.use_rows;
		bool bottomRows = plan.bottom_rows;
		bool useR20 = plan.use_r20;
		struct row_anchor *base = plan.base;
		struct row_anchor *probe = plan.probe;
		live_plan_consume(&consumed, stamp);
		u32 snapshotUs = (u32)((svcGetSystemTick() - snapshot_begin) *
				       1000000 / SYSCLOCK_ARM11);
		stream.counters.snapshot_count++;
		stream.counters.snapshot_sum_us += snapshotUs;
		if (snapshotUs > stream.counters.snapshot_max_us)
			stream.counters.snapshot_max_us = snapshotUs;
		next = live_next_encode_tick(next, now, SYSCLOCK_ARM11, jobFps);
		const u32 initialRate = jobRate;
		u32 budget =
		    pair_payload_budget(jobRate, jobFps, combined, targetQ);
		if (budgetEpoch != epoch || budgetRate != jobRate) {
			stream.runtime.payload_control =
			    (LivePayloadControl){.percent = 100};
			stream.settings.spare = 0;
			budgetEpoch = epoch;
			budgetRate = jobRate;
			if (targetQ)
				stream.settings.quality = targetQ;
			else if (previousTarget)
				stream.settings.quality = 60;
			previousTarget = targetQ;
		}
		u32 targetBytes =
		    budget * stream.runtime.payload_control.percent / 100;
		for (unsigned i = 0; i < 2; i++) {
			stream.counters.source_reuse[i] +=
			    stream.counters.pairs &&
			    source[i] == stream.counters.last_source[i];
			stream.counters.last_source[i] = source[i];
			stream.runtime.job_us[i] = 0;
		}
		stream.counters.pairs++;
		u32 seq = ++stream.runtime.sequence;
		unsigned q = targetQ ? targetQ : stream.settings.quality;
		live_timing_begin(seq, stamp);
		if (stream.settings.screen_mask != 3) {
			encode_single_screen(seq, stamp, epoch, budget, q);
			continue;
		}
		struct pair_encode pair = {
		    .seq = seq,
		    .stamp = stamp,
		    .epoch = epoch,
		    .initial_rate = initialRate,
		    .budget = budget,
		    .target_bytes = targetBytes,
		    .target_q = targetQ,
		    .q = q,
		    .job_rate = jobRate,
		    .job_fps = jobFps,
		    .combined = combined,
		    .use_rows = useRows,
		    .bottom_rows = bottomRows,
		    .use_r20 = useR20,
		    .base = base,
		    .probe = probe,
		    .next = next,
		};
		encode_pair(&pair);
	}
}
static void pacing_sleep_until(u64 due)
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

static void pacing_charge(u64 *due, u64 begin, u64 returned, u32 wire,
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

static void send_audio(u64 *due)
{
	live_audio_sender_send(&stream.audio, stream.runtime.session,
			       stream.settings.audio_enabled, stopped(),
			       stream.net.fd, &stream.net.target, due,
			       pacing_sleep_until, audio_pacing_charge);
}

static size_t network_report_text(char *text, size_t capacity)
{
	struct live_net_report snapshot;

	live_net_snapshot(&stream.net, stream.runtime.session, &snapshot);
	return live_net_report_format(text, capacity, &snapshot);
}

/* The socket stream.runtime.worker owns the live counters; publish one complete
 * copy. */
static void queue_network_report(void)
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

static void reset_receiver_feedback(void)
{
	stream.runtime.receiver_feedback = (LiveReceiverFeedback){0};
	__atomic_store_n(&stream.runtime.receiver_video, false,
			 __ATOMIC_RELEASE);
}

static void row_peer(bool enable, bool forget)
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

static void reset_peer_config(void)
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

static u64 send_status(u64 *due, const u8 *msg, u32 size)
{
	pacing_sleep_until(*due);
	u64 begin = svcGetSystemTick();
	sendto(stream.net.fd, msg, size, 0,
	       (struct sockaddr *)&stream.net.target, sizeof stream.net.target);
	pacing_charge(due, begin, svcGetSystemTick(), size + 28, PACKET_STATUS);
	return begin;
}

/* Call only after address, port, stream.runtime.session and control payload
 * validation. */
static void peer_activity(u64 now)
{
	stream.net.last_peer_control = now;
	if (stream.net.automatic_host)
		live_discover_activity(&stream.net.discovery,
				       ntohl(stream.net.target.sin_addr.s_addr),
				       now);
}

static void accept_repair(const u8 *msg, int size, u64 now)
{
	if (!stream.settings.combined_fec || size <= 20 ||
	    size > 20 + (int)LIVE_REPAIR_MAX ||
	    get32(msg + 4) != stream.runtime.session)
		return;
	u32 n = get32(msg + 16);
	if (n != (u32)size - 20)
		return;
	stream.runtime.repair_interest = now;
	if (live_repair_request(&stream.repair_history, get32(msg + 8),
				get32(msg + 12), msg + 20, n, LIVE_CHUNK, now,
				(u64)SYSCLOCK_ARM11 * LIVE_REPAIR_SERVICE_MS /
				    1000)) {
		peer_activity(now);
		stream.runtime.repair_requests++;
		stream.runtime.packet_report.nack =
		    (LivePacketSample){.seq = get32(msg + 8),
				       .pts = get32(msg + 12),
				       .begin = now};
		stream.runtime.packet_report.flags |= 2;
	}
}

static void poll_frame_feedback(u64 now)
{
	if (!stream.runtime.repair_interest ||
	    now - stream.runtime.repair_interest > 2ull * SYSCLOCK_ARM11 ||
	    stream.control.deferred_size ||
	    now - stream.runtime.last_repair_poll < SYSCLOCK_ARM11 / 250)
		return;
	stream.runtime.last_repair_poll = now;
	struct sockaddr_in from;
	socklen_t size = sizeof from;
	int n = recvfrom(stream.net.fd, stream.control.deferred_bytes,
			 sizeof stream.control.deferred_bytes, 0,
			 (struct sockaddr *)&from, &size);
	if (n <= 0 || size < sizeof from || from.sin_family != AF_INET ||
	    from.sin_port != stream.net.target.sin_port ||
	    from.sin_addr.s_addr != stream.net.target.sin_addr.s_addr)
		return;
	if (n >= 4 && !memcmp(stream.control.deferred_bytes, "R19N", 4))
		accept_repair(stream.control.deferred_bytes, n, now);
	else {
		stream.control.deferred_from = from;
		stream.control.deferred_size = n;
	}
}

/* Called with the pair stream.runtime.lock held. Changing the mask invalidates
 * every queued pair and the row reference: both were built for the old panel
 * set. */
static void apply_screen_mask_locked(u32 mask)
{
	if (mask == stream.settings.screen_mask)
		return;

	stream.settings.screen_mask = mask;
	while (stream.pending.count)
		discard_pending(stream.pending.head, TIMING_RECONFIGURED);
	stream.row.base = stream.row.candidate = NULL;
}

static void control_configure(const struct live_control_config *config, u64 now)
{
	if (stream.settings.config_sequence &&
	    (u32)(config->sequence - stream.settings.config_sequence) >=
		0x80000000u)
		return;

	peer_activity(now);
	stream.settings.config_sequence = config->sequence;
	LightLock_Lock(&stream.runtime.lock);
	apply_screen_mask_locked(config->mask);
	configure_stream(config->fps, config->rate, config->combined_fec,
			 config->quality);
	LightLock_Unlock(&stream.runtime.lock);
	stream.runtime.last_status_tick = 0;
}

static void control_set_mask(u32 mask, u64 now)
{
	peer_activity(now);
	LightLock_Lock(&stream.runtime.lock);
	apply_screen_mask_locked(mask);
	LightLock_Unlock(&stream.runtime.lock);
}

static void control_set_pacing(unsigned mode, u64 now)
{
	if (stream.settings.pacing_mode != mode) {
		stream.settings.pacing_mode = mode;
		stream.runtime.last_status_tick = 0;
	}
	peer_activity(now);
}

static void control_ack_r20(u64 now)
{
	if (!stream.row.banks || !stream.settings.compact_jpeg ||
	    !stream.settings.combined_fec)
		return;
	LightLock_Lock(&stream.runtime.lock);
	stream.r20.last_ack = now;
	LightLock_Unlock(&stream.runtime.lock);
	peer_activity(now);
}

static void control_repair_peer(u64 now)
{
	stream.runtime.repair_peer_at = now;
	peer_activity(now);
}

static void control_ack_rows(u32 flags, u32 row_sequence, u64 now)
{
	if (!stream.row.banks || !stream.settings.compact_jpeg)
		return;

	stream.row.last_ack = now;
	peer_activity(now);
	LightLock_Lock(&stream.runtime.lock);
	row_peer(true, !flags);
	if (flags && stream.row.candidate &&
	    stream.row.candidate->seq == row_sequence) {
		stream.row.base = stream.row.candidate;
		stream.row.candidate = NULL;
		stream.row.probe = NULL;
		stream.row.acks++;
	}
	LightLock_Unlock(&stream.runtime.lock);
}

static void control_feedback(u32 sent, u32 transit, u64 now)
{
	peer_activity(now);
	stream.counters.feedback_count++;
	live_receiver_observe(&stream.runtime.receiver_feedback, sent, transit,
			      now,
			      (u64)LIVE_PEER_LEASE_SECONDS * SYSCLOCK_ARM11);
}

static const struct live_control_ops control_ops = {
    .configure = control_configure,
    .set_mask = control_set_mask,
    .set_pacing = control_set_pacing,
    .ack_r20 = control_ack_r20,
    .repair_peer = control_repair_peer,
    .repair_request = accept_repair,
    .ack_rows = control_ack_rows,
    .feedback = control_feedback,
};

static bool net_paced(void)
{
	return stream.settings.pacing_mode != 0;
}

static void net_observe_probe(u64 begin, u64 returned, u32 wire)
{
	live_packet_observe(&stream.runtime.packet_report, PACKET_STATUS, begin,
			    returned, wire, SYSCLOCK_ARM11);
}

static void net_charge_packet(u64 *due, u64 begin, u64 returned, u32 wire,
			      int kind)
{
	pacing_charge(due, begin, returned, wire, (enum LivePacketKind)kind);
}

static const struct live_net_ops net_ops = {
    .paced = net_paced,
    .sleep_until = pacing_sleep_until,
    .charge = net_charge_packet,
    .observe = net_observe_probe,
    .report = queue_network_report,
    .reset_peer = reset_peer_config,
    .reset_feedback = reset_receiver_feedback,
};

static void send_status_burst(u64 *due)
{
	u64 now = svcGetSystemTick();

	now = svcGetSystemTick();
	if (!stream.net.media_ready ||
	    now - stream.runtime.last_status_tick < SYSCLOCK_ARM11)
		return;
	struct live_status_main status = {
	    .session = stream.runtime.session,
	    .duration_ms =
		(u32)((now - stream.runtime.started) * 1000 / SYSCLOCK_ARM11),
	    .rate = stream.settings.rate,
	    .video_rate = live_av_video_rate(stream.settings.rate,
					     stream.settings.audio_enabled),
	    .fps = stream.settings.fps,
	    .encoded_top = live_encoded_frames(0),
	    .encoded_bottom = live_encoded_frames(1),
	    .sent_top = stream.counters.frames[0],
	    .sent_bottom = stream.counters.frames[1],
	    .send_errors = stream.counters.errors,
	    .expired_pairs = stream.counters.expired,
	    .feedback_count = stream.counters.feedback_count,
	    .encoder_core_top = stream.runtime.encoder_cores[0],
	    .encoder_core_bottom = stream.runtime.encoder_cores[1],
	    .last_encode_us_top = __atomic_load_n(
		&stream.counters.last_encode_us[0], __ATOMIC_RELAXED),
	    .last_encode_us_bottom = __atomic_load_n(
		&stream.counters.last_encode_us[1], __ATOMIC_RELAXED),
	    .audio_enabled = stream.settings.audio_enabled,
	    .audio_blocks_queued =
		__atomic_load_n(&stream.audio.queue.queued, __ATOMIC_RELAXED),
	    .audio_blocks_dropped =
		__atomic_load_n(&stream.audio.queue.dropped, __ATOMIC_RELAXED) +
		__atomic_load_n(&stream.audio.queue.expired, __ATOMIC_RELAXED),
	    .audio_packets_sent = stream.audio.packets_sent,
	    .audio_queue_depth = live_audio_queue_depth(&stream.audio.queue),
	};
	uint32_t pacing_words[LIVE_PACING_WORDS];
	LightLock_Lock(&stream.runtime.lock);
	uint32_t overwritten = stream.counters.replaced;
	LightLock_Unlock(&stream.runtime.lock);
	live_packet_snapshot(&stream.runtime.packet_report, pacing_words,
			     stream.runtime.session, status.duration_ms,
			     stream.settings.pacing_mode,
			     stream.runtime.started, SYSCLOCK_ARM11);
	struct live_status_aux aux = {
	    .session = stream.runtime.session,
	    .duration_ms = status.duration_ms,
	    .quality_target = stream.settings.quality_target,
	    .overwritten = overwritten,
	    .row_repeats = stream.row.repeats,
	    .repair_requests = stream.runtime.repair_requests,
	    .repair_packets = stream.repair_tx.packets,
	    .repair_wire_bytes = stream.repair_tx.wire_bytes,
	    .audio_dropped = stream.audio.queue.dropped,
	    .audio_expired = stream.audio.queue.expired,
	    .send_active_us =
		(u32)(stream.runtime.send_active_ticks / SYSCLOCK_ARM11 *
			  1000000ull +
		      (stream.runtime.send_active_ticks % SYSCLOCK_ARM11) *
			  1000000ull / SYSCLOCK_ARM11),
	    .compact_jpeg = stream.settings.compact_jpeg,
	    .rows_enabled = stream.row.banks != NULL,
	    .combined_fec = stream.settings.combined_fec,
	    .audio_wire_bytes = stream.audio.wire_bytes,
	    .video_wire_bytes = stream.counters.video_wire_bytes,
	    .pacing_mode = stream.settings.pacing_mode,
	};
	memcpy(aux.pacing_words, pacing_words, sizeof(aux.pacing_words));
	u8 burst[512];
	struct live_status_packet packets[LIVE_STATUS_MAX_PACKETS];
	unsigned packet_count =
	    live_status_build_burst(burst, sizeof(burst), &status, &aux,
				    packets, LIVE_STATUS_MAX_PACKETS);
	if (!packet_count)
		return;
	stream.runtime.last_status_tick =
	    send_status(due, burst + packets[0].offset, packets[0].size);
	for (unsigned i = 1; i < packet_count; i++)
		send_status(due, burst + packets[i].offset, packets[i].size);
#ifdef LIVE_FRAME_TIMING
	u32 capacity;
	if (live_timing_marker(stream.runtime.last_status_tick, &capacity)) {
		u8 marker[24];
		memcpy(marker, "R12D", 4);
		put32(marker + 4, stream.runtime.session);
		put32(marker + 8, 1);
		put32(marker + 12, capacity);
		pacing_sleep_until(*due);
		u64 call = svcGetSystemTick();
		put32(marker + 16, (u32)call);
		put32(marker + 20, (u32)(call >> 32));
		sendto(stream.net.fd, marker, sizeof marker, 0,
		       (struct sockaddr *)&stream.net.target,
		       sizeof stream.net.target);
		pacing_charge(due, call, svcGetSystemTick(), sizeof marker + 28,
			      PACKET_STATUS);
	}
#endif
}

static void control_receive(u64 *due)
{
	u64 now = svcGetSystemTick();

	if (!(stream.control.deferred_size ||
	      now - stream.runtime.last_control_tick >= SYSCLOCK_ARM11 / 100 ||
	      now - stream.net.last_probe_tick >= SYSCLOCK_ARM11))
		return;
	stream.runtime.last_control_tick = now;
	for (unsigned i = 0; i < 16; i++) {
		u8 msg[64];
		struct sockaddr_in from;
		socklen_t size = sizeof from;
		int n;
		if (stream.control.deferred_size) {
			n = stream.control.deferred_size;
			from = stream.control.deferred_from;
			memcpy(msg, stream.control.deferred_bytes, n);
			stream.control.deferred_size = 0;
		} else
			n = recvfrom(stream.net.fd, msg, sizeof msg, 0,
				     (struct sockaddr *)&from, &size);
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				stream.net.diag.receive_errno = errno;
			break;
		}
		now = svcGetSystemTick();
		stream.net.diag.replies++;
		if (size < sizeof from || from.sin_family != AF_INET ||
		    from.sin_port != stream.net.target.sin_port)
			continue;
		if (stream.net.automatic_host && !stream.net.discovery.peer &&
		    live_discover_invitation(msg, (size_t)n)) {
			u32 address = ntohl(from.sin_addr.s_addr);
			if (!live_reply_address(address,
						stream.net.local_address,
						stream.net.local_mask) ||
			    address == stream.net.diag.reported_broadcast)
				continue;
			stream.net.diag.hints++;
			if (stream.net.diag.last_hint_tick &&
			    now - stream.net.diag.last_hint_tick <
				SYSCLOCK_ARM11 / 5)
				continue;
			stream.net.diag.last_hint_tick = now;
			u8 probe[LIVE_DISCOVERY_BYTES];
			live_discover_invited_probe(&stream.net.discovery,
						    stream.runtime.session,
						    probe);
			if (live_net_send_probe(&stream.net, probe, address,
						due, &net_ops))
				stream.net.diag.hints_answered++;
			continue;
		}
		if (n < 8 || get32(msg + 4) != stream.runtime.session)
			continue;
		if (stream.net.automatic_host && n >= 4 &&
		    !memcmp(msg, "3DLA", 4)) {
			bool first = !stream.net.discovery.peer;
			u32 address = ntohl(from.sin_addr.s_addr);
			if (live_reply_address(address,
					       stream.net.local_address,
					       stream.net.local_mask) &&
			    address != stream.net.diag.reported_broadcast &&
			    live_discover_ack(&stream.net.discovery, address,
					      stream.runtime.session, msg,
					      (size_t)n, now)) {
				stream.net.diag.accepted++;
				stream.net.last_peer_control = now;
				if (first) {
					stream.net.target.sin_addr =
					    from.sin_addr;
					stream.runtime.last_status_tick = 0;
					__atomic_store_n(
					    &stream.net.media_ready, true,
					    __ATOMIC_RELEASE);
					queue_network_report();
				}
			}
			continue;
		}
		if (!stream.net.media_ready ||
		    from.sin_addr.s_addr != stream.net.target.sin_addr.s_addr)
			continue;
		live_control_handle(msg, n, now, stream.settings.screen_mask,
				    stream.settings.combined_fec,
				    stream.settings.quality_target,
				    &control_ops);
	}
	const bool hasVideo =
	    live_receiver_recent(&stream.runtime.receiver_feedback, now,
				 (u64)LIVE_PEER_LEASE_SECONDS * SYSCLOCK_ARM11);
	if (hasVideo !=
	    __atomic_load_n(&stream.runtime.receiver_video, __ATOMIC_RELAXED))
		__atomic_store_n(&stream.runtime.receiver_video, hasVideo,
				 __ATOMIC_RELEASE);
}

static void control(u64 *due)
{
	u64 now = svcGetSystemTick();
	if (stream.net.fd < 0)
		live_net_poll(&stream.net, stream.runtime.session, now, due,
			      &net_ops);
	if (stream.net.fd < 0)
		return;
	if (stream.row.last_ack &&
	    now - stream.row.last_ack >=
		(u64)LIVE_PEER_LEASE_SECONDS * SYSCLOCK_ARM11) {
		stream.row.last_ack = 0;
		LightLock_Lock(&stream.runtime.lock);
		row_peer(false, false);
		LightLock_Unlock(&stream.runtime.lock);
	}
	control_receive(due);
	/* Consume queued acknowledgements before expiring the lease, rotating
	 * the challenge or closing the only socket that can receive them. */
	live_net_poll(&stream.net, stream.runtime.session, svcGetSystemTick(),
		      due, &net_ops);
	send_status_burst(due);
}

typedef struct {
	u64 totalDue;
	LiveVideoPacing video;
	bool firstPacket;
} PacketPacing;

/* Use stream.settings.spare wire time between fresh packets. A repair shares
 * the strict total clock with stream.audio and never advances the fresh video's
 * slot clock. */
static void repair_charge(u64 *due, u64 begin, u64 returned, u32 wire, int kind)
{
	pacing_charge(due, begin, returned, wire, (enum LivePacketKind)kind);
}

static void repair_report(u32 seq, u32 pts, u32 index, u64 begin, u64 returned)
{
	stream.runtime.packet_report.repair =
	    (LivePacketSample){.seq = seq,
			       .pts = pts,
			       .index = index,
			       .begin = begin,
			       .end = returned};
	stream.runtime.packet_report.flags |= 4;
}

static bool send_repair(u64 *total_due, u64 now, u64 next_fresh_due)
{
	struct live_repair_io io = {
	    .fd = stream.net.fd,
	    .target = &stream.net.target,
	    .session = stream.runtime.session,
	    .rate = stream.settings.rate,
	    .stopping = stopped,
	    .capture_paused = capture_paused,
	    .charge = repair_charge,
	    .report = repair_report,
	};
	bool sent = live_repair_tx_send(
	    &stream.repair_tx, &stream.repair_history,
	    stream.buffers.send_arena, sizeof stream.buffers.send_arena,
	    stream.settings.fps, total_due, now, next_fresh_due, &io);

	if (sent && stream.repair_tx.last_wire)
		stream.counters.video_wire_bytes += stream.repair_tx.last_wire;
	return sent;
}

static bool send_packet(PacketPacing *pacing, u64 deadline, const char *magic,
			u32 seq, u32 stamp, u32 sc, u32 total, u32 offset,
			const u8 *data, u32 len)
{
	bool usedGap = false;
	for (;;) {
		send_audio(&pacing->totalDue);
		u64 now = svcGetSystemTick();
		poll_frame_feedback(now);
		now = svcGetSystemTick();
		if (stopped() || now > deadline) {
			live_timing_finish(stopped() ? TIMING_STOPPED
						     : TIMING_DEADLINE);
			return false;
		}
		u64 due = live_video_due(&pacing->video);
		if (!usedGap && stream.repair_tx.credit &&
		    pacing->totalDue < due && now < due &&
		    live_repair_next(&stream.repair_history, now)) {
			if (send_repair(&pacing->totalDue, now, due)) {
				usedGap = true;
				continue;
			}
			if (now < pacing->totalDue)
				due = pacing->totalDue;
		}
		if (due < pacing->totalDue)
			due = pacing->totalDue;
		if (now >= due)
			break;
		u64 wait = due - now;
		/* A sparse video pair must not keep a ready stream.audio batch
		 * waiting. */
		if (stream.settings.audio_enabled &&
		    wait > SYSCLOCK_ARM11 / 500)
			wait = SYSCLOCK_ARM11 / 500;
		pacing_sleep_until(now + wait);
	}
	const u32 header = 32;
	u8 packet[LIVE_CHUNK + 32];
	memcpy(packet, magic, 4);
	put32(packet + 4, stream.runtime.session);
	put32(packet + 8, seq);
	put32(packet + 12, stamp);
	put32(packet + 16, sc);
	put32(packet + 20, total);
	put32(packet + 24, offset);
	put32(packet + 28, stream.settings.fps);
	memcpy(packet + header, data, len);
	u64 begin = svcGetSystemTick();
	int sent = sendto(stream.net.fd, packet, len + header, 0,
			  (struct sockaddr *)&stream.net.target,
			  sizeof stream.net.target);
	u64 returned = svcGetSystemTick();
	u32 us = (u32)((returned - begin) * 1000000 / SYSCLOCK_ARM11);
	live_timing_packet(begin, returned, sent == (int)(len + header));
	stream.counters.send_calls++;
	stream.counters.send_sum_us += us;
	if (us > stream.counters.send_max_us)
		stream.counters.send_max_us = us;
	pacing_charge(&pacing->totalDue, begin, returned, len + header + 28,
		      PACKET_VIDEO);
	live_video_charge(&pacing->video, begin, len + header + 28,
			  SYSCLOCK_ARM11);
	if (sent != (int)(len + header)) {
		stream.runtime.last_errno = errno;
		stream.counters.errors++;
		live_timing_finish(TIMING_SOCKET_ERROR);
		return false;
	}
	if (pacing->firstPacket) {
		stream.runtime.packet_report.first =
		    (LivePacketSample){seq, stamp, offset, begin, returned};
		stream.runtime.packet_report.flags |= 1;
		pacing->firstPacket = false;
	}
	stream.counters.video_wire_bytes += len + header + 28;
	return true;
}
struct tx_frame {
	u32 sizes[2];
	u32 seq;
	u32 stamp;
	u32 repeat;
	u64 queued;
	bool send_key;
	LiveRepairFrame *cached;
	u8 *sending[2];
};

static void sender_purge_pending(u64 now)
{
	while (stream.pending.count) {
		const LivePendingPair *head =
		    &stream.pending.slots[stream.pending.head];

		if (head->epoch != stream.settings.config_epoch ||
		    (head->r20 && !live_r20_recent(&stream.r20, now))) {
			discard_pending(stream.pending.head,
					TIMING_RECONFIGURED);
		} else if (live_pending_expired(head, now, SYSCLOCK_ARM11,
						stream.settings.fps)) {
			discard_pending(stream.pending.head, TIMING_TOO_OLD);
			stream.counters.expired++;
		} else {
			break;
		}
	}
}

/* Called with the pair stream.runtime.lock held. */
static bool sender_select_key(struct tx_frame *tx, u64 now)
{
	bool selective = stream.runtime.repair_peer_at &&
			 now - stream.runtime.repair_peer_at <
			     (u64)LIVE_PEER_LEASE_SECONDS * SYSCLOCK_ARM11;
	bool send_key =
	    stream.row.candidate &&
	    (selective
		 ? (stream.row.sends == 0 &&
		    stream.row.candidate->seq == tx->seq)
		 : (stream.row.sends < 3 &&
		    (u32)(tx->seq - stream.row.candidate->seq) < 0x80000000u));

	if (send_key &&
	    (svcGetSystemTick() - stream.row.last_key > SYSCLOCK_ARM11 / 4 ||
	     stream.row.candidate->length[0] + stream.row.candidate->length[1] >
		 pair_payload_budget(stream.settings.rate, stream.settings.fps,
				     stream.settings.combined_fec,
				     stream.settings.quality_target))) {
		send_key = false;
		stream.row.sends = 3;
	}
	if (send_key) {
		tx->seq = stream.row.candidate->seq;
		tx->stamp = stream.row.candidate->stamp;
		tx->repeat = stream.row.sends++;
		if (tx->repeat)
			stream.row.repeats++;
	}
	return send_key;
}

/* Called with the pair stream.runtime.lock held. */
static void sender_copy_panels(struct tx_frame *tx, const LivePendingPair *pair,
			       bool send_key)
{
	static LiveRepairFrame single_frame;
	u64 reserved, until;

	for (unsigned sc = 0; sc < 2; sc++)
		tx->sizes[sc] = send_key ? stream.row.candidate->length[sc]
					 : pair->length[sc];
	reserved = svcGetSystemTick();
	until = reserved + (u64)SYSCLOCK_ARM11 * LIVE_REPAIR_HISTORY_MS / 1000;
	if (tx->sizes[0] && tx->sizes[1]) {
		tx->cached = live_repair_reserve(
		    &stream.repair_history, sizeof stream.buffers.send_arena,
		    tx->sizes[0], tx->sizes[1], tx->seq, tx->stamp,
		    stream.settings.fps, reserved, until);
	} else {
		/* A single-screen stream has no second panel, so it cannot
		 * share the two-panel repair arena; send it directly. */
		single_frame = (LiveRepairFrame){0};
		single_frame.top = tx->sizes[0];
		single_frame.bottom = tx->sizes[1];
		tx->cached = &single_frame;
	}
	if (tx->cached) {
		tx->sending[0] =
		    stream.buffers.send_arena +
		    (tx->sizes[0] && tx->sizes[1] ? tx->cached->offset : 0);
		tx->sending[1] = tx->sending[0] + tx->sizes[0];
	}
	for (unsigned sc = 0; tx->cached && sc < 2; sc++) {
		const u8 *panel = stream.buffers.pending_bytes + pair->offset +
				  (sc ? pair->length[0] : 0);

		memcpy(tx->sending[sc],
		       send_key ? row_anchor_panel(stream.row.candidate, sc)
				: panel,
		       tx->sizes[sc]);
	}
}

static bool sender_prepare(struct tx_frame *tx)
{
	bool ready;
	u64 now;

	memset(tx, 0, sizeof(*tx));
	LightLock_Lock(&stream.runtime.lock);
	now = svcGetSystemTick();
	sender_purge_pending(now);
	ready = stream.pending.count != 0;
	if (ready) {
		unsigned slot = stream.pending.head;
		const LivePendingPair *pair = &stream.pending.slots[slot];
		bool send_key;

		tx->seq = pair->seq;
		tx->stamp = pair->stamp;
		tx->queued = pair->queued;
		send_key = sender_select_key(tx, now);
		if (tx->seq == pair->seq) {
			live_timing_claim(slot);
		} else {
			live_timing_drop_pending(slot, TIMING_REPLACED);
			stream.counters.replaced++;
		}
		sender_copy_panels(tx, pair, send_key);
		live_pending_remove(&stream.pending, slot);
	}
	LightLock_Unlock(&stream.runtime.lock);
	return ready;
}

static void sender_send_frame(struct tx_frame *tx, PacketPacing *pacing,
			      bool *sent_frame, u32 *last_sent_seq)
{
	u32 *sizes = tx->sizes;
	u32 seq = tx->seq, stamp = tx->stamp, repeat = tx->repeat;
	u64 queued = tx->queued;
	LiveRepairFrame *cached = tx->cached;
	u8 **sending = tx->sending;
	if (!cached) {
		stream.counters.expired++;
		live_timing_finish(TIMING_TOO_OLD);
		return;
	}
	u32 screenChunks[2] = {live_data_packets(sizes[0]),
			       live_data_packets(sizes[1])};
	const bool shared =
	    stream.settings.combined_fec && sizes[0] && sizes[1];
	u32 dataCount = shared ? live_data_packets(sizes[0] + sizes[1])
			       : screenChunks[0] + screenChunks[1];
	u32 wire =
	    shared ? live_combined_wire(sizes[0] + sizes[1])
		   : live_screen_wire(sizes[0]) + live_screen_wire(sizes[1]);
	stream.repair_tx.credit =
	    shared ? live_repair_allowance(
			 live_av_video_rate(stream.settings.rate,
					    stream.settings.audio_enabled),
			 stream.settings.fps, wire)
		   : 0;
	live_video_slot(&pacing->video, queued, wire, dataCount,
			stream.settings.fps,
			live_av_video_rate(stream.settings.rate,
					   stream.settings.audio_enabled));
#ifdef LIVE_FRAME_TIMING
	u32 parityCount =
	    shared ? live_parity_packets(dataCount, LIVE_PAIR_PARITY)
		   : live_parity_packets(screenChunks[0], LIVE_PARITY) +
			 live_parity_packets(screenChunks[1], LIVE_PARITY);
	live_timing_plan(dataCount, dataCount + parityCount);
#endif
	u64 begin = svcGetSystemTick(),
	    deadline = live_video_deadline(
		begin, SYSCLOCK_ARM11, wire,
		live_av_video_rate(stream.settings.rate,
				   stream.settings.audio_enabled),
		stream.settings.quality_target != 0);
	if (begin - queued > SYSCLOCK_ARM11 / stream.settings.fps * 2) {
		stream.counters.expired++;
		live_timing_finish(TIMING_TOO_OLD);
		return;
	}
	pacing->firstPacket = true;
	u8 parity[LIVE_PAIR_PARITY][LIVE_CHUNK] = {0};
	bool ok = true;
	if (shared) {
		u8 fragment[LIVE_CHUNK];
		u32 count = dataCount;
		/* Shift repeated keys by a third so the same
		 * short Wi-Fi fade need not erase the same
		 * offsets on each pass. */
		u32 index = repeat * count / 3;
		for (u32 step = 0; ok && step < count; step++) {
			u32 len =
			    live_pair_fragment(fragment, sending[0], sizes[0],
					       sending[1], sizes[1], index);
			u8 *stripe = parity[index % LIVE_PAIR_PARITY];
			for (u32 j = 0; j < len; j++)
				stripe[j] ^= fragment[j];
			ok = send_packet(pacing, deadline, "R10P", seq, stamp,
					 sizes[0], sizes[1], index, fragment,
					 len);
			if (++index == count)
				index = 0;
		}
		for (unsigned order = 0; order < LIVE_PAIR_PARITY && ok;
		     order++) {
			unsigned stripe = (count + order) % LIVE_PAIR_PARITY;
			if (stripe >= count)
				return;
			ok = send_packet(pacing, deadline, "R10F", seq, stamp,
					 sizes[0], sizes[1], stripe,
					 parity[stripe], LIVE_CHUNK);
		}
	} else {
		for (u32 off = 0; ok && (off < sizes[0] || off < sizes[1]);
		     off += LIVE_CHUNK)
			for (unsigned sc = 0; sc < 2 && ok; sc++) {
				if (off >= sizes[sc])
					return;
				u32 len = sizes[sc] - off > LIVE_CHUNK
					      ? LIVE_CHUNK
					      : sizes[sc] - off;
				u8 *stripe =
				    parity[sc * LIVE_PARITY +
					   off / LIVE_CHUNK % LIVE_PARITY];
				for (u32 j = 0; j < len; j++)
					stripe[j] ^= sending[sc][off + j];
				ok = send_packet(pacing, deadline, "R3P2", seq,
						 stamp, sc, sizes[sc], off,
						 sending[sc] + off, len);
			}
		for (unsigned stripe = 0; stripe < LIVE_PARITY && ok; stripe++)
			for (unsigned sc = 0; sc < 2 && ok; sc++) {
				if (stripe >= screenChunks[sc])
					return;
				ok = send_packet(
				    pacing, deadline, "R3F2", seq, stamp, sc,
				    sizes[sc], stripe,
				    parity[sc * LIVE_PARITY + stripe],
				    LIVE_CHUNK);
			}
	}
	stream.runtime.send_active_ticks += svcGetSystemTick() - begin;
	cached->complete = ok && shared;
	if (ok) {
		if (!*sent_frame || *last_sent_seq != seq) {
			if (sizes[0])
				stream.counters.frames[0]++;
			if (sizes[1])
				stream.counters.frames[1]++;
		}
		*sent_frame = true;
		*last_sent_seq = seq;
		live_timing_finish(TIMING_SENT);
	} else
		stream.counters.expired++;
}

static void sender(void *unused)
{
	(void)unused;
	PacketPacing pacing = {.totalDue = svcGetSystemTick()};
	bool sentFrame = false;
	u32 lastSentSeq = 0;
	while (!stopped()) {
		control(&pacing.totalDue);
		if (!stream.net.media_ready || capture_paused()) {
			svcSleepThread(10000000);
			return;
		}
		send_audio(&pacing.totalDue);
		struct tx_frame tx;

		if (!sender_prepare(&tx)) {
			if (!send_repair(&pacing.totalDue, svcGetSystemTick(),
					 0))
				svcSleepThread(500000);
			return;
		}
		sender_send_frame(&tx, &pacing, &sentFrame, &lastSentSeq);
	}
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
	    .pair_payload_budget = pair_payload_budget(
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
	if (sc < 0 || sc > 1 || !stream.runtime.worker || stopped() ||
	    capture_paused() || wide)
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
	if (!stream.runtime.worker || stopped())
		return;
	LightLock_Lock(&stream.runtime.lock);
	stream.runtime.plan_stamp = stamp;
	stream.runtime.plan_pts[0] = top;
	stream.runtime.plan_pts[1] = bottom;
	stream.runtime.plan_mask = mask;
	LightLock_Unlock(&stream.runtime.lock);
}
