/*
 * R19P header layout.
 */
#include "live_repair_packet.h"

#include <string.h>

#define CHUNK 1168u

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

size_t live_repair_payload_size(const LiveRepairFrame *frame, uint32_t index)
{
	uint32_t offset, total;

	if (!frame)
		return 0;
	offset = index * CHUNK;
	total = frame->top + frame->bottom;
	if (offset >= total)
		return 0;
	return total - offset > CHUNK ? CHUNK : total - offset;
}

size_t live_repair_packet_build(uint8_t *packet, size_t capacity,
				const LiveRepairFrame *frame, uint32_t index,
				uint32_t session)
{
	if (!packet || !frame || capacity < LIVE_REPAIR_HEADER_BYTES ||
	    !live_repair_payload_size(frame, index))
		return 0;

	memcpy(packet, "R19P", 4);
	put32(packet + 4, session);
	put32(packet + 8, frame->seq);
	put32(packet + 12, frame->pts);
	put32(packet + 16, frame->top);
	put32(packet + 20, frame->bottom);
	put32(packet + 24, index);
	put32(packet + 28, frame->fps);

	return LIVE_REPAIR_HEADER_BYTES;
}
