/*
 * R7ST/R9ST and the accompanying status burst.
 */
#include "live_status.h"

#include "live_audio.h"
#include "live_fps.h"
#include "live_pair.h"
#include "live_repair.h"

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

	if (!buffer || capacity < length)
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

static unsigned append(size_t capacity, struct live_status_packet *packets,
		       unsigned count, unsigned max, size_t *offset,
		       uint32_t size)
{
	if (count >= max || *offset + size > capacity || size > UINT16_MAX)
		return count;

	packets[count].offset = (uint16_t)*offset;
	packets[count].size = (uint16_t)size;
	*offset += size;
	return count + 1;
}

unsigned live_status_build_burst(uint8_t *buffer, size_t capacity,
				 const struct live_status_main *main_status,
				 const struct live_status_aux *aux,
				 struct live_status_packet *packets,
				 unsigned max_packets)
{
	size_t offset = 0, main_size;
	uint8_t *packet;
	unsigned count = 0;

	if (!buffer || !main_status || !aux || !packets || !max_packets)
		return 0;

	main_size = live_status_main_build(buffer, capacity, main_status);
	if (!main_size)
		return 0;
	count = append(capacity, packets, count, max_packets, &offset,
		       (uint32_t)main_size);
	if (!count)
		return 0;

	packet = buffer + offset;
	memcpy(packet, "R16S", 4);
	put32(packet + 4, aux->session);
	put32(packet + 8, 1);
	put32(packet + 12, aux->quality_target);
	put32(packet + 16, aux->duration_ms);
	count = append(capacity, packets, count, max_packets, &offset, 20);

	packet = buffer + offset;
	memcpy(packet, "R16Q", 4);
	put32(packet + 4, aux->session);
	put32(packet + 8, 1);
	put32(packet + 12, LIVE_JPEG_EXTRA_QUALITY);
	count = append(capacity, packets, count, max_packets, &offset, 16);

	packet = buffer + offset;
	memcpy(packet, "R17T", 4);
	put32(packet + 4, aux->session);
	put32(packet + 8, aux->duration_ms);
	put32(packet + 12,
	      (uint32_t)(aux->audio_wire_bytes + aux->video_wire_bytes));
	put32(packet + 16, (uint32_t)aux->video_wire_bytes);
	count = append(capacity, packets, count, max_packets, &offset, 20);

	if (aux->rows_enabled && aux->compact_jpeg) {
		packet = buffer + offset;
		memcpy(packet, "R17S", 4);
		put32(packet + 4, aux->session);
		put32(packet + 8, 1);
		count =
		    append(capacity, packets, count, max_packets, &offset, 12);
		if (aux->combined_fec) {
			packet = buffer + offset;
			memcpy(packet, "R20S", 4);
			count = append(capacity, packets, count, max_packets,
				       &offset, 12);
		}
	}

	if (aux->combined_fec) {
		packet = buffer + offset;
		memcpy(packet, "R19S", 4);
		put32(packet + 4, aux->session);
		put32(packet + 8, 1);
		put32(packet + 12, LIVE_REPAIR_MAX);
		count =
		    append(capacity, packets, count, max_packets, &offset, 16);
	}

	{
		const uint32_t timing[] = {
		    aux->session,	    aux->duration_ms,
		    aux->overwritten,	    aux->row_repeats,
		    aux->repair_requests,   aux->repair_packets,
		    aux->repair_wire_bytes, aux->audio_dropped,
		    aux->audio_expired,	    aux->send_active_us,
		};

		packet = buffer + offset;
		memcpy(packet, "R19T", 4);
		for (unsigned i = 0; i < sizeof(timing) / sizeof(timing[0]);
		     i++)
			put32(packet + 4 + 4 * i, timing[i]);
		count = append(capacity, packets, count, max_packets, &offset,
			       4 + sizeof(timing));
	}

	packet = buffer + offset;
	memcpy(packet, "R21S", 4);
	put32(packet + 4, aux->session);
	put32(packet + 8, 1);
	put32(packet + 12, aux->pacing_mode);
	count = append(capacity, packets, count, max_packets, &offset, 16);

	packet = buffer + offset;
	memcpy(packet, aux->pacing_words, sizeof(aux->pacing_words));
	count = append(capacity, packets, count, max_packets, &offset,
		       sizeof(aux->pacing_words));

	return count;
}
