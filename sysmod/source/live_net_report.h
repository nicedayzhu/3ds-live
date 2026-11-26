/*
 * network-status.txt reporting.
 *
 * The snapshot is deliberately made of plain values: the socket worker owns
 * the live counters, copies them once, and the writer never touches stream
 * state.  Formatting is kept separate so it can be tested on the host.
 */
#ifndef LIVE_NET_REPORT_H
#define LIVE_NET_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIVE_NET_REPORT_BYTES 768

struct live_net_report {
	uint32_t session;
	bool automatic;
	bool peer_ready;
	uint32_t polls;
	uint32_t local_address;
	uint32_t local_mask;
	uint32_t last_destination;
	uint32_t peer_address;
	uint32_t local_port;
	uint32_t media_port;
	int info_result;
	int info_errno;
	int socket_errno;
	int bind_errno;
	uint32_t probe_attempts;
	uint32_t probes_sent;
	int probe_errno;
	int receive_errno;
	uint32_t received_datagrams;
	uint32_t accepted_replies;
	uint32_t invites;
	uint32_t invites_answered;
};

/* Format one complete report; returns 0 when it does not fit. */
size_t live_net_report_format(char *text, size_t capacity,
			      const struct live_net_report *snapshot);

/* Initialize the writer lock once, before the first queue/flush. */
void live_net_report_init(void);

/* Publish a snapshot for the single writer thread. */
void live_net_report_queue(const struct live_net_report *snapshot);

/* Write the newest snapshot to sdmc:/3ds/3ds-live/network-status.txt. */
void live_net_report_flush(bool force);

#endif /* LIVE_NET_REPORT_H */
