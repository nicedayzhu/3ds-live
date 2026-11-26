#ifndef LIVE_ROW_REFRESH_H
#define LIVE_ROW_REFRESH_H
#include <stdint.h>

typedef struct {
	unsigned frames, wins;
	int64_t saved;
} LiveRowTrial;

/* Compare actual IPv4 costs of the current base and a recent local sample.
 * Four frames distinguish a persistent change of scenery from an isolated
 * match. Require projected savings over the next twelve similar frames to
 * repay a full key and the two extra sends it may require before its ACK. */
static inline int live_row_trial_observe(LiveRowTrial *trial,
					 uint32_t current_wire,
					 uint32_t trial_wire,
					 uint32_t full_wire)
{
	int64_t gain = (int64_t)current_wire - trial_wire;
	trial->saved += gain;
	if (gain * 8 >= current_wire)
		trial->wins++;
	if (++trial->frames < 4)
		return 0;
	return trial->wins >= 3 &&
		       trial->saved * 3 >= 3 * (int64_t)full_wire - current_wire
		   ? 1
		   : -1;
}
#endif
