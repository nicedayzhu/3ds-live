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

/* Encoder 0 coordinates pairs, encoder 1 handles bottom. The socket worker
 * owns the socket and claims queued pairs in capture order. */
#define MAX_JPEG LIVE_JPEG_MAX_BYTES
static LiveJpegSource *rawSources[2];
static LiveJpegFrame *rawWorking[2];
static size_t rawBytes[2];
static _Alignas(LZ4_stream_t) u8 encoded[2][MAX_JPEG];
static u8 pendingBytes[2 * MAX_JPEG], sendArena[2 * MAX_JPEG];
static LivePendingQueue pending;
static LiveRepairHistory repairHistory;
static struct live_repair_tx repair_tx;
static u32 repairRequests;
static u64 repairInterest, repairPeerAt, lastRepairPoll, sendActiveTicks;
/* The socket worker can pick up a NACK inside a send slot. A different
 * control packet is deferred intact until the slot has finished. */
static struct {
	int size;
	struct sockaddr_in from;
	u8 bytes[64];
} deferredControl;
static bool rawValid[2], enabled, socReady, compactJpeg, audioEnabled,
    combinedFec;
static volatile bool stopping;
static u32 rawPts[2], planPts[2], planStamp, planMask;
static u32 sequence, session;
static u64 started;
static LightLock lock, rawLocks[2];
static LightEvent events[3];
static Thread worker, encoders[2];
static s32 encoderCores[2];
static int lastErrno;
#define SOC_ARENA_BYTES 0x100000u
static LiveReceiverFeedback receiverFeedback;
static bool receiverVideo;
static bool capturePaused;
static struct live_net net = {.fd = -1};
static u32 fps = 30, rate = 800000, configEpoch, configSequence;
static unsigned qualityTarget;
static unsigned quality = 60, spare;
static LivePayloadControl payloadControl;
static u32 jobKind, jobQuality, jobDone, jobSize[2], jobChanged[2], jobUs[2];
/* Bit 0 = top screen, bit 1 = bottom screen. A single-screen mask skips the
 * hidden encoder entirely and sends per-screen packets, so both ARM11 time and
 * airtime are released instead of rendering a panel nobody displays. */
static u32 screenMask = 3;
static u32 attempts[2], jpegFrames[2], maxUs[2], maxBytes[2], convertCount[2],
    convertMaxUs[2];
static u64 sumUs[2], convertSumUs[2], changedMcus[2];
static u64 convertedMcus[2], snapshotSumUs;
static u32 snapshotCount, snapshotMaxUs;
static u32 sourceReuse[2], lastSource[2], frames[2], replaced, errors, expired,
    oversize, retries, pairs;
static u32 sendCalls, sendMaxUs, lastEncodeUs[2], feedbackCount, configChanges;
/* Socket-thread owned. Mode 0 is the original submission-start byte clock. */
static unsigned pacingMode = 1;
static LivePacketReport packetReport;
static u32 ratePendingKept, rateEncodingKept;
static u64 sendSumUs, lastStatusTick, lastControlTick, videoWireBytes;
static u64 compactSavedBytes;
static u32 compactFrames, compactFallbacks;
/* Each optional reference bank holds up to 180 KiB. Larger pairs can still
 * travel as independent JPEGs within the per-screen and receive limits. */
static struct row_anchor *rowBanks, *rowBase, *rowCandidate, *rowProbe;
/* Only the encoder builds/reuses this dictionary. Acknowledging a new
 * reference changes rowBase, but never modifies either bank's bytes. */
static struct live_r20 r20;
_Static_assert(MAX_JPEG % _Alignof(LZ4_stream_t) == 0 &&
		   ROW_PAIR_BYTES % _Alignof(LZ4_stream_t) == 0,
	       "LZ4 workspaces at arena ends must be aligned");
static LiveRowTrial rowTrial;
static u64 probeAfter;
static bool rowsEnabled;
static unsigned rowSends;
static u64 rowLastKey, rowLastAck, rowBytesSaved;
static u32 rowKeys, rowRepeats, rowDeltas, rowAcks;
static u32 manualRenderFailures, manualWireDrops;
static struct live_audio_sender audio;
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
	return __atomic_load_n(&stopping, __ATOMIC_ACQUIRE);
}
static bool capture_paused(void)
{
	return __atomic_load_n(&capturePaused, __ATOMIC_ACQUIRE);
}

static void discard_pending(unsigned slot, LiveTimingOutcome why)
{
	(void)why;
	live_timing_drop_pending(slot, why);
	live_pending_remove(&pending, slot);
}

static void clear_pending(void)
{
	while (pending.count)
		discard_pending(pending.head, TIMING_RECONFIGURED);
}

void live_set_capture_paused(bool paused)
{
	LightLock_Lock(&lock);
	if (paused && !capture_paused()) {
		// Reject a JPEG still being encoded and forget the previous
		// display plan.
		++configEpoch;
		planMask = 0;
		clear_pending();
	}
	__atomic_store_n(&capturePaused, paused, __ATOMIC_RELEASE);
	LightLock_Unlock(&lock);
}
bool live_requested(void)
{
	struct live_config config;

	if (worker)
		return enabled;

	if (!live_config_load(&config, LIVE_CONFIG_PATH))
		return false;

	enabled = config.enabled;
	compactJpeg = config.compact_jpeg;
	audioEnabled = config.audio_enabled;
	combinedFec = config.combined_fec;
	fps = config.fps;
	rate = config.rate;

	memset(&net.target, 0, sizeof(net.target));
	net.target.sin_family = AF_INET;
	net.automatic_host = config.automatic_host;
	if (!net.automatic_host &&
	    inet_pton(AF_INET, config.host, &net.target.sin_addr) != 1)
		enabled = false;
	net.target.sin_port = htons(config.port);

	if (!live_config_valid(&config))
		enabled = false;

	return enabled;
}

u32 live_fps(void)
{
	return __atomic_load_n(&fps, __ATOMIC_RELAXED);
}
bool live_peer_ready(void)
{
	return !stopped() &&
	       __atomic_load_n(&net.media_ready, __ATOMIC_ACQUIRE);
}
bool live_receiver_has_video(void)
{
	return !stopped() && __atomic_load_n(&receiverVideo, __ATOMIC_ACQUIRE);
}
u32 live_encoded_frames(unsigned sc)
{
	return sc < 2 ? __atomic_load_n(&jpegFrames[sc], __ATOMIC_RELAXED) : 0;
}
bool live_audio_active(void)
{
	return audioEnabled && worker && !stopped() && !capture_paused();
}
bool live_audio_submit(const s16 *pcm, u32 pts)
{
	if (!pcm || !live_audio_active() || !live_peer_ready())
		return false;
	return live_audio_sender_push(&audio, pcm, pts, svcGetSystemTick());
}
static void encode_job(unsigned sc, unsigned kind, unsigned q)
{
	u64 begin = svcGetSystemTick();
	if (kind == 1) {
		int changed =
		    live_jpeg_prepare_yuv(sc, rawWorking[sc], rawBytes[sc]);
		if (changed < 0) {
			jobChanged[sc] = 0;
			jobSize[sc] = 0;
			return;
		}
		jobChanged[sc] = (u32)changed;
	}
	jobSize[sc] = (u32)live_jpeg_render(sc, q, encoded[sc], MAX_JPEG);
	jobUs[sc] +=
	    (u32)((svcGetSystemTick() - begin) * 1000000 / SYSCLOCK_ARM11);
}
static bool run_single_job(unsigned kind, unsigned q, unsigned sc)
{
	jobKind = kind;
	jobQuality = q;
	if (sc == 0) {
		encode_job(0, kind, q);
		return !stopped();
	}
	__atomic_store_n(&jobDone, 0, __ATOMIC_RELEASE);
	LightEvent_Signal(&events[1]);
	while (!__atomic_load_n(&jobDone, __ATOMIC_ACQUIRE) && !stopped())
		LightEvent_WaitTimeout(&events[2], 1000000);
	return !stopped();
}

