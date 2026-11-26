/*
 * Audio batching and transmission for the 3DS system module.
 *
 * The capture thread only pushes PCM blocks; the network thread drains a
 * bounded batch, encodes IMA ADPCM, adds one redundant previous batch and
 * sends one R9A1 datagram.  Pacing and the socket stay owned by the caller.
 */
#ifndef LIVE_AUDIO_SENDER_H
#define LIVE_AUDIO_SENDER_H

#include "live_audio.h"
#include "live_audio_transport.h"

#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>
#include <sys/socket.h>

struct live_audio_sender {
	LiveAudioQueue queue;
	LiveAudioState codec;
	LiveAudioHistory history;
	uint64_t encode_sum_us;
	uint64_t wire_bytes;
	uint32_t packet_sequence;
	uint32_t packets_sent;
	uint32_t send_errors;
	uint32_t encoded_blocks;
	uint32_t encode_max_us;
};

typedef void (*live_pacing_sleep_fn)(uint64_t due);
typedef void (*live_pacing_charge_fn)(uint64_t *due, uint64_t begin,
				      uint64_t returned, uint32_t wire,
				      int kind);

void live_audio_sender_init(struct live_audio_sender *sender);

/* Capture producer.  Returns false when the queue is full. */
bool live_audio_sender_push(struct live_audio_sender *sender,
			    const int16_t *pcm, uint32_t pts, uint64_t tick);

/*
 * Network consumer.  Call only while the peer is live.  `stopping` is checked
 * again after pacing so a stop request cannot be followed by a datagram.
 */
void live_audio_sender_send(struct live_audio_sender *sender, uint32_t session,
			    bool enabled, bool stopping, int fd,
			    const struct sockaddr_in *target, uint64_t *due,
			    live_pacing_sleep_fn sleep_until,
			    live_pacing_charge_fn charge);

#endif /* LIVE_AUDIO_SENDER_H */
