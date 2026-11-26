/*
 * Host test for the R19P header.
 */
#include "live_repair_packet.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

int main(void)
{
	LiveRepairFrame frame = {
	    .seq = 0x11223344,
	    .pts = 0x55667788,
	    .top = 2500,
	    .bottom = 1500,
	    .fps = 30,
	};
	uint8_t packet[32];

	assert(live_repair_payload_size(&frame, 0) == 1168);
	assert(live_repair_payload_size(&frame, 1) == 1168);
	assert(live_repair_payload_size(&frame, 2) == 1168);
	assert(live_repair_payload_size(&frame, 3) == 496);
	assert(live_repair_payload_size(&frame, 4) == 0);

	assert(live_repair_packet_build(packet, sizeof(packet), &frame, 3,
					0x99) == 32);
	assert(memcmp(packet, "R19P", 4) == 0);
	assert(get32(packet + 4) == 0x99);
	assert(get32(packet + 8) == 0x11223344);
	assert(get32(packet + 12) == 0x55667788);
	assert(get32(packet + 16) == 2500);
	assert(get32(packet + 20) == 1500);
	assert(get32(packet + 24) == 3);
	assert(get32(packet + 28) == 30);

	assert(live_repair_packet_build(packet, 8, &frame, 0, 1) == 0);
	assert(live_repair_packet_build(packet, sizeof(packet), &frame, 9, 1) ==
	       0);
	puts("test_live_repair_packet: ok");
	return 0;
}
