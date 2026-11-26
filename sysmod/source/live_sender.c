/*
 * Packet pacing, repair transmission and the sender thread.
 */
#include "live_stream_internal.h"

#include "live_audio.h"
#include "live_pacing.h"
#include "live_pair.h"
#include "live_repair.h"
#include "live_timing.h"

#include <errno.h>
#include <string.h>

static void put32(u8 *p, u32 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
	p[2] = (u8)(value >> 16);
	p[3] = (u8)(value >> 24);
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
	    .stopping = live_stream_stopped,
	    .capture_paused = live_stream_capture_paused,
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
		if (live_stream_stopped() || now > deadline) {
			live_timing_finish(live_stream_stopped()
					       ? TIMING_STOPPED
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
			live_stream_discard_pending(stream.pending.head,
						    TIMING_RECONFIGURED);
		} else if (live_pending_expired(head, now, SYSCLOCK_ARM11,
						stream.settings.fps)) {
			live_stream_discard_pending(stream.pending.head,
						    TIMING_TOO_OLD);
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
		 live_stream_pair_budget(stream.settings.rate,
					 stream.settings.fps,
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

void sender(void *unused)
{
	(void)unused;
	PacketPacing pacing = {.totalDue = svcGetSystemTick()};
	bool sentFrame = false;
	u32 lastSentSeq = 0;
	while (!live_stream_stopped()) {
		control(&pacing.totalDue);
		if (!stream.net.media_ready || live_stream_capture_paused()) {
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
