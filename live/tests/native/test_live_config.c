/*
 * Host tests for live.ini parsing.  The file is intentionally written next to
 * the test binary so the suite needs no SD card or libctru.
 */
#include "live_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test_path = "live-config-test.ini";

static void write_file(const char *text)
{
	FILE *file = fopen(test_path, "w");

	assert(file);
	fputs(text, file);
	fclose(file);
}

static void test_missing_file_keeps_defaults(void)
{
	struct live_config config;

	remove(test_path);
	assert(live_config_load(&config, test_path));
	assert(config.enabled);
	assert(config.fps == 30);
	assert(config.rate == 800000);
	assert(config.port == 8001);
	assert(config.automatic_host);
	assert(strcmp(config.host, "auto") == 0);
}

static void test_parses_comments_and_keys(void)
{
	struct live_config config;

	write_file("# comment\n"
		   "enabled=on\n"
		   "host=192.168.0.104\n"
		   "port=9000\n"
		   "fps=24\n"
		   "bytes_per_second=1500000\n"
		   "compact_jpeg=off\n"
		   "audio=off\n"
		   "combined_fec=off\n");

	assert(live_config_load(&config, test_path));
	assert(config.enabled);
	assert(!config.automatic_host);
	assert(strcmp(config.host, "192.168.0.104") == 0);
	assert(config.port == 9000);
	assert(config.fps == 24);
	assert(config.rate == 1500000);
	assert(!config.compact_jpeg);
	assert(!config.audio_enabled);
	assert(!config.combined_fec);
	assert(live_config_valid(&config));
}

static void test_rejects_invalid_values(void)
{
	struct live_config config;

	write_file("enabled=on\nhost=127.0.0.1\nfps=17\n"
		   "bytes_per_second=100\nport=0\n");
	assert(live_config_load(&config, test_path));
	assert(!live_config_valid(&config));

	write_file("enabled=on\nhost=auto\nfps=30\n"
		   "bytes_per_second=2000000\nport=8001\n");
	assert(live_config_load(&config, test_path));
	assert(live_config_valid(&config));
}

int main(void)
{
	test_missing_file_keeps_defaults();
	test_parses_comments_and_keys();
	test_rejects_invalid_values();
	remove(test_path);
	puts("test_live_config: ok");
	return 0;
}
