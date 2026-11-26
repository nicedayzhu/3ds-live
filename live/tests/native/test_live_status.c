/*
 * Host test for the R7ST/R9ST status layout.
 */
#include "live_status.h"

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
	struct live_status_main status = {
	    .session = 0x1234,
	    .duration_ms = 5678,
	    .rate = 2000000,
	    .video_rate = 1900000,
	    .fps = 30,
	    .encoded_top = 11,
	    .encoded_bottom = 12,
	    .sent_top = 13,
	    .sent_bottom = 14,
	    .send_errors = 15,
	    .expired_pairs = 16,
	    .feedback_count = 17,
	    .encoder_core_top = 2,
	    .encoder_core_bottom = 3,
	    .last_encode_us_top = 400,
	    .last_encode_us_bottom = 500,
	    .audio_enabled = false,
	    .audio_blocks_queued = 21,
	    .audio_blocks_dropped = 22,
	    .audio_packets_sent = 23,
	    .audio_queue_depth = 24,
	};
	uint8_t packet[108];

	assert(live_status_main_build(packet, sizeof(packet), &status) == 76);
	assert(memcmp(packet, "R7ST", 4) == 0);
	assert(get32(packet + 4) == 0x1234);
	assert(get32(packet + 8) == 5678);
	assert(get32(packet + 12) == 2000000);
	assert(get32(packet + 24) == 11);
	assert(get32(packet + 28) == 12);
	assert(get32(packet + 52) == 2);
	assert(get32(packet + 56) == 3);

	status.audio_enabled = true;
	assert(live_status_main_build(packet, sizeof(packet), &status) == 108);
	assert(memcmp(packet, "R9ST", 4) == 0);
	assert(get32(packet + 76) == 1);
	assert(get32(packet + 80) == 32728498u);
	assert(get32(packet + 92) == 21);
	assert(get32(packet + 96) == 22);
	assert(get32(packet + 100) == 23);
	assert(get32(packet + 104) == 24);

	assert(live_status_main_build(packet, 8, &status) == 0);
	assert(live_status_main_build(NULL, 108, &status) == 0);
	puts("test_live_status: ok");
	return 0;
}
