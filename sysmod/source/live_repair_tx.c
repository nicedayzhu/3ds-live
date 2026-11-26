/*
 * R19P spare-slot transmission.
 */
#include "live_repair_tx.h"

#include "live_pacing.h"
#include "live_repair_packet.h"
#include "packet_report.h"

#include <3ds.h>
#include <string.h>
#include <unistd.h>

bool live_repair_tx_send(struct live_repair_tx *tx, LiveRepairHistory *history,
			 const uint8_t *arena, uint32_t arena_size,
			 uint32_t fps, uint64_t *total_due, uint64_t now,
			 uint64_t next_fresh_due,
			 const struct live_repair_io *io)
{
	LiveRepairFrame *frame;
	uint32_t index, offset, wire;
	size_t payload;
	uint8_t packet[LIVE_REPAIR_HEADER_BYTES + 1168];
	uint64_t begin, returned;
	int sent;

	(void)fps;
	tx->last_wire = 0;

	if (!tx->credit || now < *total_due ||
	    (io->stopping && io->stopping()) ||
	    (io->capture_paused && io->capture_paused()))
		return false;

	frame = live_repair_next(history, now);
	if (!frame)
		return false;

	index = frame->indices[frame->next];
	offset = index * 1168u;
	payload = live_repair_payload_size(frame, index);
	if (!payload || offset + payload > arena_size)
		return false;
	wire = (uint32_t)payload + 60;
	if (wire > tx->credit)
		return false;
	if (next_fresh_due && live_packet_due(*total_due, now, wire, io->rate,
					      SYSCLOCK_ARM11) > next_fresh_due)
		return false;

	if (!live_repair_packet_build(packet, sizeof(packet), frame, index,
				      io->session))
		return false;
	memcpy(packet + LIVE_REPAIR_HEADER_BYTES,
	       arena + frame->offset + offset, payload);

	begin = svcGetSystemTick();
	sent = sendto(io->fd, packet, LIVE_REPAIR_HEADER_BYTES + payload, 0,
		      (const struct sockaddr *)io->target, sizeof(*io->target));
	returned = svcGetSystemTick();
	io->charge(total_due, begin, returned, wire, PACKET_REPAIR);

	if (!frame->next && sent == (int)(LIVE_REPAIR_HEADER_BYTES + payload) &&
	    io->report)
		io->report(frame->seq, frame->pts, index, begin, returned);

	tx->credit -= wire;
	frame->next++;
	if (sent == (int)(LIVE_REPAIR_HEADER_BYTES + payload)) {
		tx->packets++;
		tx->wire_bytes += wire;
		tx->last_wire = wire;
	}
	return true;
}
