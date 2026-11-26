/*
 * Media socket and peer addressing state.
 *
 * Discovery, control and sender code share one endpoint description.  Keeping
 * it in one struct prevents the three subsystems from growing their own copies
 * of fd/target/ready flags.
 */
#ifndef LIVE_NET_H
#define LIVE_NET_H

#include "live_net_report.h"

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

struct live_net_diag {
	uint32_t polls;
	uint32_t probe_attempts;
	uint32_t probes_sent;
	uint32_t replies;
	uint32_t accepted;
	uint32_t hints;
	uint32_t hints_answered;
	uint32_t reported_broadcast;
	uint32_t last_destination;
	int info_result;
	int info_errno;
	int socket_errno;
	int bind_errno;
	int send_errno;
	int receive_errno;
	unsigned local_port;
	uint64_t saved_at;
	uint64_t last_hint_tick;
};

struct live_net {
	int fd;
	struct sockaddr_in target;
	bool automatic_host;
	bool media_ready;
	uint32_t local_address;
	uint32_t local_mask;
	struct live_net_diag diag;
};

/* Open the non-blocking UDP media socket. */
int live_net_open(struct live_net *net);

void live_net_close(struct live_net *net);

/* Fill the portable network-status snapshot. */
void live_net_snapshot(const struct live_net *net, uint32_t session,
		       struct live_net_report *snapshot);

#endif /* LIVE_NET_H */
