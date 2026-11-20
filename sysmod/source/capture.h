#ifndef LIVE_CAPTURE_H
#define LIVE_CAPTURE_H

#include <3ds.h>

bool capture_start(void);
void capture_poll(void);
void capture_invalidate(void);
void capture_reset(void);
void capture_stop(void);
u32 capture_application_pid(void);

#endif
