/*
 * Media socket lifecycle and portable status snapshot.
 */
#include "live_net.h"

#include "live_discovery.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

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