static bool run_pair_job(unsigned kind, unsigned q)
{
	jobKind = kind;
	jobQuality = q;
	__atomic_store_n(&jobDone, 0, __ATOMIC_RELEASE);
	LightEvent_Signal(&events[1]);
	encode_job(0, kind, q);
	while (!__atomic_load_n(&jobDone, __ATOMIC_ACQUIRE) && !stopped())
		LightEvent_WaitTimeout(&events[2], 1000000);
	return !stopped();
}
static u32 payload_size(unsigned screen, unsigned q, bool combined)
{
	u32 size = jobSize[screen];
	/* Budget exactly what packing will send, including a full-JPEG
	 * fallback. */
	if (combined && compactJpeg && live_jpeg_wire_size(size) &&
	    encoded[screen][size - 2] == 0xff &&
	    encoded[screen][size - 1] == 0xd9 &&
	    live_jpeg_wire_prefix_valid(encoded[screen], screen ? 320 : 400, q))
		return (u32)live_jpeg_wire_size(size);
	return size ? size : MAX_JPEG + 1;
}
static u32 pack_panel(unsigned sc, unsigned q, u8 *out, u32 capacity)
{
	size_t packed =
	    compactJpeg ? live_jpeg_wire_pack(out, capacity, encoded[sc],
					      jobSize[sc], sc ? 320 : 400, q)
			: 0;
	if (packed) {
		compactFrames++;
		compactSavedBytes += jobSize[sc] - packed;
		return (u32)packed;
	}
	memcpy(out, encoded[sc], jobSize[sc]);
	if (compactJpeg)
		compactFallbacks++;
	return jobSize[sc];
}

static u32 row_size(const struct row_pair_plan *plan, unsigned sc,
		    const struct row_anchor *ref)
{
	size_t delta =
	    live_jpeg_rows_pack(NULL, 0, encoded[sc], plan->compact[sc],
				&plan->rows[sc], row_anchor_panel(ref, sc),
				ref->length[sc], &ref->rows[sc], ref->seq);
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
		plan->compact[sc] =
		    live_jpeg_wire_pack(encoded[sc], MAX_JPEG, encoded[sc],
					jobSize[sc], sc ? 320 : 400, qs);
		plan->wire[sc] =
		    plan->compact[sc]
			? plan->compact[sc]
			: (jobSize[sc] ? jobSize[sc] : MAX_JPEG + 1);
		if (!plan->compact[sc] ||
		    !live_jpeg_rows_parse(&plan->rows[sc], encoded[sc],
					  plan->compact[sc]))
			plan->valid = false;
	}
	if (plan->valid)
		for (unsigned sc = 0; sc < 2; sc++) {
			/* A decimated bottom must be byte-identical between due
			 * frames so the PC can skip decoding it; row deltas
			 * would carry a new sequence each pair. */
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
	live_r20_forget(&r20, ref);
	ref->seq = seq;
	ref->stamp = stamp;
	for (unsigned sc = 0; sc < 2; sc++) {
		ref->length[sc] = plan->compact[sc];
		ref->rows[sc] = plan->rows[sc];
		memcpy(ref->bytes + (sc ? ref->length[0] : 0), encoded[sc],
		       ref->length[sc]);
	}
}

/* Called under the publication lock. A probe uses the existing spare bank
 * and never goes on the network. The acknowledged base remains immutable
 * until the PC acknowledges a real candidate. */
static bool needs_key(const struct row_pair_plan *plan, u64 now, u32 seq,
		      u32 stamp, u32 budget)
{
	u32 full = plan->compact[0] + plan->compact[1];
	if (!plan->valid || full > ROW_PAIR_BYTES || full > budget)
		return false;
	if (rowCandidate && now - rowLastKey < SYSCLOCK_ARM11 / 2)
		return false;
	if (!rowBase)
		return true; /* Bootstrap and explicit receiver reset. */
	if (now - rowLastKey < SYSCLOCK_ARM11 || now < probeAfter)
		return false;
	u32 wire = plan->wire[0] + plan->wire[1];
	if (!rowProbe) {
		/* Two chunks or less of remaining JPEG offer little to reclaim.
		 */
		if (wire <= 2 * LIVE_CHUNK)
			return false;
		rowCandidate =
		    NULL; /* The previous unacknowledged offer has expired. */
		rowProbe = rowBase == rowBanks ? rowBanks + 1 : rowBanks;
		store_row_anchor(rowProbe, plan, seq, stamp);
		rowTrial = (LiveRowTrial){0};
		return false;
	}
	if (!plan->trial_size)
		return false;
	int decision = live_row_trial_observe(
	    &rowTrial, live_combined_wire(wire),
	    live_combined_wire(plan->trial_size), live_combined_wire(full));
	if (!decision)
		return false;
	rowProbe = NULL;
	probeAfter = now + SYSCLOCK_ARM11 / 4;
	return decision > 0;
}

/* Called with lock held. Full keys are eligible only when the complete wire
 * representation also fits the requested budget. Stale candidates are
 * replaced with a fresh capture rather than freezing the stream for an ACK. */
static void publish_rows(const struct row_pair_plan *plan,
			 const struct row_anchor *base, LivePendingPair *pair,
			 bool key, u64 now)
{
	if (key) {
		rowCandidate = rowBase == rowBanks ? rowBanks + 1 : rowBanks;
		store_row_anchor(rowCandidate, plan, pair->seq, pair->stamp);
		for (unsigned sc = 0; sc < 2; sc++)
			memcpy(rowCandidate->bytes +
				   (sc ? rowCandidate->length[0] : 0),
			       "R17K", 4);
		rowSends = 0;
		rowLastKey = now;
		rowKeys++;
	}
	for (unsigned sc = 0; sc < 2; sc++) {
		u8 *out =
		    pendingBytes + pair->offset + (sc ? pair->length[0] : 0);
		if (plan->compact[sc]) {
			compactFrames++;
			compactSavedBytes += jobSize[sc] - plan->compact[sc];
		} else
			compactFallbacks++;
		if (key) {
			memcpy(out, row_anchor_panel(rowCandidate, sc),
			       pair->length[sc]);
		} else if (sc == 1 && plan->bottom_delta) {
			memcpy(out,
			       plan->bottom_compressed ? plan->bottom_compressed
						       : plan->bottom_delta,
			       pair->length[sc]);
			rowDeltas++;
			rowBytesSaved +=
			    plan->compact[sc] - plan->bottom_delta_bytes;
			if (plan->bottom_compressed) {
				r20.panels++;
				r20.saved +=
				    plan->bottom_delta_bytes - pair->length[sc];
			}
		} else if (base && plan->compact[sc] &&
			   plan->wire[sc] < plan->compact[sc]) {
			pair->length[sc] = live_jpeg_rows_pack(
			    out, pair->length[sc], encoded[sc],
			    plan->compact[sc], &plan->rows[sc],
			    row_anchor_panel(base, sc), base->length[sc],
			    &base->rows[sc], base->seq);
			rowDeltas++;
			rowBytesSaved += plan->compact[sc] - pair->length[sc];
		} else {
			memcpy(out, encoded[sc], pair->length[sc]);
		}
	}
}

static u32 pair_payload_budget(u32 byteRate, u32 hz, bool combined,
			       unsigned targetQ)
{
	u32 videoRate = live_av_video_rate(byteRate, audioEnabled);
	return live_quality_budget(videoRate, hz, combined, targetQ);
}

/* Called by the network thread with lock held, between frame sends. A byte
 * ceiling changes the allowable JPEG size, not the meaning of its fragments. */
