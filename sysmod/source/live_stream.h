#ifndef LIVE_STREAM_H
#define LIVE_STREAM_H
#include <3ds.h>
bool live_requested(void);
bool live_peer_ready(void);
/* The paired player has reported new complete video in the last four seconds. */
bool live_receiver_has_video(void);
/* Yield the media workers while HOME switches displays; keep the peer and configuration. */
void live_set_capture_paused(bool paused);
u32 live_fps(void);
u32 live_encoded_frames(unsigned screen);
/* Allocator owns page-backed SYSTEM arenas, freed after live_stop returns. */
Result live_start(void *(*allocate)(u32 bytes));
void live_stop(void);
/* Flush the latest socket-worker snapshot from the status writer. */
void live_flush_network_report(bool force);
bool live_submit(int screen,const u8 *frame,u32 format,u32 stride,u32 timeUs,bool wide);
void live_schedule(u32 stamp,u32 topCapture,u32 bottomCapture,u32 validMask);
/* Read-only DSP capture hands off one 160-frame stereo PCM16 block. The
 * producer never waits for encoding or the network; pts shares video epoch. */
bool live_audio_active(void);
bool live_audio_submit(const s16 *stereo160,u32 pts_us);
#endif
