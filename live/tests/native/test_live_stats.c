/*
 * Host test for the live-stats.txt snapshot writer.
 */
#include "live_stats.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *test_path = "live-stats-test.txt";

static void read_file(char *text, size_t capacity)
{
	FILE *file = fopen(test_path, "r");
	size_t size;

	assert(file);
	size = fread(text, 1, capacity - 1, file);
	text[size] = '\0';
	fclose(file);
}

static void test_writes_snapshot(void)
{
	struct live_stats stats = {
	    .combined_fec = true,
	    .sent_top = 101,
	    .sent_bottom = 102,
	    .rate = 2000000,
	    .fps = 30,
	    .duration_ms = 1234,
	    .pair_payload_budget = 33000,
	    .compact_jpeg = true,
	    .r20_attempts = 4,
	    .r20_encode_sum_us = 400,
	    .audio_enabled = true,
	    .audio_packets_sent = 9,
	    .screens =
		{
		    {.attempts = 11,
		     .jpeg = 10,
		     .encode_sum_us = 500,
		     .encoder_core = 2},
		    {.attempts = 12,
		     .jpeg = 11,
		     .encode_sum_us = 600,
		     .encoder_core = 3},
		},
	};
	char text[4096];

	assert(live_stats_write(test_path, &stats, "session=test\n", 13));
	read_file(text, sizeof(text));
	assert(strstr(text, "session=test\n"));
	assert(strstr(text, "version=live-r10-fec4-candidate\n"));
	assert(strstr(text, "sent_top=101\nsent_bottom=102\n"));
	assert(strstr(text, "pair_jpeg_budget="));
	assert(strstr(text, "r20_encode_mean_us=100\n"));
	assert(strstr(text, "audio_packets_sent=9\n"));
	assert(strstr(text, "screen0_attempts=11\n"));
	assert(strstr(text, "screen0_encode_mean_us=45\n"));
	assert(strstr(text, "screen1_encoder_core=3\n"));
}

static void test_rejects_invalid_path(void)
{
	struct live_stats stats = {0};

	assert(!live_stats_write("Z:/no-such-dir/live-stats.txt", &stats, NULL,
				 0));
	assert(!live_stats_write(NULL, &stats, NULL, 0));
}

int main(void)
{
	test_writes_snapshot();
	test_rejects_invalid_path();
	remove(test_path);
	puts("test_live_stats: ok");
	return 0;
}
