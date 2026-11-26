/*
 * live.ini parsing and validation.
 *
 * Kept free of libctru and of the streaming state so the host tests can
 * cover malformed files, comments and the conservative defaults.
 */
#include "live_config.h"

#include "live_discovery.h"
#include "live_fps.h"
#include "live_pair.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIVE_CONFIG_LINE_MAX 128

static char *trim(char *text)
{
	char *end;

	while (*text && isspace((unsigned char)*text))
		text++;

	end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1]))
		*--end = '\0';

	return text;
}

static void set_word(char *value)
{
	char *space = value;

	while (*space && !isspace((unsigned char)*space))
		space++;
	*space = '\0';
}

static void parse_line(struct live_config *cfg, char *line)
{
	char *key, *value, *equals;

	key = trim(line);
	if (!*key || *key == '#' || *key == ';')
		return;

	equals = strchr(key, '=');
	if (!equals)
		return;
	*equals = '\0';

	value = trim(equals + 1);
	set_word(value);
	if (!*value)
		return;
	key = trim(key);

	if (!strcmp(key, "enabled"))
		cfg->enabled = !strcmp(value, "on");
	else if (!strcmp(key, "host"))
		snprintf(cfg->host, sizeof(cfg->host), "%s", value);
	else if (!strcmp(key, "port"))
		cfg->port = live_config_port(value);
	else if (!strcmp(key, "fps"))
		cfg->fps = live_migrate_fps((uint32_t)strtoul(value, NULL, 10));
	else if (!strcmp(key, "bytes_per_second"))
		cfg->rate = (uint32_t)strtoul(value, NULL, 10);
	else if (!strcmp(key, "compact_jpeg"))
		cfg->compact_jpeg = !strcmp(value, "on");
	else if (!strcmp(key, "audio"))
		cfg->audio_enabled = strcmp(value, "off") != 0;
	else if (!strcmp(key, "combined_fec"))
		cfg->combined_fec = !strcmp(value, "on");
}

void live_config_defaults(struct live_config *cfg)
{
	if (!cfg)
		return;

	memset(cfg, 0, sizeof(*cfg));
	cfg->enabled = true;
	cfg->compact_jpeg = true;
	cfg->audio_enabled = true;
	cfg->combined_fec = true;
	cfg->fps = 30;
	cfg->rate = 800000;
	cfg->port = 8001;
	cfg->automatic_host = true;
	snprintf(cfg->host, sizeof(cfg->host), "auto");
}

bool live_config_load(struct live_config *cfg, const char *path)
{
	FILE *file;
	char line[LIVE_CONFIG_LINE_MAX];

	if (!cfg || !path)
		return false;

	live_config_defaults(cfg);
	cfg->automatic_host = true;

	file = fopen(path, "r");
	if (!file)
		return true;

	while (fgets(line, sizeof(line), file))
		parse_line(cfg, line);
	fclose(file);

	cfg->automatic_host = !strcmp(cfg->host, "auto");
	return true;
}

bool live_config_unicast_host(const char *host)
{
	struct in_addr address;

	if (!host || inet_pton(AF_INET, host, &address) != 1)
		return false;

	return live_unicast_address(ntohl(address.s_addr));
}

bool live_config_valid(const struct live_config *cfg)
{
	if (!cfg || !cfg->enabled)
		return false;
	if (!live_valid_fps(cfg->fps))
		return false;
	if (cfg->rate < LIVE_MIN_RATE || cfg->rate > LIVE_MAX_RATE)
		return false;
	if (cfg->port < 1)
		return false;
	if (!cfg->automatic_host && !live_config_unicast_host(cfg->host))
		return false;

	return true;
}
