/*
 * R19P repair packet header.
 *
 * Pure layout code so the host test can check every field without a socket.
 */
#ifndef LIVE_REPAIR_PACKET_H
#define LIVE_REPAIR_PACKET_H

#include "live_repair.h"

#include <stddef.h>
#include <stdint.h>

#define LIVE_REPAIR_HEADER_BYTES 32u

/*
 * Write the 32-byte R19P header.  Returns the header size, or 0 when the
 * chunk index is outside the frame.
 */
size_t live_repair_packet_build(uint8_t *packet, size_t capacity,
				const LiveRepairFrame *frame, uint32_t index,
				uint32_t session);

/* Payload length for index; 0 when out of range. */
size_t live_repair_payload_size(const LiveRepairFrame *frame, uint32_t index);

#endif /* LIVE_REPAIR_PACKET_H */
