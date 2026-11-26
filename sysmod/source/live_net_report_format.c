/*
 * Pure formatter for network-status.txt.  No libctru, no sockets, no file
 * system: the host test exercises exactly the bytes the console writes.
 */
#include "live_net_report.h"

#include <arpa/inet.h>
#include <stdio.h>

static const char *format_address(uint32_t address, char *text)
{
	struct in_addr ip = {.s_addr = htonl(address)};

	return inet_ntop(AF_INET, &ip, text, INET_ADDRSTRLEN) ? text : "?";
}

size_t live_net_report_format(char *text, size_t capacity,
			      const struct live_net_report *snapshot)
{
	char local[INET_ADDRSTRLEN], mask[INET_ADDRSTRLEN];
	char destination[INET_ADDRSTRLEN], peer[INET_ADDRSTRLEN];
	int length;

	if (!text || !capacity || !snapshot)
		return 0;

	length = snprintf(
	    text, capacity,
	    "session=%08lx\nautomatic=%u\npeer_ready=%u\nnetwork_polls=%lu\n"
	    "local=%s\nmask=%s\nlast_probe_destination=%s\npeer=%s\nlocal_port="
	    "%lu\nmedia_port=%lu\n"
	    "ip_info_result=%d\nip_info_errno=%d\nsocket_errno=%d\nbind_errno=%"
	    "d\n"
	    "probe_attempts=%lu\nprobes_sent=%lu\nprobe_errno=%d\nreceive_"
	    "errno=%d\n"
	    "received_datagrams=%lu\naccepted_replies=%lu\ninvites=%"
	    "lu\ninvites_answered=%lu\n",
	    (unsigned long)snapshot->session, snapshot->automatic,
	    snapshot->peer_ready, (unsigned long)snapshot->polls,
	    format_address(snapshot->local_address, local),
	    format_address(snapshot->local_mask, mask),
	    format_address(snapshot->last_destination, destination),
	    format_address(snapshot->peer_address, peer),
	    (unsigned long)snapshot->local_port,
	    (unsigned long)snapshot->media_port, snapshot->info_result,
	    snapshot->info_errno, snapshot->socket_errno, snapshot->bind_errno,
	    (unsigned long)snapshot->probe_attempts,
	    (unsigned long)snapshot->probes_sent, snapshot->probe_errno,
	    snapshot->receive_errno,
	    (unsigned long)snapshot->received_datagrams,
	    (unsigned long)snapshot->accepted_replies,
	    (unsigned long)snapshot->invites,
	    (unsigned long)snapshot->invites_answered);

	return length > 0 && (size_t)length < capacity ? (size_t)length : 0;
}
