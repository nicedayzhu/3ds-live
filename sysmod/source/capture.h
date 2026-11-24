#ifndef LIVE_CAPTURE_H
#define LIVE_CAPTURE_H

#include <3ds.h>

#define LIVE_CAPTURE_MAX_STRIDE (240u * 4u * 2u)
#define LIVE_CAPTURE_MAX_DMA_BYTES ((800u + 320u) * LIVE_CAPTURE_MAX_STRIDE)

bool capture_start(void);
void capture_poll(void);
void capture_invalidate(void);
void capture_reset(void);
void capture_stop(void);
u32 capture_application_pid(void);

#endif
