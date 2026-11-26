/*
 * Configuration file handling for the 3DS Live system module.
 *
 * The parser is intentionally independent of libctru so it can be exercised
 * on the host.  Validation is a separate step: a caller may load a config,
 * inspect it and then decide whether it is usable.
 */
#ifndef LIVE_CONFIG_H
#define LIVE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIVE_CONFIG_PATH "sdmc:/3ds/3ds-live/live.ini"
#define LIVE_CONFIG_HOST_MAX 64

struct live_config {
	bool enabled;
	bool compact_jpeg;
	bool audio_enabled;
	bool combined_fec;
	uint32_t fps;
	uint32_t rate;
	uint16_t port;
	bool automatic_host;
	char host[LIVE_CONFIG_HOST_MAX];
};

/* Fill cfg with the built-in defaults. */
void live_config_defaults(struct live_config *cfg);

/*
 * Load path into cfg.
 *
 * Missing files and malformed lines are not errors: the corresponding keys
 * keep their defaults, matching the historical live.ini behaviour.  Returns
 * false only when cfg or path is invalid.
 */
bool live_config_load(struct live_config *cfg, const char *path);

/* True when fps, rate, port and host describe a usable stream. */
bool live_config_valid(const struct live_config *cfg);

/* True for a unicast IPv4 address that is not loopback or 0.0.0.0. */
bool live_config_unicast_host(const char *host);

#endif /* LIVE_CONFIG_H */
