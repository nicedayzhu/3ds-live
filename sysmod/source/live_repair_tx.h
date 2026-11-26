/*
 * Spare-slot R19P repair transmission.
 */
#ifndef LIVE_REPAIR_TX_H
#define LIVE_REPAIR_TX_H

#include "live_repair.h"

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

struct live_repair_tx {
	uint32_t credit;
	uint32_t packets;
	uint32_t wire_bytes;
	uint32_t last_wire;
};

struct live_repair_io {
	int fd;
	const struct sockaddr_in *target;
	uint32_t session;
	uint32_t rate;
	bool (*stopping)(void);
	bool (*capture_paused)(void);
	void (*charge)(uint64_t *due, uint64_t begin, uint64_t returned,
		       uint32_t wire, int kind);
	void (*report)(uint32_t seq, uint32_t pts, uint32_t index,
		       uint64_t begin, uint64_t returned);
};

/*
 * Try one repair fragment.  The history and arena stay owned by the sender.
 * `next_fresh_due` may be 0 when no fresh packet is pending.
 */
bool live_repair_tx_send(struct live_repair_tx *tx, LiveRepairHistory *history,
			 const uint8_t *arena, uint32_t arena_size,
			 uint32_t fps, uint64_t *total_due, uint64_t now,
			 uint64_t next_fresh_due,
			 const struct live_repair_io *io);

#endif /* LIVE_REPAIR_TX_H */
