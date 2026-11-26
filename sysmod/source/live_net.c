/*
 * Media socket lifecycle, discovery probes and portable status snapshot.
 */
#include "live_net.h"

#include "packet_report.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static bool paced(const struct live_net_ops *ops)
{
	return ops->paced && ops->paced();
}

int live_net_open(struct live_net *net)
{
	struct sockaddr_in local = {
	    .sin_family = AF_INET,
	    .sin_port = htons(net->automatic_host ? LIVE_DISCOVERY_PORT : 0)};
	int sock;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	net->diag.socket_errno = sock < 0 ? errno : 0;
	if (sock < 0)
		return -1;

	if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
		net->diag.socket_errno = errno;
		close(sock);
		return -1;
	}
	net->diag.bind_errno =
	    bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0 ? errno
								     : 0;
	if (!net->diag.bind_errno)
		net->diag.local_port = ntohs(local.sin_port);
	net->fd = sock;
	return sock;
}

void live_net_close(struct live_net *net)
{
	if (net->fd >= 0) {
		close(net->fd);
		net->fd = -1;
	}
}

bool live_net_send_probe(struct live_net *net, const uint8_t *probe,
			 uint32_t address, uint64_t *due,
			 const struct live_net_ops *ops)
{
	struct sockaddr_in destination = net->target;
	int sent;
	uint64_t begin = 0, returned = 0;
	bool paired;

	if (!probe || !due || !ops || net->fd < 0)
		return false;

	destination.sin_addr.s_addr = htonl(address);
	net->diag.last_destination = address;
	net->diag.probe_attempts++;
	paired = net->media_ready && address == net->discovery.peer;
	if (paired && paced(ops))
		ops->sleep_until(*due);
	if (paired)
		begin = svcGetSystemTick();
	sent = sendto(net->fd, probe, LIVE_DISCOVERY_BYTES, 0,
		      (struct sockaddr *)&destination, sizeof(destination));
	if (paired) {
		returned = svcGetSystemTick();
		if (paced(ops))
			ops->charge(due, (uint64_t)begin, (uint64_t)returned,
				    LIVE_DISCOVERY_BYTES + 28, PACKET_STATUS);
		else if (ops->observe)
			ops->observe((uint64_t)begin, (uint64_t)returned, 0);
	}
	if (sent != LIVE_DISCOVERY_BYTES) {
		net->diag.send_errno = errno;
		return false;
	}
	net->diag.probes_sent++;
	net->diag.send_errno = 0;
	if (!net->diag.local_port) {
		struct sockaddr_in local = {0};
		socklen_t size = sizeof(local);

		if (getsockname(net->fd, (struct sockaddr *)&local, &size) == 0)
			net->diag.local_port = ntohs(local.sin_port);
	}
	return true;
}

