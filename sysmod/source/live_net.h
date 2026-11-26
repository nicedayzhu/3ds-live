/*
 * Socket + discovery owner.
 *
 * Discovery probes and the periodic network-configuration check live here.
 * Policy that belongs to the stream (peer reset, receiver feedback, report
 * scheduling, packet pacing) is injected through live_net_ops.
 */
#ifndef LIVE_NET_H
#define LIVE_NET_H

#include "live_discovery.h"
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
	LiveDiscovery discovery;
	uint64_t last_probe_tick;
	uint64_t last_peer_control;
	struct live_net_diag diag;
};

struct live_net_ops {
	bool (*paced)(void);
	void (*sleep_until)(uint64_t due);
	void (*charge)(uint64_t *due, uint64_t begin, uint64_t returned,
		       uint32_t wire, int kind);
	void (*observe)(uint64_t begin, uint64_t returned, uint32_t wire);
	void (*report)(void);
	void (*reset_peer)(void);
	void (*reset_feedback)(void);
};

int live_net_open(struct live_net *net);
void live_net_close(struct live_net *net);

/* Send one discovery probe; used by poll and by invitation replies. */
bool live_net_send_probe(struct live_net *net, const uint8_t *probe,
			 uint32_t address, uint64_t *due,
			 const struct live_net_ops *ops);

/* Periodic lease check, address-change detection and probe scheduling. */
void live_net_poll(struct live_net *net, uint32_t session, uint64_t now,
		   uint64_t *due, const struct live_net_ops *ops);

void live_net_snapshot(const struct live_net *net, uint32_t session,
		       struct live_net_report *snapshot);

#endif /* LIVE_NET_H */
