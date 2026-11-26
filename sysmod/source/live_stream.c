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
