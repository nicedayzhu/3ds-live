#ifndef LIVE_RECEIVER_FEEDBACK_H
#define LIVE_RECEIVER_FEEDBACK_H
#include <stdbool.h>
#include <stdint.h>

/* R3FB carries the PC's monotonic milliseconds and its cumulative count of
 * completed screens. Only the socket worker updates this history. */
typedef struct {
	uint32_t pc_ms, completed;
	uint64_t last_report, last_progress;
	bool seen, progressing;
} LiveReceiverFeedback;

static inline void live_receiver_observe(LiveReceiverFeedback *feedback,
					 uint32_t pc_ms, uint32_t completed,
					 uint64_t now, uint64_t timeout)
{
	const bool contiguous =
	    feedback->seen && now - feedback->last_report < timeout;
	if (contiguous) {
		const uint32_t elapsed = pc_ms - feedback->pc_ms;
		if (!elapsed || elapsed >= 0x80000000u)
			return; /* duplicate or reordered */
	}
	const uint32_t added = completed - feedback->completed;
	if (!contiguous || added >= 0x80000000u) {
		/* A new endpoint, long gap or PC restart must establish a new
		 * baseline. A single queued report containing an old nonzero
		 * count proves nothing. */
		feedback->progressing = false;
	} else if (added) {
		feedback->progressing = true;
		feedback->last_progress = now;
	}
	feedback->seen = true;
	feedback->pc_ms = pc_ms;
	feedback->completed = completed;
	feedback->last_report = now;
}

static inline bool live_receiver_recent(const LiveReceiverFeedback *feedback,
					uint64_t now, uint64_t timeout)
{
	return feedback->progressing && now - feedback->last_progress < timeout;
}

#endif
