#ifndef LIVE_PACING_H
#define LIVE_PACING_H
#include <stdint.h>
/* A capture period is not a transmit deadline. Game scheduling and SOC IPC
 * can stretch a pair past 33 ms even when every datagram succeeds. Keep only
 * the latest pending pair, but allow an in-flight pair 150 ms to finish.
 * The PC reassembly timeout must exceed this (currently 200 ms). */
#define LIVE_SEND_TIMEOUT_MS 150u
/* The receiver expires a partial pair after 1 s. Keep room for arrival jitter.
 */
#define LIVE_MANUAL_SEND_MAX_MS 950u

/* Encoder-owned. A full one-pair mailbox at publication means a complete
 * encoded pair was replaced before the socket worker could pick it up. */
typedef struct {
	unsigned percent, samples, replacements, clean_windows;
} LivePayloadControl;

static inline void live_payload_observe(LivePayloadControl *control,
					unsigned fps, int replaced)
{
	control->replacements += replaced != 0;
	if (++control->samples < fps)
		return;
	if (control->replacements >= 2) {
		control->percent =
		    control->percent > 70 ? control->percent - 10 : 60;
		control->clean_windows = 0;
	} else if (control->replacements) {
		control->clean_windows = 0;
	} else if (++control->clean_windows == 4) {
		control->percent =
		    control->percent < 98 ? control->percent + 2 : 100;
		control->clean_windows = 0;
	}
	control->samples = control->replacements = 0;
}
static inline uint64_t live_pair_deadline(uint64_t start,
					  uint64_t ticks_per_second)
{
	return start + ticks_per_second * LIVE_SEND_TIMEOUT_MS / 1000;
}

/* An explicit Q can take several capture slots to transmit. Give this
 * single in-flight pair its wire time plus 50 ms of scheduler slack. The
 * encoder continues to fill the one-pair mailbox on the requested clock. */
static inline uint64_t live_video_deadline(uint64_t start,
					   uint64_t ticks_per_second,
					   uint32_t wire_bytes,
					   uint32_t video_rate, int manual_q)
{
	uint32_t available = video_rate * 96u / 100u;
	if (!manual_q || !available)
		return live_pair_deadline(start, ticks_per_second);
	uint64_t ms =
	    ((uint64_t)wire_bytes * 1000u + available - 1) / available + 50u;
	if (ms < LIVE_SEND_TIMEOUT_MS)
		ms = LIVE_SEND_TIMEOUT_MS;
	if (ms > LIVE_MANUAL_SEND_MAX_MS)
		ms = LIVE_MANUAL_SEND_MAX_MS;
	return start + ticks_per_second * ms / 1000u;
}

/* Anchor early wakeups to the original slot. After a full missed period,
 * resume from the current time so a backlog cannot cause a capture burst. */
static inline uint64_t live_next_encode_tick(uint64_t slot, uint64_t now,
					     uint64_t ticks_per_second,
					     uint32_t fps)
{
	uint64_t period = ticks_per_second / fps, next = slot + period;
	return next > now ? next : now + period;
}

/* Each published capture timestamp can feed one encoder job. Zero is a
 * valid PTS, including at wrap. The encoder commits only after both source
 * buffers for that plan were successfully copied. */
typedef struct {
	uint32_t stamp;
	int valid;
} LiveConsumedPlan;
static inline int live_plan_available(const LiveConsumedPlan *last,
				      uint32_t stamp)
{
	return !last->valid || last->stamp != stamp;
}
static inline void live_plan_consume(LiveConsumedPlan *last, uint32_t stamp)
{
	last->stamp = stamp;
	last->valid = 1;
}

/* Fill about 92% of a capture period with this pair's actual IPv4 video
 * bytes (JPEG, all datagram headers and FEC). Small updates can be delivered
 * immediately at the available video ceiling. Audio has its own reserved
 * share, and both media types still use the total datagram pacing clock. */
static inline uint32_t live_video_pacing_rate(uint32_t wire_bytes,
					      uint32_t data_packets,
					      uint32_t fps,
					      uint32_t video_ceiling)
{
	if (data_packets <= 4 || !wire_bytes || !fps)
		return video_ceiling;
	uint64_t rate = ((uint64_t)wire_bytes * fps * 100u + 91u) / 92u;
	return rate < video_ceiling ? (uint32_t)rate : video_ceiling;
}
/* Charge from submission start. Time spent inside sendto is already part of
 * this interval, not an additional sleep. Late wakes never accrue burst credit.
 * Caller validates rate>0; wire_bytes includes the IPv4 and UDP headers. */
static inline uint64_t live_packet_due(uint64_t previous_due, uint64_t start,
				       uint32_t wire_bytes, uint32_t rate,
				       uint64_t ticks_per_second)
{
	uint64_t base = start > previous_due ? start : previous_due;
	return base +
	       ((uint64_t)wire_bytes * ticks_per_second + rate - 1) / rate;
}

/* SOC can spend a complete byte interval inside sendto. Its actual enqueue
 * instant is private to the network service. Leave a short recovery gap
 * after such a return without charging normal calls twice. Never admit a
 * packet before the original byte clock. `byte_ticks` is one datagram. */
static inline uint64_t live_packet_return_due(uint64_t byte_due,
					      uint64_t returned,
					      uint64_t byte_ticks, uint64_t hz,
					      int guarded)
{
	if (!guarded)
		return byte_due;
	uint64_t gap = (byte_ticks + 3) / 4;
	uint64_t limit = (hz * 150u + 999999u) / 1000000u;
	if (gap > limit)
		gap = limit;
	uint64_t protected_due = returned + gap;
	return protected_due > byte_due ? protected_due : byte_due;
}

/* The soft schedule is tied to publication of this pair. A late SOC call,
 * sleep or an audio batch need not move every later video packet equally
 * late. The separate real-time spacing clock permits at most 20% above
 * the smooth rate and never exceeds the available video ceiling. Its last
 * charge also carries across pairs. All media still obey the total clock. */
typedef struct {
	uint64_t slot_due, earliest;
	uint32_t rate, peak;
} LiveVideoPacing;

static inline void live_video_slot(LiveVideoPacing *p, uint64_t published,
				   uint32_t wire_bytes, uint32_t data_packets,
				   uint32_t fps, uint32_t ceiling)
{
	p->slot_due = published;
	p->rate =
	    live_video_pacing_rate(wire_bytes, data_packets, fps, ceiling);
	uint32_t peak = p->rate + p->rate / 5u;
	p->peak = peak < ceiling ? peak : ceiling;
}

static inline uint64_t live_video_due(const LiveVideoPacing *p)
{
	return p->slot_due > p->earliest ? p->slot_due : p->earliest;
}

static inline void live_video_charge(LiveVideoPacing *p, uint64_t start,
				     uint32_t wire_bytes,
				     uint64_t ticks_per_second)
{
	uint64_t scaled = (uint64_t)wire_bytes * ticks_per_second;
	uint64_t nominal = (scaled + p->rate - 1) / p->rate;
	p->slot_due += nominal;
	p->earliest =
	    start +
	    (p->peak == p->rate ? nominal : (scaled + p->peak - 1) / p->peak);
}
#endif
