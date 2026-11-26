/*
 * Batching and wire formatting for R9A1 audio.
 */
#include "live_audio_sender.h"

#include "packet_report.h"

#include <3ds.h>
#include <string.h>
#include <unistd.h>

#define AUDIO_AGE_TICKS                                                        \
	((uint64_t)SYSCLOCK_ARM11 * LIVE_AUDIO_MAX_AGE_US / 1000000ull)
#define AUDIO_BATCH_WAIT_TICKS                                                 \
	((uint64_t)SYSCLOCK_ARM11 * LIVE_AUDIO_BATCH_WAIT_US / 1000000ull)

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void put16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

void live_audio_sender_init(struct live_audio_sender *sender)
{
	memset(sender, 0, sizeof(*sender));
	live_audio_queue_init(&sender->queue);
	live_audio_init(&sender->codec);
	live_audio_history_reset(&sender->history);
}

bool live_audio_sender_push(struct live_audio_sender *sender,
			    const int16_t *pcm, uint32_t pts, uint64_t tick)
{
	return live_audio_queue_push(&sender->queue, pcm, pts, tick);
}

static bool audio_batch_ready(struct live_audio_sender *sender, uint64_t now)
{
	uint64_t oldest;

	if (!live_audio_queue_peek_tick(&sender->queue, &oldest))
		return false;
	return live_audio_queue_depth(&sender->queue) >=
		   LIVE_AUDIO_BATCH_BLOCKS ||
	       (now >= oldest && now - oldest >= AUDIO_BATCH_WAIT_TICKS);
}

static unsigned encode_batch(struct live_audio_sender *sender, uint8_t *records,
			     uint64_t *first_capture)
{
	LiveAudioCapture item;
	unsigned count = 0;

	while (count < LIVE_AUDIO_BATCH_BLOCKS &&
	       live_audio_queue_pop(&sender->queue, &item, svcGetSystemTick(),
				    AUDIO_AGE_TICKS)) {
		uint8_t *record = records + count * LIVE_AUDIO_RECORD_BYTES;
		uint64_t begin;
		uint32_t elapsed;
		size_t bytes;

		put32(record, item.sequence);
		put32(record + 4, item.pts_us);
		begin = svcGetSystemTick();
		bytes = live_audio_encode(&sender->codec, item.pcm,
					  LIVE_AUDIO_SAMPLES, record + 8,
					  LIVE_AUDIO_BLOCK_BYTES);
		elapsed = (uint32_t)((svcGetSystemTick() - begin) * 1000000 /
				     SYSCLOCK_ARM11);
		sender->encode_sum_us += elapsed;
		if (elapsed > sender->encode_max_us)
			sender->encode_max_us = elapsed;
		if (bytes != LIVE_AUDIO_BLOCK_BYTES)
			continue;
		if (!count)
			*first_capture = item.queued_tick;
		sender->encoded_blocks++;
		count++;
	}

	return count;
}

void live_audio_sender_send(struct live_audio_sender *sender, uint32_t session,
			    bool enabled, bool stopping, int fd,
			    const struct sockaddr_in *target, uint64_t *due,
			    live_pacing_sleep_fn sleep_until,
			    live_pacing_charge_fn charge)
{
	uint8_t packet[LIVE_AUDIO_MAX_PACKET_BYTES];
	uint8_t fresh[LIVE_AUDIO_BATCH_BLOCKS * LIVE_AUDIO_RECORD_BYTES];
	const LiveAudioBatch *repeat;
	uint64_t first_capture = 0, begin, now;
	unsigned repeat_count, count, bytes;
	int sent;

	if (!enabled || stopping)
		return;
	now = svcGetSystemTick();
	if (!audio_batch_ready(sender, now))
		return;

	count = encode_batch(sender, fresh, &first_capture);
	if (!count)
		return;

	repeat = live_audio_history_repeat(&sender->history, svcGetSystemTick(),
					   AUDIO_AGE_TICKS);
	repeat_count = repeat ? repeat->count : 0;
	memcpy(packet, "R9A1", 4);
	put32(packet + 4, session);
	put32(packet + 8, ++sender->packet_sequence);
	put16(packet + 12, (uint16_t)(count + repeat_count));
	put16(packet + 14, 0);
	put32(packet + 16, LIVE_AUDIO_RATE_MHZ);
	put16(packet + 20, LIVE_AUDIO_SAMPLES);
	put16(packet + 22, LIVE_AUDIO_CHANNELS);
	if (repeat)
		memcpy(packet + LIVE_AUDIO_HEADER_BYTES, repeat->records,
		       repeat_count * LIVE_AUDIO_RECORD_BYTES);
	memcpy(packet + LIVE_AUDIO_HEADER_BYTES +
		   repeat_count * LIVE_AUDIO_RECORD_BYTES,
	       fresh, count * LIVE_AUDIO_RECORD_BYTES);
	bytes = LIVE_AUDIO_HEADER_BYTES +
		(count + repeat_count) * LIVE_AUDIO_RECORD_BYTES;

	sleep_until(*due);
	if (stopping)
		return;
	begin = svcGetSystemTick();
	sent = sendto(fd, packet, bytes, 0, (const struct sockaddr *)target,
		      sizeof(*target));
	charge(due, begin, svcGetSystemTick(), bytes + 28, PACKET_AUDIO);
	if (sent == (int)bytes) {
		sender->packets_sent++;
		sender->wire_bytes += bytes + 28;
	} else {
		sender->send_errors++;
	}
	/* A failed first send still gets one redundancy slot. */
	live_audio_history_store(&sender->history, fresh, count, first_capture);
}
