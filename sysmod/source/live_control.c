/*
 * Byte-level control protocol parser.
 */
#include "live_control.h"

#include "live_fps.h"
#include "live_pair.h"

#include <stddef.h>
#include <string.h>

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static bool is_tag(const uint8_t *msg, int size, const char *tag)
{
	return size >= 4 && !memcmp(msg, tag, 4);
}

static bool handle_config(const uint8_t *msg, int size, uint64_t now,
			  uint32_t current_mask, bool current_combined,
			  unsigned current_quality,
			  const struct live_control_ops *ops)
{
	bool quality = size >= 28 && size <= 32 && is_tag(msg, size, "R16C");
	bool legacy = size == 20 && is_tag(msg, size, "R6CF");
	bool budget = size == 24 && is_tag(msg, size, "R10C");
	struct live_control_config config;
	uint32_t sequence, fps, rate;

	if (!quality && !legacy && !budget)
		return false;

	sequence = get32(msg + 8);
	fps = get32(msg + 12);
	rate = get32(msg + 16);
	config.sequence = sequence;
	config.fps = fps;
	config.rate = rate;
	config.combined_fec =
	    size >= 24 ? get32(msg + 20) != 0 : current_combined;
	config.quality = live_config_jpeg_target(msg, size, current_quality);
	config.mask = size >= 32 ? get32(msg + 28) : current_mask;
	if (config.mask < 1 || config.mask > 3)
		config.mask = current_mask;

	if (!live_valid_fps(fps) || rate < LIVE_MIN_RATE ||
	    rate > LIVE_MAX_RATE || !sequence ||
	    !live_valid_quality_target(config.quality))
		return true;
	if (size >= 24 && get32(msg + 20) > 1)
		return true;

	if (ops->configure)
		ops->configure(&config, now);
	return true;
}

bool live_control_handle(const uint8_t *msg, int size, uint64_t now,
			 uint32_t current_mask, bool current_combined,
			 unsigned current_quality,
			 const struct live_control_ops *ops)
{
	if (!msg || size < 4 || !ops)
		return false;

	if (size == 16 && is_tag(msg, size, "R21C")) {
		uint32_t requested;

		if (get32(msg + 8) != 1)
			return true;
		requested = get32(msg + 12);
		if (requested <= 1 && ops->set_pacing)
			ops->set_pacing(requested, now);
		return true;
	}

	if (size == 12 && is_tag(msg, size, "R20A")) {
		if (get32(msg + 8) == 1 && ops->ack_r20)
			ops->ack_r20(now);
		return true;
	}

	if (size == 12 && is_tag(msg, size, "R23C")) {
		uint32_t mask = get32(msg + 8);

		if (mask >= 1 && mask <= 3 && ops->set_mask)
			ops->set_mask(mask, now);
		return true;
	}

	if (size == 12 && is_tag(msg, size, "R19A")) {
		if (get32(msg + 8) == 1 && ops->repair_peer)
			ops->repair_peer(now);
		return true;
	}

	if (size >= 4 && is_tag(msg, size, "R19N")) {
		if (ops->repair_request)
			ops->repair_request(msg, size, now);
		return true;
	}

	if (size == 16 && is_tag(msg, size, "R17A")) {
		uint32_t flags = get32(msg + 8);
		uint32_t sequence = get32(msg + 12);

		if ((flags <= 1 && (flags || !sequence)) && ops->ack_rows)
			ops->ack_rows(flags, sequence, now);
		return true;
	}

	if (size == 28 && is_tag(msg, size, "R3FB")) {
		if (ops->feedback)
			ops->feedback(get32(msg + 8), get32(msg + 16), now);
		return true;
	}

	return handle_config(msg, size, now, current_mask, current_combined,
			     current_quality, ops);
}