void live_net_poll(struct live_net *net, uint32_t session, uint64_t now,
		   uint64_t *due, const struct live_net_ops *ops)
{
	struct in_addr ip = {0}, mask = {0}, broadcast = {0};
	uint32_t address, netmask;
	bool expired, moved;

	if (!net || !due || !ops)
		return;

	expired = live_discover_expire(&net->discovery, now, SYSCLOCK_ARM11);
	if (!expired && net->last_probe_tick &&
	    now - net->last_probe_tick < SYSCLOCK_ARM11)
		return;

	net->last_probe_tick = now;
	net->diag.polls++;
	if (net->fd < 0)
		live_net_open(net);

	/* A live reply on this socket verifies the route. Avoid a synchronous
	 * SOC network-configuration query on each second of an active stream.
	 */
	if (!expired && net->media_ready && net->last_peer_control &&
	    now >= net->last_peer_control &&
	    now - net->last_peer_control <
		(uint64_t)LIVE_PEER_LEASE_SECONDS * SYSCLOCK_ARM11) {
		if (net->automatic_host && net->discovery.peer) {
			uint8_t probe[LIVE_DISCOVERY_BYTES];

			live_discover_invited_probe(&net->discovery, session,
						    probe);
			live_net_send_probe(net, probe, net->discovery.peer,
					    due, ops);
		}
		return;
	}

	net->diag.info_result = SOCU_GetIPInfo(&ip, &mask, &broadcast);
	net->diag.info_errno = net->diag.info_result ? errno : 0;
	if (!ip.s_addr) {
		long host = gethostid();

		if (host != -1)
			ip.s_addr = (uint32_t)host;
	}
	address = ntohl(ip.s_addr);
	netmask = ntohl(mask.s_addr);
	moved = address && net->local_address &&
		(address != net->local_address ||
		 (netmask && net->local_mask && netmask != net->local_mask));

	if (expired || moved) {
		__atomic_store_n(&net->media_ready, false, __ATOMIC_RELEASE);
		if (ops->reset_feedback)
			ops->reset_feedback();
		net->discovery.peer = 0;
		net->discovery.outstanding = false;
		if (net->automatic_host)
			net->target.sin_addr.s_addr = 0;
		if (ops->reset_peer)
			ops->reset_peer();
		live_net_close(net);
		net->diag.local_port = 0;
		live_net_open(net);
	}

	if (address) {
		if (address != net->local_address || netmask)
			net->local_mask = netmask;
		net->local_address = address;
	}
	net->diag.reported_broadcast = ntohl(broadcast.s_addr);
	if (!net->media_ready &&
	    (net->diag.polls == 2 ||
	     now - net->diag.saved_at >= 5ull * SYSCLOCK_ARM11) &&
	    ops->report)
		ops->report();

	if (net->fd < 0)
		return;

	if (!net->automatic_host) {
		bool usable =
		    live_peer_address(ntohl(net->target.sin_addr.s_addr),
				      net->local_address, net->local_mask);

		if (!usable && net->media_ready && ops->reset_feedback)
			ops->reset_feedback();
		if (net->last_peer_control &&
		    (!usable ||
		     now - net->last_peer_control >=
			 (uint64_t)LIVE_PEER_LEASE_SECONDS * SYSCLOCK_ARM11) &&
		    ops->reset_peer)
			ops->reset_peer();
		bool was_ready = net->media_ready;

		__atomic_store_n(&net->media_ready, usable, __ATOMIC_RELEASE);
		if (was_ready != usable && ops->report)
			ops->report();
		return;
	}

	uint8_t probe[LIVE_DISCOVERY_BYTES];
	bool sent;

	live_discover_probe(&net->discovery, session, probe);
	if (net->discovery.peer) {
		sent = live_net_send_probe(net, probe, net->discovery.peer, due,
					   ops);
	} else {
		/* Routers and SOC implementations differ in handling subnet vs
		 * limited broadcast. Both probes share one challenge and expose
		 * no media. */
		uint32_t subnet = net->diag.reported_broadcast;

		if (!subnet && net->local_address && net->local_mask)
			subnet = live_broadcast_address(net->local_address,
							net->local_mask);
		sent = subnet && subnet != UINT32_MAX &&
		       live_net_send_probe(net, probe, subnet, due, ops);
		if (live_net_send_probe(net, probe, UINT32_MAX, due, ops))
			sent = true;
	}
	if (!sent) {
		net->discovery.outstanding = false;
		/* Even if SOC refuses outbound broadcasts, the fixed listener
		 * must stay open so a PC invitation can initiate the unicast
		 * handshake. */
		if (!net->discovery.peer && net->diag.bind_errno) {
			live_net_close(net);
			net->diag.local_port = 0;
		}
	}
}

void live_net_snapshot(const struct live_net *net, uint32_t session,
		       struct live_net_report *snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->session = session;
	snapshot->automatic = net->automatic_host;
	snapshot->peer_ready = net->media_ready;
	snapshot->polls = net->diag.polls;
	snapshot->local_address = net->local_address;
	snapshot->local_mask = net->local_mask;
	snapshot->last_destination = net->diag.last_destination;
	snapshot->peer_address = ntohl(net->target.sin_addr.s_addr);
	snapshot->local_port = net->diag.local_port;
	snapshot->media_port = ntohs(net->target.sin_port);
	snapshot->info_result = net->diag.info_result;
	snapshot->info_errno = net->diag.info_errno;
	snapshot->socket_errno = net->diag.socket_errno;
	snapshot->bind_errno = net->diag.bind_errno;
	snapshot->probe_attempts = net->diag.probe_attempts;
	snapshot->probes_sent = net->diag.probes_sent;
	snapshot->probe_errno = net->diag.send_errno;
	snapshot->receive_errno = net->diag.receive_errno;
	snapshot->received_datagrams = net->diag.replies;
	snapshot->accepted_replies = net->diag.accepted;
	snapshot->invites = net->diag.hints;
	snapshot->invites_answered = net->diag.hints_answered;
}
