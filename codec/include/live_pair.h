#ifndef LIVE_PAIR_H
#define LIVE_PAIR_H
#include <stdint.h>
#include <string.h>
#include "live_pacing.h"
#include "live_fps.h"
#define LIVE_CHUNK 1168u
#define LIVE_PARITY 2u
#define LIVE_PAIR_PARITY 4u
#define LIVE_MIN_RATE 400000u
#define LIVE_MAX_RATE 2000000u
static inline uint32_t live_data_packets(uint32_t bytes)
{
	return bytes / LIVE_CHUNK + (bytes % LIVE_CHUNK != 0);
}
static inline uint32_t live_parity_packets(uint32_t data_packets,
					   uint32_t stripes)
{
	return data_packets < stripes ? data_packets : stripes;
}
/* A stripe with no data has no recovery value. Every populated stripe
 * keeps its existing full-size parity packet, even for a tiny final JPEG. */
static inline uint32_t live_screen_wire(uint32_t bytes)
{
	uint32_t chunks = live_data_packets(bytes);
	return bytes + 60 * chunks +
	       live_parity_packets(chunks, LIVE_PARITY) * (LIVE_CHUNK + 60);
}
static inline uint32_t live_split_payload_limit(uint32_t available)
{
	uint32_t fixed = 2 * LIVE_PARITY * (LIVE_CHUNK + 60) + 120;
	if (available <= fixed)
		return 0;
	return (uint32_t)((uint64_t)(available - fixed) * LIVE_CHUNK /
			  (LIVE_CHUNK + 60));
}
static inline uint32_t live_pair_budget(uint32_t rate, uint32_t fps)
{
	return fps ? live_split_payload_limit(rate / fps * 96 / 100) : 0;
}
/* R10 joins the two payloads before fragmentation and has up to four
 * nonempty parity stripes. Include IPv4, UDP and media headers. */
static inline uint32_t live_combined_wire(uint32_t bytes)
{
	uint32_t chunks = live_data_packets(bytes);
	return bytes + 60 * chunks +
	       live_parity_packets(chunks, LIVE_PAIR_PARITY) *
		   (LIVE_CHUNK + 60);
}
static inline uint32_t live_combined_payload_limit(uint32_t available)
{
	const uint32_t packet = LIVE_CHUNK + 60;
	uint32_t parity = LIVE_PAIR_PARITY * packet;
	if (available < 2 * parity) {
		/* The first four chunks each add a new full-size parity stripe.
		 */
		uint32_t full = available / (2 * packet),
			 tail = available % (2 * packet);
		return full * LIVE_CHUNK +
		       (tail > packet + 60 ? tail - packet - 60 : 0);
	}
	available -= parity;
	uint32_t tail = available % (LIVE_CHUNK + 60);
	return available / (LIVE_CHUNK + 60) * LIVE_CHUNK +
	       (tail > 60 ? tail - 60 : 0);
}
static inline uint32_t live_combined_budget(uint32_t rate, uint32_t fps)
{
	return fps ? live_combined_payload_limit(rate / fps * 96 / 100) : 0;
}
#define LIVE_JPEG_EXTRA_QUALITY 70u /* Announced to receivers by R16Q v1. */
static inline int live_valid_quality_target(unsigned q)
{
	return q == 0 || q == 15 || q == 20 || q == 25 || q == 30 || q == 35 ||
	       q == 40 || q == 50 || q == 60 || q == LIVE_JPEG_EXTRA_QUALITY ||
	       q == 80;
}
/* The caller has validated the R6CF/R10C/R16C tag and exact length. Only
 * R16C carries a JPEG selection; its explicit zero requests automatic Q. */
static inline uint32_t live_config_jpeg_target(const uint8_t *msg,
					       size_t length, uint32_t current)
{
	/* R16C v1 is 28 bytes; v2 appends the screen mask and is 32 bytes. */
	if (length != 28 && length != 32)
		return current;
	return (uint32_t)msg[24] | (uint32_t)msg[25] << 8 |
	       (uint32_t)msg[26] << 16 | (uint32_t)msg[27] << 24;
}
static inline uint32_t live_quality_budget(uint32_t video_rate, uint32_t fps,
					   int combined, unsigned target)
{
	if (!target)
		return combined ? live_combined_budget(video_rate, fps)
				: live_pair_budget(video_rate, fps);
	uint32_t available = (uint64_t)(video_rate * 96u / 100u) *
			     LIVE_MANUAL_SEND_MAX_MS / 1000u;
	return combined ? live_combined_payload_limit(available)
			: live_split_payload_limit(available);
}
/* Copy one bounded chunk of the logical concatenation, without a frame-sized
 * extra allocation. Shared by the ARM sender and host protocol conformance QA.
 */
static inline uint32_t live_pair_fragment(uint8_t *out, const uint8_t *top,
					  uint32_t nt, const uint8_t *bottom,
					  uint32_t nb, uint32_t index)
{
	uint64_t offset = (uint64_t)index * LIVE_CHUNK,
		 total = (uint64_t)nt + nb;
	if (!out || !top || !bottom || offset >= total)
		return 0;
	uint32_t length = total - offset > LIVE_CHUNK
			      ? LIVE_CHUNK
			      : (uint32_t)(total - offset);
	uint32_t first = offset < nt ? nt - (uint32_t)offset : 0;
	if (first > length)
		first = length;
	if (first)
		memcpy(out, top + (uint32_t)offset, first);
	if (length > first)
		memcpy(out + first, bottom + (uint32_t)(offset + first - nt),
		       length - first);
	return length;
}
static inline unsigned live_lower_quality(unsigned quality, uint32_t size,
					  uint32_t budget)
{
	unsigned scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
	uint32_t next =
	    (uint32_t)((uint64_t)scale * size * 110 / (budget * 100));
	unsigned q = next > 100 ? 5000 / (next ? next : 1) : 100 - next / 2;
	if (q >= quality)
		q = quality > 4 ? quality - 4 : 1;
	return q < 1 ? 1 : q;
}
/* Only automatic quality retries an oversized render. A fixed target is
 * either delivered at that Q or counted as a frame that could not fit. */
static inline unsigned live_retry_quality(unsigned target, unsigned q,
					  unsigned attempt, uint32_t size,
					  uint32_t budget)
{
	if (target || q <= 1 || attempt >= 2 || !budget)
		return q;
	return attempt == 1 ? 1 : live_lower_quality(q, size, budget);
}
#endif
