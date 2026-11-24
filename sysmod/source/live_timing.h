#ifndef LIVE_TIMING_H
#define LIVE_TIMING_H
#include <3ds.h>

typedef enum {
    TIMING_SENT = 1, TIMING_REPLACED, TIMING_OVERSIZE, TIMING_RECONFIGURED,
    TIMING_TOO_OLD, TIMING_DEADLINE, TIMING_SOCKET_ERROR, TIMING_STOPPED
} LiveTimingOutcome;

#ifdef LIVE_FRAME_TIMING
void live_timing_prepare(void *(*allocate)(u32));
void live_timing_begin(u32 seq, u32 pts);
void live_timing_encoded(void);
void live_timing_publish(unsigned slot, u64 queued);
void live_timing_drop_encoded(LiveTimingOutcome why);
void live_timing_drop_pending(unsigned slot, LiveTimingOutcome why);
void live_timing_claim(unsigned slot);
void live_timing_plan(u32 dataFragments, u32 allFragments);
void live_timing_packet(u64 call, u64 returned, bool accepted);
void live_timing_finish(LiveTimingOutcome why);
bool live_timing_marker(u64 now, u32 *capacity);
bool live_timing_export(u32 session, u64 started);
#else
#define live_timing_prepare(...)      ((void)0)
#define live_timing_begin(...)        ((void)0)
#define live_timing_encoded()         ((void)0)
#define live_timing_publish(...)      ((void)0)
#define live_timing_drop_encoded(...) ((void)0)
#define live_timing_drop_pending(...) ((void)0)
#define live_timing_claim(...)        ((void)0)
#define live_timing_plan(...)         ((void)0)
#define live_timing_packet(...)       ((void)0)
#define live_timing_finish(...)       ((void)0)
#endif
#endif
