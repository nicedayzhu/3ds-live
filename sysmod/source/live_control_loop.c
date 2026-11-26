/*
 * Control receive loop, discovery/media dispatch and the status burst.
 */
#include "live_stream_internal.h"
#include "live_stream.h"

#include "live_control.h"
#include "live_status.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static u32 get32(const u8 *p)
{
	return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
#ifdef LIVE_FRAME_TIMING
static void put32(u8 *p, u32 n)
{
	for (unsigned i = 0; i < 4; i++)
		p[i] = (u8)(n >> (8 * i));
}
#endif
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

void poll_frame_feedback(u64 now)
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
		live_stream_discard_pending(stream.pending.head,
					    TIMING_RECONFIGURED);
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
	live_stream_configure(config->fps, config->rate, config->combined_fec,
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

void control(u64 *due)
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
