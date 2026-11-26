/*
 * Host tests for the pure network-status formatter.
 */
#include "live_net_report.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_formats_known_snapshot(void)
{
	/* 192.168.0.103, 255.255.255.0, 192.168.0.104. */
	const struct live_net_report snapshot = {
	    .session = 0x23ca830e,
	    .automatic = true,
	    .peer_ready = true,
	    .polls = 5,
	    .local_address = 0xc0a80067,
	    .local_mask = 0xffffff00,
	    .last_destination = 0xc0a80068,
	    .peer_address = 0xc0a80068,
	    .local_port = 8002,
	    .media_port = 8001,
	    .info_result = 0,
	    .info_errno = 0,
	    .socket_errno = 0,
	    .bind_errno = 0,
	    .probe_attempts = 11,
	    .probes_sent = 11,
	    .probe_errno = 0,
	    .receive_errno = 0,
	    .received_datagrams = 2,
	    .accepted_replies = 1,
	    .invites = 1,
	    .invites_answered = 1,
	};
	char text[LIVE_NET_REPORT_BYTES];
	size_t length;

	length = live_net_report_format(text, sizeof(text), &snapshot);
	assert(length > 0);
	assert(strstr(text, "session=23ca830e\n"));
	assert(strstr(text, "automatic=1\n"));
	assert(strstr(text, "peer_ready=1\n"));
	assert(strstr(text, "local=192.168.0.103\n"));
	assert(strstr(text, "mask=255.255.255.0\n"));
	assert(strstr(text, "last_probe_destination=192.168.0.104\n"));
	assert(strstr(text, "peer=192.168.0.104\n"));
	assert(strstr(text, "local_port=8002\n"));
	assert(strstr(text, "media_port=8001\n"));
	assert(strstr(text, "received_datagrams=2\n"));
	assert(strstr(text, "accepted_replies=1\n"));
}

static void test_rejects_small_buffer(void)
{
	const struct live_net_report snapshot = {0};
	char text[8];

	assert(live_net_report_format(text, sizeof(text), &snapshot) == 0);
	assert(live_net_report_format(NULL, 0, &snapshot) == 0);
}

int main(void)
{
	test_formats_known_snapshot();
	test_rejects_small_buffer();
	puts("test_live_net_report: ok");
	return 0;
}
