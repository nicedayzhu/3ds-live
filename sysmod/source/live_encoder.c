/*
 * Video encoding pipeline: raw snapshot, JPEG pair encoding, row reuse and
 * R20 publication.
 */
#include "live_stream_internal.h"
#include "live_stream.h"

#include "live_jpeg.h"
#include "live_jpeg_rows.h"
#include "live_jpeg_source.h"
#include "live_jpeg_wire.h"
#include "live_pacing.h"
#include "live_pair.h"
#include "live_row_refresh.h"
#include "live_timing.h"

#include <string.h>

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
		return !live_stream_stopped();
	}
	__atomic_store_n(&stream.runtime.job_done, 0, __ATOMIC_RELEASE);
	LightEvent_Signal(&stream.runtime.events[1]);
	while (!__atomic_load_n(&stream.runtime.job_done, __ATOMIC_ACQUIRE) &&
	       !live_stream_stopped())
		LightEvent_WaitTimeout(&stream.runtime.events[2], 1000000);
	return !live_stream_stopped();
}

static bool run_pair_job(unsigned kind, unsigned q)
{
	stream.runtime.job_kind = kind;
	stream.runtime.job_quality = q;
	__atomic_store_n(&stream.runtime.job_done, 0, __ATOMIC_RELEASE);
	LightEvent_Signal(&stream.runtime.events[1]);
	encode_job(0, kind, q);
	while (!__atomic_load_n(&stream.runtime.job_done, __ATOMIC_ACQUIRE) &&
	       !live_stream_stopped())
		LightEvent_WaitTimeout(&stream.runtime.events[2], 1000000);
	return !live_stream_stopped();
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

u32 live_stream_pair_budget(u32 byteRate, u32 hz, bool combined,
			    unsigned targetQ)
{
	u32 videoRate =
	    live_av_video_rate(byteRate, stream.settings.audio_enabled);
	return live_quality_budget(videoRate, hz, combined, targetQ);
}

/* Called by the network thread with stream.runtime.lock held, between frame
 * sends. A byte ceiling changes the allowable JPEG size, not the meaning of its
 * fragments. */
void live_stream_configure(u32 hz, u32 bps, bool fec, unsigned targetQ)
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
	u32 budget = live_stream_pair_budget(bps, hz, fec, targetQ);
	unsigned count = stream.pending.count, head = stream.pending.head;
	for (unsigned i = 0; i < count; i++) {
		unsigned slot = head ^ i;
		if (!live_pending_compatible(&stream.pending.slots[slot],
					     stream.settings.config_epoch,
					     budget))
			live_stream_discard_pending(slot, TIMING_RECONFIGURED);
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
		live_stream_discard_pending(stream.pending.head,
					    TIMING_REPLACED);
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
			budget = live_stream_pair_budget(
			    e->job_rate, e->job_fps, e->combined, e->target_q);
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
		if (next_q == q || live_stream_capture_paused())
			break;
		stream.counters.retries++;
		q = next_q;
		if (!run_pair_job(2, q))
			break;
		live_timing_encoded();
	}
	if (live_stream_stopped())
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
	current_budget = live_stream_pair_budget(
	    stream.settings.rate, e->job_fps, e->combined, e->target_q);
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
			live_stream_discard_pending(stream.pending.head,
						    TIMING_REPLACED);
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
	while (!live_stream_stopped()) {
		LightEvent_WaitTimeout(&stream.runtime.events[1], 100000000);
		if (live_stream_stopped())
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

void encoder(void *arg)
{
	if ((uintptr_t)arg) {
		encoder_bottom_worker();
		return;
	}
	u64 next = svcGetSystemTick();
	LiveConsumedPlan consumed = {0};
	u32 budgetEpoch = 0, budgetRate = 0;
	unsigned previousTarget = 0;
	while (!live_stream_stopped()) {
		if (!live_peer_ready() || live_stream_capture_paused()) {
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
		if (live_stream_stopped())
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
		    live_stream_pair_budget(jobRate, jobFps, combined, targetQ);
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
