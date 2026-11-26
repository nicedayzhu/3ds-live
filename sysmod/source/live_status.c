/*
 * R7ST/R9ST wire layout.
 */
#include "live_status.h"

#include "live_audio.h"
#include "live_fps.h"
#include "live_pair.h"

#include <string.h>

#define HEADER_BYTES 4u
#define WORD_BYTES 4u
#define AUDIO_WORDS 26u
#define VIDEO_WORDS 18u

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

size_t live_status_main_build(uint8_t *buffer, size_t capacity,
			      const struct live_status_main *status)
{
	uint32_t values[AUDIO_WORDS];
	size_t words = status->audio_enabled ? AUDIO_WORDS : VIDEO_WORDS;
	size_t length = HEADER_BYTES + words * WORD_BYTES;
	size_t i;

	if (!buffer || !status || capacity < length)
		return 0;

	values[0] = status->session;
	values[1] = status->duration_ms;
	values[2] = status->rate;
	values[3] = status->video_rate;
	values[4] = status->fps;
	values[5] = status->encoded_top;
	values[6] = status->encoded_bottom;
	values[7] = status->sent_top;
	values[8] = status->sent_bottom;
	values[9] = status->send_errors;
	values[10] = status->expired_pairs;
	values[11] = status->feedback_count;
	values[12] = (uint32_t)status->encoder_core_top;
	values[13] = (uint32_t)status->encoder_core_bottom;
	values[14] = status->last_encode_us_top;
	values[15] = status->last_encode_us_bottom;
	values[16] = LIVE_MAX_RATE;
	values[17] = LIVE_FPS_MASK;

	if (status->audio_enabled) {
		values[18] = 1;
		values[19] = LIVE_AUDIO_RATE_MHZ;
		values[20] = LIVE_AUDIO_CHANNELS;
		values[21] = LIVE_AUDIO_SAMPLES;
		values[22] = status->audio_blocks_queued;
		values[23] = status->audio_blocks_dropped;
		values[24] = status->audio_packets_sent;
		values[25] = status->audio_queue_depth;
	}

	memcpy(buffer, status->audio_enabled ? "R9ST" : "R7ST", HEADER_BYTES);
	for (i = 0; i < words; i++)
		put32(buffer + HEADER_BYTES + i * WORD_BYTES, values[i]);

	return length;
}
