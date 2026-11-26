/*
 * Wire parsing for the media/control datagrams a receiver sends to the console.
 *
 * This module owns byte layout and validation only.  Applying a command
 * (taking the pair lock, reconfiguring the encoder, acknowledging rows) stays
 * in live_stream.c through the ops table, so protocol tests do not need any
 * libctru state.
 */
#ifndef LIVE_CONTROL_H
#define LIVE_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

struct live_control_config {
	uint32_t sequence;
	uint32_t fps;
	uint32_t rate;
	uint32_t mask;
	unsigned quality;
	bool combined_fec;
};

struct live_control_ops {
	void (*configure)(const struct live_control_config *config,
			  uint64_t now);
	void (*set_mask)(uint32_t mask, uint64_t now);
	void (*set_pacing)(unsigned mode, uint64_t now);
	void (*ack_r20)(uint64_t now);
	void (*repair_peer)(uint64_t now);
	void (*repair_request)(const uint8_t *msg, int size, uint64_t now);
	void (*ack_rows)(uint32_t flags, uint32_t sequence, uint64_t now);
	void (*feedback)(uint32_t sent, uint32_t transit, uint64_t now);
};

/*
 * Handle one datagram addressed to this session.  Returns true when the tag
 * was recognised, including packets rejected by validation.  current_* are
 * the live values used by legacy messages that omit a field.
 */
bool live_control_handle(const uint8_t *msg, int size, uint64_t now,
			 uint32_t current_mask, bool current_combined,
			 unsigned current_quality,
			 const struct live_control_ops *ops);

#endif /* LIVE_CONTROL_H */