static void configure_stream(u32 hz, u32 bps, bool fec, unsigned targetQ)
{
	bool formatChanged =
	    fps != hz || combinedFec != fec || qualityTarget != targetQ;
	if (!formatChanged && rate == bps)
		return;
	if (formatChanged) {
		configEpoch++;
		rowBase = rowCandidate = rowProbe = NULL;
		live_repair_clear(&repairHistory);
		repair_tx.credit = 0;
	}
	u32 budget = pair_payload_budget(bps, hz, fec, targetQ);
	unsigned count = pending.count, head = pending.head;
	for (unsigned i = 0; i < count; i++) {
		unsigned slot = head ^ i;
		if (!live_pending_compatible(&pending.slots[slot], configEpoch,
					     budget))
			discard_pending(slot, TIMING_RECONFIGURED);
		else
			ratePendingKept++;
	}
	__atomic_store_n(&fps, hz, __ATOMIC_RELAXED);
	rate = bps;
	combinedFec = fec;
	qualityTarget = targetQ;
	configChanges++;
	/* A new quality/rate invalidates the reused bottom JPEG. */
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
	const unsigned sc = (screenMask & 1) ? 0 : 1;
	size_t packed;
	u32 wire;
	LivePendingPair incoming;
	u32 bytes;
	unsigned slot;
	LivePendingPair *pair;

	if (!run_single_job(1, q, sc))
		return;
	live_timing_encoded();
	packed = compactJpeg
		     ? live_jpeg_wire_pack(encoded[sc], MAX_JPEG, encoded[sc],
					   jobSize[sc], sc ? 320 : 400, q)
		     : 0;
	wire = packed ? (u32)packed : jobSize[sc];
	if (!jobSize[sc] || wire > budget) {
		oversize++;
		return;
	}
	incoming = (LivePendingPair){
	    .length = {sc == 0 ? wire : 0, sc == 1 ? wire : 0},
	    .seq = seq,
	    .stamp = stamp,
	    .epoch = epoch,
	    .r20 = false};
	bytes = live_pending_bytes(&incoming);
	while (!live_pending_fits(&pending, bytes, sizeof pendingBytes)) {
		discard_pending(pending.head, TIMING_REPLACED);
		replaced++;
	}
	slot = live_pending_append(&pending, pendingBytes, sizeof pendingBytes,
				   incoming);
	pair = &pending.slots[slot];
	memcpy(pendingBytes + pair->offset, encoded[sc], wire);
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

		LightLock_Lock(&lock);
		current_rate = rate;
		LightLock_Unlock(&lock);
		if (current_rate != e->job_rate) {
			e->job_rate = current_rate;
			budget = pair_payload_budget(e->job_rate, e->job_fps,
						     e->combined, e->target_q);
			target_bytes = budget * payloadControl.percent / 100;
		}
		measure_pair(plan, q, e->combined || e->target_q, e->use_rows,
			     e->base, e->probe, e->bottom_rows);
		if (e->use_r20 && plan->valid)
			live_r20_compress(&r20, plan, e->base, e->next,
					  encoded[0], encoded[1], MAX_JPEG);
		total = plan->wire[0] + plan->wire[1];
		if (jobSize[0] && jobSize[1] && total <= budget)
			break;
		next_q =
		    live_retry_quality(e->target_q, q, retry, total, budget);
		if (next_q == q || capture_paused())
			break;
		retries++;
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
	bool fits = jobSize[0] && jobSize[1] && payload_total <= budget;
	uint32_t current_budget;

	for (unsigned i = 0; i < 2; i++) {
		attempts[i]++;
		sumUs[i] += jobUs[i];
		changedMcus[i] += jobChanged[i];
		if (jobUs[i] > maxUs[i])
			maxUs[i] = jobUs[i];
		__atomic_store_n(&lastEncodeUs[i], jobUs[i], __ATOMIC_RELAXED);
		if (fits) {
			__atomic_add_fetch(&jpegFrames[i], 1, __ATOMIC_RELAXED);
			if (jobSize[i] > maxBytes[i])
				maxBytes[i] = jobSize[i];
		}
	}
	quality = q;
	if (!fits) {
		if (e->target_q) {
			if (!jobSize[0] || !jobSize[1])
				manualRenderFailures++;
			else
				manualWireDrops++;
		}
		live_timing_drop_encoded(TIMING_OVERSIZE);
		oversize++;
		spare = 0;
		return;
	}
	if (!e->target_q) {
		if (payload_total > target_bytes) {
			quality =
			    live_lower_quality(q, payload_total, target_bytes);
			spare = 0;
		} else if (payload_total < target_bytes * 9 / 10) {
			if (++spare >= 4) {
				quality = quality < 83 ? quality + 2 : 85;
				spare = 0;
			}
		} else {
			spare = 0;
		}
	}
	LightLock_Lock(&lock);
	current_budget =
	    pair_payload_budget(rate, e->job_fps, e->combined, e->target_q);
	if (plan->bottom_compressed &&
	    !live_r20_recent(&r20, svcGetSystemTick())) {
		plan->bottom_compressed = NULL;
		plan->wire[1] = plan->bottom_delta_bytes;
		payload_total = plan->wire[0] + plan->wire[1];
	}
	if (e->epoch == configEpoch && (!e->base || e->base == rowBase) &&
	    payload_total <= current_budget &&
	    payload_total <= sizeof pendingBytes) {
		u64 now;
		bool key;
		LivePendingPair incoming;
		uint32_t bytes;
		bool overflow;
		unsigned slot;
		LivePendingPair *pair;

		if (e->initial_rate != rate)
			rateEncodingKept++;
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
		    !live_pending_fits(&pending, bytes, sizeof pendingBytes);
		while (
		    !live_pending_fits(&pending, bytes, sizeof pendingBytes)) {
			discard_pending(pending.head, TIMING_REPLACED);
			replaced++;
		}
		live_payload_observe(&payloadControl, e->job_fps, overflow);
		slot = live_pending_append(&pending, pendingBytes,
					   sizeof pendingBytes, incoming);
		pair = &pending.slots[slot];
		if (e->use_rows) {
			publish_rows(plan, e->base, pair, key, now);
		} else {
			u8 *out = pendingBytes + pair->offset;

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
	LightLock_Unlock(&lock);
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

static void encoder(void *arg)
{
	if ((uintptr_t)arg) {
		while (!stopped()) {
			LightEvent_WaitTimeout(&events[1], 100000000);
			if (stopped())
				break;
			if (__atomic_load_n(&jobDone, __ATOMIC_ACQUIRE))
				continue;
			encode_job(1, jobKind, jobQuality);
			__atomic_store_n(&jobDone, 1, __ATOMIC_RELEASE);
			LightEvent_Signal(&events[2]);
		}
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
		bool ready;
		u32 stamp = 0, source[2] = {0, 0}, wanted[2], jobRate, jobFps,
		    epoch;
		LightLock_Lock(&lock);
		stamp = planStamp;
		ready = planMask == 3 && live_plan_available(&consumed, stamp);
		wanted[0] = planPts[0];
		wanted[1] = planPts[1];
		jobRate = rate;
		jobFps = fps;
		epoch = configEpoch;
		bool combined = combinedFec && screenMask == 3;
		unsigned targetQ = qualityTarget;
		bool useRows = rowsEnabled && combined && compactJpeg;
		bool bottomRows = combined;
		bool useR20 =
		    useRows && bottomRows && live_r20_recent(&r20, now);
		struct row_anchor *base = useRows ? rowBase : NULL;
		struct row_anchor *probe = useRows ? rowProbe : NULL;
		LightLock_Unlock(&lock);
		if (stopped())
			break;
		/* The capture thread converts directly into each pending
		 * buffer.  A per-screen lock keeps that relatively long
		 * operation off the socket and configuration lock while
		 * preserving one complete pair here. */
		u64 snapshotBegin = svcGetSystemTick();
		for (unsigned i = 0; ready && i < 2; i++) {
			LightLock_Lock(&rawLocks[i]);
			if (!rawValid[i] || rawPts[i] != wanted[i])
				ready = false;
			else {
				memcpy(rawWorking[i],
				       live_jpeg_source_frame(rawSources[i]),
				       rawBytes[i]);
				source[i] = rawPts[i];
			}
			LightLock_Unlock(&rawLocks[i]);
		}
		if (!ready) {
			LightEvent_WaitTimeout(&events[0], 1000000);
			continue;
		}
		live_plan_consume(&consumed, stamp);
		u32 snapshotUs = (u32)((svcGetSystemTick() - snapshotBegin) *
				       1000000 / SYSCLOCK_ARM11);
		snapshotCount++;
		snapshotSumUs += snapshotUs;
		if (snapshotUs > snapshotMaxUs)
			snapshotMaxUs = snapshotUs;
		next = live_next_encode_tick(next, now, SYSCLOCK_ARM11, jobFps);
		const u32 initialRate = jobRate;
		u32 budget =
		    pair_payload_budget(jobRate, jobFps, combined, targetQ);
		if (budgetEpoch != epoch || budgetRate != jobRate) {
			payloadControl = (LivePayloadControl){.percent = 100};
			spare = 0;
			budgetEpoch = epoch;
			budgetRate = jobRate;
			if (targetQ)
				quality = targetQ;
			else if (previousTarget)
				quality = 60;
			previousTarget = targetQ;
		}
		u32 targetBytes = budget * payloadControl.percent / 100;
		for (unsigned i = 0; i < 2; i++) {
			sourceReuse[i] += pairs && source[i] == lastSource[i];
			lastSource[i] = source[i];
			jobUs[i] = 0;
		}
		pairs++;
		u32 seq = ++sequence;
		unsigned q = targetQ ? targetQ : quality;
		live_timing_begin(seq, stamp);
		if (screenMask != 3) {
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
		live_packet_observe_sleep(&packetReport, start, due, end);
		start = end;
	}
}

static void pacing_charge(u64 *due, u64 begin, u64 returned, u32 wire,
			  enum LivePacketKind kind)
{
	u64 base = begin > *due ? begin : *due;
	u64 byteDue = live_packet_due(*due, begin, wire, rate, SYSCLOCK_ARM11);
	*due = live_packet_return_due(byteDue, returned, byteDue - base,
				      SYSCLOCK_ARM11, pacingMode);
	live_packet_observe(&packetReport, kind, begin, returned,
			    *due - byteDue, SYSCLOCK_ARM11);
}

/* Only the network thread calls this. Audio is serviced between video
 * datagrams, not after a whole 150 ms video send. Previous-batch redundancy
 * repairs one lost audio datagram without a retransmission round trip. */
static void audio_pacing_charge(u64 *due, u64 begin, u64 returned, u32 wire,
				int kind)
{
	pacing_charge(due, begin, returned, wire, (enum LivePacketKind)kind);
}

static void send_audio(u64 *due)
{
	live_audio_sender_send(&audio, session, audioEnabled, stopped(), net.fd,
			       &net.target, due, pacing_sleep_until,
			       audio_pacing_charge);
}

static size_t network_report_text(char *text, size_t capacity)
{
	struct live_net_report snapshot;

	live_net_snapshot(&net, session, &snapshot);
	return live_net_report_format(text, capacity, &snapshot);
}

/* The socket worker owns the live counters; publish one complete copy. */
static void queue_network_report(void)
{
	struct live_net_report snapshot;

	live_net_snapshot(&net, session, &snapshot);
	live_net_report_queue(&snapshot);
	net.diag.saved_at = svcGetSystemTick();
}

void live_flush_network_report(bool force)
{
	live_net_report_flush(force);
}

static void reset_receiver_feedback(void)
{
	receiverFeedback = (LiveReceiverFeedback){0};
	__atomic_store_n(&receiverVideo, false, __ATOMIC_RELEASE);
}

static void row_peer(bool enable, bool forget)
{
	/* Called with lock held. An encode begun under the previous negotiation
	 * must not publish a delta after a peer forgets its base. */
	if (enable == rowsEnabled && !(forget && rowBase))
		return;
	rowsEnabled = enable;
	rowBase = rowCandidate = rowProbe = NULL;
	configEpoch++;
	clear_pending();
}

static void reset_peer_config(void)
{
	pacingMode = 1;
	packetReport.flags = 0;
	live_repair_clear(&repairHistory);
	repairInterest = repairPeerAt = repair_tx.credit = 0;
	deferredControl.size = 0;
	net.last_peer_control = 0;
	rowLastAck = 0;
	configSequence = 0;
	LightLock_Lock(&lock);
	r20.last_ack = 0;
	if (rowsEnabled)
		row_peer(false, false);
	else {
		++configEpoch;
		clear_pending();
	}
	LightLock_Unlock(&lock);
}

static u64 send_status(u64 *due, const u8 *msg, u32 size)
{
	pacing_sleep_until(*due);
	u64 begin = svcGetSystemTick();
	sendto(net.fd, msg, size, 0, (struct sockaddr *)&net.target,
	       sizeof net.target);
	pacing_charge(due, begin, svcGetSystemTick(), size + 28, PACKET_STATUS);
	return begin;
}

/* Call only after address, port, session and control payload validation. */
static void peer_activity(u64 now)
{
	net.last_peer_control = now;
	if (net.automatic_host)
		live_discover_activity(&net.discovery,
				       ntohl(net.target.sin_addr.s_addr), now);
}

static void accept_repair(const u8 *msg, int size, u64 now)
{
	if (!combinedFec || size <= 20 || size > 20 + (int)LIVE_REPAIR_MAX ||
	    get32(msg + 4) != session)
		return;
	u32 n = get32(msg + 16);
	if (n != (u32)size - 20)
		return;
	repairInterest = now;
	if (live_repair_request(&repairHistory, get32(msg + 8), get32(msg + 12),
				msg + 20, n, LIVE_CHUNK, now,
				(u64)SYSCLOCK_ARM11 * LIVE_REPAIR_SERVICE_MS /
				    1000)) {
		peer_activity(now);
		repairRequests++;
		packetReport.nack = (LivePacketSample){.seq = get32(msg + 8),
						       .pts = get32(msg + 12),
						       .begin = now};
		packetReport.flags |= 2;
	}
}

static void poll_frame_feedback(u64 now)
{
	if (!repairInterest || now - repairInterest > 2ull * SYSCLOCK_ARM11 ||
	    deferredControl.size || now - lastRepairPoll < SYSCLOCK_ARM11 / 250)
		return;
	lastRepairPoll = now;
	struct sockaddr_in from;
	socklen_t size = sizeof from;
	int n = recvfrom(net.fd, deferredControl.bytes,
			 sizeof deferredControl.bytes, 0,
			 (struct sockaddr *)&from, &size);
	if (n <= 0 || size < sizeof from || from.sin_family != AF_INET ||
	    from.sin_port != net.target.sin_port ||
	    from.sin_addr.s_addr != net.target.sin_addr.s_addr)
		return;
	if (n >= 4 && !memcmp(deferredControl.bytes, "R19N", 4))
		accept_repair(deferredControl.bytes, n, now);
	else {
		deferredControl.from = from;
		deferredControl.size = n;
	}
}

/* Called with the pair lock held. Changing the mask invalidates every queued
 * pair and the row reference: both were built for the old panel set. */
static void apply_screen_mask_locked(u32 mask)
{
	if (mask == screenMask)
		return;

	screenMask = mask;
	while (pending.count)
		discard_pending(pending.head, TIMING_RECONFIGURED);
	rowBase = rowCandidate = NULL;
}

static void control_configure(const struct live_control_config *config, u64 now)
{
	if (configSequence &&
	    (u32)(config->sequence - configSequence) >= 0x80000000u)
		return;

	peer_activity(now);
	configSequence = config->sequence;
	LightLock_Lock(&lock);
	apply_screen_mask_locked(config->mask);
	configure_stream(config->fps, config->rate, config->combined_fec,
			 config->quality);
	LightLock_Unlock(&lock);
	lastStatusTick = 0;
}

static void control_set_mask(u32 mask, u64 now)
{
	peer_activity(now);
	LightLock_Lock(&lock);
	apply_screen_mask_locked(mask);
	LightLock_Unlock(&lock);
}

static void control_set_pacing(unsigned mode, u64 now)
{
	if (pacingMode != mode) {
		pacingMode = mode;
		lastStatusTick = 0;
	}
	peer_activity(now);
}

static void control_ack_r20(u64 now)
{
	if (!rowBanks || !compactJpeg || !combinedFec)
		return;
	LightLock_Lock(&lock);
	r20.last_ack = now;
	LightLock_Unlock(&lock);
	peer_activity(now);
}

static void control_repair_peer(u64 now)
{
	repairPeerAt = now;
	peer_activity(now);
}

static void control_ack_rows(u32 flags, u32 sequence, u64 now)
{
	if (!rowBanks || !compactJpeg)
		return;

	rowLastAck = now;
	peer_activity(now);
	LightLock_Lock(&lock);
	row_peer(true, !flags);
	if (flags && rowCandidate && rowCandidate->seq == sequence) {
		rowBase = rowCandidate;
		rowCandidate = NULL;
		rowProbe = NULL;
		rowAcks++;
	}
	LightLock_Unlock(&lock);
}

static void control_feedback(u32 sent, u32 transit, u64 now)
{
	peer_activity(now);
	feedbackCount++;
	live_receiver_observe(&receiverFeedback, sent, transit, now,
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
	return pacingMode != 0;
}

static void net_observe_probe(u64 begin, u64 returned, u32 wire)
{
	live_packet_observe(&packetReport, PACKET_STATUS, begin, returned, wire,
			    SYSCLOCK_ARM11);
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
	if (!net.media_ready || now - lastStatusTick < SYSCLOCK_ARM11)
		return;
	struct live_status_main status = {
	    .session = session,
	    .duration_ms = (u32)((now - started) * 1000 / SYSCLOCK_ARM11),
	    .rate = rate,
	    .video_rate = live_av_video_rate(rate, audioEnabled),
	    .fps = fps,
	    .encoded_top = live_encoded_frames(0),
	    .encoded_bottom = live_encoded_frames(1),
	    .sent_top = frames[0],
	    .sent_bottom = frames[1],
	    .send_errors = errors,
	    .expired_pairs = expired,
	    .feedback_count = feedbackCount,
	    .encoder_core_top = encoderCores[0],
	    .encoder_core_bottom = encoderCores[1],
	    .last_encode_us_top =
		__atomic_load_n(&lastEncodeUs[0], __ATOMIC_RELAXED),
	    .last_encode_us_bottom =
		__atomic_load_n(&lastEncodeUs[1], __ATOMIC_RELAXED),
	    .audio_enabled = audioEnabled,
	    .audio_blocks_queued =
		__atomic_load_n(&audio.queue.queued, __ATOMIC_RELAXED),
	    .audio_blocks_dropped =
		__atomic_load_n(&audio.queue.dropped, __ATOMIC_RELAXED) +
		__atomic_load_n(&audio.queue.expired, __ATOMIC_RELAXED),
	    .audio_packets_sent = audio.packets_sent,
	    .audio_queue_depth = live_audio_queue_depth(&audio.queue),
	};
	uint32_t pacing_words[LIVE_PACING_WORDS];
	LightLock_Lock(&lock);
	uint32_t overwritten = replaced;
	LightLock_Unlock(&lock);
	live_packet_snapshot(&packetReport, pacing_words, session,
			     status.duration_ms, pacingMode, started,
			     SYSCLOCK_ARM11);
	struct live_status_aux aux = {
	    .session = session,
	    .duration_ms = status.duration_ms,
	    .quality_target = qualityTarget,
	    .overwritten = overwritten,
	    .row_repeats = rowRepeats,
	    .repair_requests = repairRequests,
	    .repair_packets = repair_tx.packets,
	    .repair_wire_bytes = repair_tx.wire_bytes,
	    .audio_dropped = audio.queue.dropped,
	    .audio_expired = audio.queue.expired,
	    .send_active_us =
		(u32)(sendActiveTicks / SYSCLOCK_ARM11 * 1000000ull +
		      (sendActiveTicks % SYSCLOCK_ARM11) * 1000000ull /
			  SYSCLOCK_ARM11),
	    .compact_jpeg = compactJpeg,
	    .rows_enabled = rowBanks != NULL,
	    .combined_fec = combinedFec,
	    .audio_wire_bytes = audio.wire_bytes,
	    .video_wire_bytes = videoWireBytes,
	    .pacing_mode = pacingMode,
	};
	memcpy(aux.pacing_words, pacing_words, sizeof(aux.pacing_words));
	u8 burst[512];
	struct live_status_packet packets[LIVE_STATUS_MAX_PACKETS];
	unsigned packet_count =
	    live_status_build_burst(burst, sizeof(burst), &status, &aux,
				    packets, LIVE_STATUS_MAX_PACKETS);
	if (!packet_count)
		return;
	lastStatusTick =
	    send_status(due, burst + packets[0].offset, packets[0].size);
	for (unsigned i = 1; i < packet_count; i++)
		send_status(due, burst + packets[i].offset, packets[i].size);
#ifdef LIVE_FRAME_TIMING
	u32 capacity;
	if (live_timing_marker(lastStatusTick, &capacity)) {
		u8 marker[24];
		memcpy(marker, "R12D", 4);
		put32(marker + 4, session);
		put32(marker + 8, 1);
		put32(marker + 12, capacity);
		pacing_sleep_until(*due);
		u64 call = svcGetSystemTick();
		put32(marker + 16, (u32)call);
		put32(marker + 20, (u32)(call >> 32));
		sendto(net.fd, marker, sizeof marker, 0,
		       (struct sockaddr *)&net.target, sizeof net.target);
		pacing_charge(due, call, svcGetSystemTick(), sizeof marker + 28,
			      PACKET_STATUS);
	}
#endif
}

static void control_receive(u64 *due)
{
	u64 now = svcGetSystemTick();

	if (!(deferredControl.size ||
	      now - lastControlTick >= SYSCLOCK_ARM11 / 100 ||
	      now - net.last_probe_tick >= SYSCLOCK_ARM11))
		return;
	lastControlTick = now;
	for (unsigned i = 0; i < 16; i++) {
		u8 msg[64];
		struct sockaddr_in from;
		socklen_t size = sizeof from;
		int n;
		if (deferredControl.size) {
			n = deferredControl.size;
			from = deferredControl.from;
			memcpy(msg, deferredControl.bytes, n);
			deferredControl.size = 0;
		} else
			n = recvfrom(net.fd, msg, sizeof msg, 0,
				     (struct sockaddr *)&from, &size);
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				net.diag.receive_errno = errno;
			break;
		}
		now = svcGetSystemTick();
		net.diag.replies++;
		if (size < sizeof from || from.sin_family != AF_INET ||
		    from.sin_port != net.target.sin_port)
			continue;
		if (net.automatic_host && !net.discovery.peer &&
		    live_discover_invitation(msg, (size_t)n)) {
			u32 address = ntohl(from.sin_addr.s_addr);
			if (!live_reply_address(address, net.local_address,
						net.local_mask) ||
			    address == net.diag.reported_broadcast)
				continue;
			net.diag.hints++;
			if (net.diag.last_hint_tick &&
			    now - net.diag.last_hint_tick < SYSCLOCK_ARM11 / 5)
				continue;
			net.diag.last_hint_tick = now;
			u8 probe[LIVE_DISCOVERY_BYTES];
			live_discover_invited_probe(&net.discovery, session,
						    probe);
			if (live_net_send_probe(&net, probe, address, due,
						&net_ops))
				net.diag.hints_answered++;
			continue;
		}
		if (n < 8 || get32(msg + 4) != session)
			continue;
		if (net.automatic_host && n >= 4 && !memcmp(msg, "3DLA", 4)) {
			bool first = !net.discovery.peer;
			u32 address = ntohl(from.sin_addr.s_addr);
			if (live_reply_address(address, net.local_address,
					       net.local_mask) &&
			    address != net.diag.reported_broadcast &&
			    live_discover_ack(&net.discovery, address, session,
					      msg, (size_t)n, now)) {
				net.diag.accepted++;
				net.last_peer_control = now;
				if (first) {
					net.target.sin_addr = from.sin_addr;
					lastStatusTick = 0;
					__atomic_store_n(&net.media_ready, true,
							 __ATOMIC_RELEASE);
					queue_network_report();
				}
			}
			continue;
		}
		if (!net.media_ready ||
		    from.sin_addr.s_addr != net.target.sin_addr.s_addr)
			continue;
		live_control_handle(msg, n, now, screenMask, combinedFec,
				    qualityTarget, &control_ops);
	}
	const bool hasVideo =
	    live_receiver_recent(&receiverFeedback, now,
				 (u64)LIVE_PEER_LEASE_SECONDS * SYSCLOCK_ARM11);
	if (hasVideo != __atomic_load_n(&receiverVideo, __ATOMIC_RELAXED))
		__atomic_store_n(&receiverVideo, hasVideo, __ATOMIC_RELEASE);
}

static void control(u64 *due)
{
	u64 now = svcGetSystemTick();
	if (net.fd < 0)
		live_net_poll(&net, session, now, due, &net_ops);
	if (net.fd < 0)
		return;
	if (rowLastAck &&
	    now - rowLastAck >= (u64)LIVE_PEER_LEASE_SECONDS * SYSCLOCK_ARM11) {
		rowLastAck = 0;
		LightLock_Lock(&lock);
		row_peer(false, false);
		LightLock_Unlock(&lock);
	}
	control_receive(due);
	/* Consume queued acknowledgements before expiring the lease, rotating
	 * the challenge or closing the only socket that can receive them. */
	live_net_poll(&net, session, svcGetSystemTick(), due, &net_ops);
	send_status_burst(due);
}

typedef struct {
	u64 totalDue;
	LiveVideoPacing video;
	bool firstPacket;
} PacketPacing;

/* Use spare wire time between fresh packets. A repair shares the strict
 * total clock with audio and never advances the fresh video's slot
 * clock. */
static void repair_charge(u64 *due, u64 begin, u64 returned, u32 wire, int kind)
{
	pacing_charge(due, begin, returned, wire, (enum LivePacketKind)kind);
}

static void repair_report(u32 seq, u32 pts, u32 index, u64 begin, u64 returned)
{
	packetReport.repair = (LivePacketSample){.seq = seq,
						 .pts = pts,
						 .index = index,
						 .begin = begin,
						 .end = returned};
	packetReport.flags |= 4;
}

static bool send_repair(u64 *total_due, u64 now, u64 next_fresh_due)
{
	struct live_repair_io io = {
	    .fd = net.fd,
	    .target = &net.target,
	    .session = session,
	    .rate = rate,
	    .stopping = stopped,
	    .capture_paused = capture_paused,
	    .charge = repair_charge,
	    .report = repair_report,
	};
	bool sent = live_repair_tx_send(&repair_tx, &repairHistory, sendArena,
					sizeof sendArena, fps, total_due, now,
					next_fresh_due, &io);

	if (sent && repair_tx.last_wire)
		videoWireBytes += repair_tx.last_wire;
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
		if (!usedGap && repair_tx.credit && pacing->totalDue < due &&
		    now < due && live_repair_next(&repairHistory, now)) {
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
		/* A sparse video pair must not keep a ready audio batch
		 * waiting. */
		if (audioEnabled && wait > SYSCLOCK_ARM11 / 500)
			wait = SYSCLOCK_ARM11 / 500;
		pacing_sleep_until(now + wait);
	}
	const u32 header = 32;
	u8 packet[LIVE_CHUNK + 32];
	memcpy(packet, magic, 4);
	put32(packet + 4, session);
	put32(packet + 8, seq);
	put32(packet + 12, stamp);
	put32(packet + 16, sc);
	put32(packet + 20, total);
	put32(packet + 24, offset);
	put32(packet + 28, fps);
	memcpy(packet + header, data, len);
	u64 begin = svcGetSystemTick();
	int sent = sendto(net.fd, packet, len + header, 0,
			  (struct sockaddr *)&net.target, sizeof net.target);
	u64 returned = svcGetSystemTick();
	u32 us = (u32)((returned - begin) * 1000000 / SYSCLOCK_ARM11);
	live_timing_packet(begin, returned, sent == (int)(len + header));
	sendCalls++;
	sendSumUs += us;
	if (us > sendMaxUs)
		sendMaxUs = us;
	pacing_charge(&pacing->totalDue, begin, returned, len + header + 28,
		      PACKET_VIDEO);
	live_video_charge(&pacing->video, begin, len + header + 28,
			  SYSCLOCK_ARM11);
	if (sent != (int)(len + header)) {
		lastErrno = errno;
		errors++;
		live_timing_finish(TIMING_SOCKET_ERROR);
		return false;
	}
	if (pacing->firstPacket) {
		packetReport.first =
		    (LivePacketSample){seq, stamp, offset, begin, returned};
		packetReport.flags |= 1;
		pacing->firstPacket = false;
	}
	videoWireBytes += len + header + 28;
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

static bool sender_prepare(struct tx_frame *tx)
{
	bool ready;
	u64 now;

	memset(tx, 0, sizeof(*tx));
	LightLock_Lock(&lock);
	now = svcGetSystemTick();
	while (pending.count) {
		const LivePendingPair *head = &pending.slots[pending.head];

		if (head->epoch != configEpoch ||
		    (head->r20 && !live_r20_recent(&r20, now)))
			discard_pending(pending.head, TIMING_RECONFIGURED);
		else if (live_pending_expired(head, now, SYSCLOCK_ARM11, fps)) {
			discard_pending(pending.head, TIMING_TOO_OLD);
			expired++;
		} else {
			break;
		}
	}
	ready = pending.count != 0;
	if (ready) {
		unsigned slot = pending.head;
		const LivePendingPair *pair = &pending.slots[slot];
		bool selective;
		bool send_key;
		u64 reserved, until;
		static LiveRepairFrame single_frame;

		tx->seq = pair->seq;
		tx->stamp = pair->stamp;
		tx->queued = pair->queued;
		selective = repairPeerAt &&
			    now - repairPeerAt <
				(u64)LIVE_PEER_LEASE_SECONDS * SYSCLOCK_ARM11;
		send_key =
		    rowCandidate &&
		    (selective
			 ? (rowSends == 0 && rowCandidate->seq == tx->seq)
			 : rowSends < 3 && (u32)(tx->seq - rowCandidate->seq) <
					       0x80000000u);
		if (send_key &&
		    (svcGetSystemTick() - rowLastKey > SYSCLOCK_ARM11 / 4 ||
		     rowCandidate->length[0] + rowCandidate->length[1] >
			 pair_payload_budget(rate, fps, combinedFec,
					     qualityTarget))) {
			send_key = false;
			rowSends = 3;
		}
		if (send_key) {
			tx->seq = rowCandidate->seq;
			tx->stamp = rowCandidate->stamp;
			tx->repeat = rowSends++;
			if (tx->repeat)
				rowRepeats++;
		}
		if (tx->seq == pair->seq)
			live_timing_claim(slot);
		else {
			live_timing_drop_pending(slot, TIMING_REPLACED);
			replaced++;
		}
		for (unsigned sc = 0; sc < 2; sc++)
			tx->sizes[sc] = send_key ? rowCandidate->length[sc]
						 : pair->length[sc];
		reserved = svcGetSystemTick();
		until = reserved +
			(u64)SYSCLOCK_ARM11 * LIVE_REPAIR_HISTORY_MS / 1000;
		if (tx->sizes[0] && tx->sizes[1]) {
			tx->cached = live_repair_reserve(
			    &repairHistory, sizeof sendArena, tx->sizes[0],
			    tx->sizes[1], tx->seq, tx->stamp, fps, reserved,
			    until);
		} else {
			/* A single-screen stream has no second panel, so it
			 * cannot share the two-panel repair arena; send it
			 * directly without repair. */
			single_frame = (LiveRepairFrame){0};
			single_frame.top = tx->sizes[0];
			single_frame.bottom = tx->sizes[1];
			tx->cached = &single_frame;
		}
		if (tx->cached) {
			tx->sending[0] =
			    sendArena + (tx->sizes[0] && tx->sizes[1]
					     ? tx->cached->offset
					     : 0);
			tx->sending[1] = tx->sending[0] + tx->sizes[0];
		}
		for (unsigned sc = 0; tx->cached && sc < 2; sc++) {
			const u8 *panel = pendingBytes + pair->offset +
					  (sc ? pair->length[0] : 0);

			memcpy(tx->sending[sc],
			       send_key ? row_anchor_panel(rowCandidate, sc)
					: panel,
			       tx->sizes[sc]);
		}
		live_pending_remove(&pending, slot);
	}
	LightLock_Unlock(&lock);
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
		expired++;
		live_timing_finish(TIMING_TOO_OLD);
		return;
	}
	u32 screenChunks[2] = {live_data_packets(sizes[0]),
			       live_data_packets(sizes[1])};
	const bool shared = combinedFec && sizes[0] && sizes[1];
	u32 dataCount = shared ? live_data_packets(sizes[0] + sizes[1])
			       : screenChunks[0] + screenChunks[1];
	u32 wire =
	    shared ? live_combined_wire(sizes[0] + sizes[1])
		   : live_screen_wire(sizes[0]) + live_screen_wire(sizes[1]);
	repair_tx.credit =
	    shared ? live_repair_allowance(
			 live_av_video_rate(rate, audioEnabled), fps, wire)
		   : 0;
	live_video_slot(&pacing->video, queued, wire, dataCount, fps,
			live_av_video_rate(rate, audioEnabled));
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
		live_av_video_rate(rate, audioEnabled), qualityTarget != 0);
	if (begin - queued > SYSCLOCK_ARM11 / fps * 2) {
		expired++;
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
	sendActiveTicks += svcGetSystemTick() - begin;
	cached->complete = ok && shared;
	if (ok) {
		if (!*sent_frame || *last_sent_seq != seq) {
			if (sizes[0])
				frames[0]++;
			if (sizes[1])
				frames[1]++;
		}
		*sent_frame = true;
		*last_sent_seq = seq;
		live_timing_finish(TIMING_SENT);
	} else
		expired++;
}

static void sender(void *unused)
{
	(void)unused;
	PacketPacing pacing = {.totalDue = svcGetSystemTick()};
	bool sentFrame = false;
	u32 lastSentSeq = 0;
	while (!stopped()) {
		control(&pacing.totalDue);
		if (!net.media_ready || capture_paused()) {
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
	packetReport = (LivePacketReport){0};
	__atomic_store_n(&capturePaused, false, __ATOMIC_RELEASE);
	memset(rawValid, 0, sizeof rawValid);
	memset(attempts, 0, sizeof attempts);
	memset(jpegFrames, 0, sizeof jpegFrames);
	memset(maxUs, 0, sizeof maxUs);
	memset(maxBytes, 0, sizeof maxBytes);
	memset(sumUs, 0, sizeof sumUs);
	memset(convertCount, 0, sizeof convertCount);
	memset(convertMaxUs, 0, sizeof convertMaxUs);
	memset(convertSumUs, 0, sizeof convertSumUs);
	memset(convertedMcus, 0, sizeof convertedMcus);
	snapshotCount = snapshotMaxUs = 0;
	snapshotSumUs = 0;
	memset(changedMcus, 0, sizeof changedMcus);
	memset(sourceReuse, 0, sizeof sourceReuse);
	memset(lastSource, 0, sizeof lastSource);
	memset(frames, 0, sizeof frames);
	memset(lastEncodeUs, 0, sizeof lastEncodeUs);
	sequence = replaced = errors = expired = oversize = retries = pairs =
	    sendCalls = sendMaxUs = feedbackCount = 0;
	configEpoch = configSequence = configChanges = ratePendingKept =
	    rateEncodingKept = 0;
	sendSumUs = 0;
	lastErrno = 0;
	qualityTarget = 0;
	quality = 60;
	spare = 0;
	screenMask = 3;
	payloadControl = (LivePayloadControl){.percent = 100};
	compactSavedBytes = 0;
	compactFrames = compactFallbacks = 0;
	rowsEnabled = false;
	rowBase = rowCandidate = rowProbe = rowBanks = NULL;
	live_r20_init(&r20);
	rowKeys = rowRepeats = rowDeltas = rowAcks = rowSends = 0;
	manualRenderFailures = manualWireDrops = 0;
	repairHistory = (LiveRepairHistory){0};
	repairRequests = 0;
	repair_tx = (struct live_repair_tx){0};
	deferredControl.size = 0;
	lastRepairPoll = repairInterest = repairPeerAt = sendActiveTicks = 0;
	probeAfter = 0;
	rowLastKey = rowLastAck = rowBytesSaved = 0;
	live_audio_sender_init(&audio);
	videoWireBytes = 0;
	session = (u32)started;
	if (!session)
		session = 1;
	lastStatusTick = lastControlTick = started;
	net.discovery = (LiveDiscovery){.challenge = (u32)(started >> 16)};
	reset_receiver_feedback();
	net.last_probe_tick = net.last_peer_control = 0;
	net.local_address = net.local_mask = 0;
	memset(&net.diag, 0, sizeof(net.diag));
	__atomic_store_n(&net.media_ready, false, __ATOMIC_RELEASE);
	live_net_report_init();
	queue_network_report();
	live_flush_network_report(true);
}

static int stream_open_resources(void *(*allocate)(u32 bytes))
{
	for (unsigned i = 0; i < 2; i++) {
		live_jpeg_reset(i);
		rawBytes[i] = live_jpeg_frame_size(i ? 320 : 400);
		size_t sourceBytes =
		    live_jpeg_source_storage_size(i ? 320 : 400);
		void *arena = allocate((u32)(sourceBytes + rawBytes[i]));
		rawSources[i] =
		    live_jpeg_source_init(arena, sourceBytes, i ? 320 : 400);
		rawWorking[i] =
		    arena ? (LiveJpegFrame *)((u8 *)arena + sourceBytes) : NULL;
		if (!rawSources[i] || !rawWorking[i]) {
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
	socReady = true;
	/* DMA buffers are mapped later when the LCD modes become known.
	 * Leave room for both at maximum stride before taking any
	 * optional memory. */
	const u32 rowPages = (2 * sizeof(*rowBanks) + 4095u) & ~4095u;
	if (compactJpeg &&
	    live_pages_available(rowPages + LIVE_CAPTURE_MAX_DMA_BYTES))
		rowBanks = allocate(rowPages);
	for (int i = 1; i >= 0; i--) {
		encoderCores[i] = i + 2;
		encoders[i] = threadCreate(encoder, (void *)(uintptr_t)i, 8192,
					   0x3E, encoderCores[i], false);
		if (!encoders[i]) {
			encoderCores[i] = 1;
			encoders[i] =
			    threadCreate(encoder, (void *)(uintptr_t)i, 8192,
					 0x3E, 1, false);
		}
		if (!encoders[i]) {
			live_stop();
			return -6;
		}
	}
	worker = threadCreate(sender, NULL, 16384, 0x3E, 1, false);
	if (!worker) {
		live_stop();
		return -5;
	}
	return 0;
}

Result live_start(void *(*allocate)(u32 bytes))
{
	if (!allocate || worker || encoders[0] || encoders[1])
		return -1;
	LightLock_Init(&lock);
	for (unsigned i = 0; i < 2; i++)
		LightLock_Init(&rawLocks[i]);
	for (unsigned i = 0; i < 3; i++)
		LightEvent_Init(&events[i], RESET_ONESHOT);
	started = svcGetSystemTick();
	stopping = false;
	jobDone = 1;
	pending = (LivePendingQueue){0};
	planMask = 0;
	pacingMode = 1;
	stream_reset();
	return stream_open_resources(allocate);
}
static void stream_write_stats(bool timing_saved)
{
	char network_text[LIVE_NET_REPORT_BYTES];
	size_t network_bytes;
	struct live_stats stats = {
	    .combined_fec = combinedFec,
	    .sent_top = frames[0],
	    .sent_bottom = frames[1],
	    .replaced = replaced,
	    .send_errors = errors,
	    .rate = rate,
	    .fps = fps,
	    .duration_ms =
		(svcGetSystemTick() - started) * 1000 / SYSCLOCK_ARM11,
	    .expired_pairs = expired,
	    .last_send_errno = lastErrno,
	    .payload_target_percent = payloadControl.percent,
	    .quality_target = qualityTarget,
	    .pair_payload_budget =
		pair_payload_budget(rate, fps, combinedFec, qualityTarget),
	    .rate_pending_kept = ratePendingKept,
	    .rate_encoding_kept = rateEncodingKept,
	    .pairs_attempted = pairs,
	    .pairs_oversize = oversize,
	    .requantize_passes = retries,
	    .quality = quality,
	    .config_changes = configChanges,
	    .manual_render_failures = manualRenderFailures,
	    .manual_wire_budget_drops = manualWireDrops,
	    .repair_requests = repairRequests,
	    .repair_packets = repair_tx.packets,
	    .repair_wire_bytes = repair_tx.wire_bytes,
	    .repair_nack_cache_miss = repairHistory.missing,
	    .repair_nack_stale = repairHistory.stale,
	    .repair_nack_duplicate = repairHistory.duplicate,
	    .repair_nack_invalid = repairHistory.invalid,
	    .repair_fragments_expired = repairHistory.expired_fragments,
	    .repair_fragments_evicted = repairHistory.evicted_fragments,
	    .send_calls = sendCalls,
	    .send_sum_us = sendSumUs,
	    .send_max_us = sendMaxUs,
	    .feedback_count = feedbackCount,
	    .video_wire_bytes = videoWireBytes,
	    .snapshot_count = snapshotCount,
	    .snapshot_sum_us = snapshotSumUs,
	    .snapshot_max_us = snapshotMaxUs,
	    .compact_jpeg = compactJpeg,
	    .compact_frames = compactFrames,
	    .compact_saved_bytes = compactSavedBytes,
	    .compact_fallbacks = compactFallbacks,
	    .row_keys = rowKeys,
	    .row_key_repeats = rowRepeats,
	    .row_key_acks = rowAcks,
	    .row_delta_panels = rowDeltas,
	    .row_bytes_saved = rowBytesSaved,
	    .r20_attempts = r20.attempts,
	    .r20_skipped = r20.skipped,
	    .r20_panels = r20.panels,
	    .r20_bytes_saved = r20.saved,
	    .r20_encode_sum_us = r20.encode_sum_us,
	    .r20_encode_max_us = r20.max_us,
	    .audio_enabled = audioEnabled,
	    .audio_blocks_queued = audio.queue.queued,
	    .audio_blocks_dropped = audio.queue.dropped,
	    .audio_blocks_expired = audio.queue.expired,
	    .audio_packets_sent = audio.packets_sent,
	    .audio_send_errors = audio.send_errors,
	    .audio_wire_bytes = audio.wire_bytes,
	    .audio_encoded_blocks = audio.encoded_blocks,
	    .audio_encode_sum_us = audio.encode_sum_us,
	    .audio_encode_max_us = audio.encode_max_us,
	};
	for (unsigned i = 0; i < 2; i++) {
		stats.screens[i] = (struct live_stats_screen){
		    .attempts = attempts[i],
		    .jpeg = jpegFrames[i],
		    .encode_sum_us = sumUs[i],
		    .encode_max_us = maxUs[i],
		    .max_bytes = maxBytes[i],
		    .encoder_core = encoderCores[i],
		    .changed_mcus = changedMcus[i],
		    .source_reused = sourceReuse[i],
		    .dma_converted = convertCount[i],
		    .convert_sum_us = convertSumUs[i],
		    .convert_max_us = convertMaxUs[i],
		    .converted_mcus = convertedMcus[i],
		};
	}
#ifdef LIVE_FRAME_TIMING
	stats.timing_present = true;
	stats.timing_session = session;
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
		if (!encoders[i])
			continue;
		threadJoin(encoders[i], U64_MAX);
		threadFree(encoders[i]);
		encoders[i] = NULL;
	}
	if (worker) {
		threadJoin(worker, U64_MAX);
		threadFree(worker);
		worker = NULL;
	}
}

static void stream_close_resources(void)
{
	queue_network_report();
	rowBase = rowCandidate = rowProbe = rowBanks = NULL;
	live_net_close(&net);
	if (socReady) {
		socExit();
		socReady = false;
	}
	for (unsigned i = 0; i < 2; i++) {
		live_jpeg_reset(i);
		live_jpeg_source_destroy(rawSources[i]);
		rawSources[i] = NULL;
		rawWorking[i] = NULL; /* Caller releases the SYSTEM arena. */
	}
}

void live_stop(void)
{
	__atomic_store_n(&stopping, true, __ATOMIC_RELEASE);
	for (unsigned i = 0; i < 3; i++)
		LightEvent_Signal(&events[i]);
	stream_join_threads();
#ifdef LIVE_FRAME_TIMING
	bool timingSaved = live_timing_export(session, started);
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
	if (sc < 0 || sc > 1 || !worker || stopped() || capture_paused() ||
	    wide)
		return false;
	u64 begin = svcGetSystemTick();
	u32 w = sc ? 320 : 400;
	/* Exact native-source comparison precedes color conversion.
	 * Only changed MCUs enter the compact YCbCr420 snapshot; no
	 * RGB565 staging frame. */
	LightLock_Lock(&rawLocks[sc]);
	int converted = live_jpeg_source_update(
	    rawSources[sc], frame, (size_t)w * stride, fmt, stride);
	if (converted >= 0) {
		rawPts[sc] = pts;
		rawValid[sc] = true;
	}
	LightLock_Unlock(&rawLocks[sc]);
	if (converted < 0)
		return false;
	convertedMcus[sc] += (unsigned)converted;
	LightEvent_Signal(&events[0]);
	u32 us = (u32)((svcGetSystemTick() - begin) * 1000000 / SYSCLOCK_ARM11);
	convertCount[sc]++;
	convertSumUs[sc] += us;
	if (us > convertMaxUs[sc])
		convertMaxUs[sc] = us;
	return true;
}
void live_schedule(u32 stamp, u32 top, u32 bottom, u32 mask)
{
	if (!worker || stopped())
		return;
	LightLock_Lock(&lock);
	planStamp = stamp;
	planPts[0] = top;
	planPts[1] = bottom;
	planMask = mask;
	LightLock_Unlock(&lock);
}
