#ifndef LIVE_JPEG_SOURCE_H
#define LIVE_JPEG_SOURCE_H
#include <stddef.h>
#include <stdint.h>

/* Centered YCbCr420 in JPEG block order: four Y, Cb, Cr, 64 samples each.
 * Revision tracks exact source changes, not a hash. Snapshots may skip frames. */
typedef struct {
    uint64_t revision;
    signed char samples[384];
} LiveJpegMcu;
typedef struct {
    unsigned width;
    uint32_t source_id;
    LiveJpegMcu mcus[];
} LiveJpegFrame;
typedef struct LiveJpegSource LiveJpegSource;

static inline size_t live_jpeg_frame_size(unsigned width) {
    return width==400||width==320 ? sizeof(LiveJpegFrame)+(width/16)*15*sizeof(LiveJpegMcu) : 0;
}
LiveJpegSource *live_jpeg_source_create(unsigned width);
/* Caller-owned arena variant for the sysmodule's small libc heap. Memory must
 * be 8-byte aligned and persist through destroy. No allocations on update. */
size_t live_jpeg_source_storage_size(unsigned width);
LiveJpegSource *live_jpeg_source_init(void *memory,size_t bytes,unsigned width);
void live_jpeg_source_destroy(LiveJpegSource *source);
/* One producer; caller serializes update and snapshot copying. Format uses
 * GSP/convert.h values. Input is read-only, column-major, with explicit bounds.
 * Returns changed source MCUs, 0 for an unchanged frame, -1 on failure.
 * No per-frame allocations. RGBA8 alpha is ignored
 * by comparison; the reference retains 4-byte pixels for fast masked loads. */
int live_jpeg_source_update(LiveJpegSource *source,const void *pixels,size_t bytes,
                           unsigned format,unsigned stride);
const LiveJpegFrame *live_jpeg_source_frame(const LiveJpegSource *source);
/* One caller per encoder context, independent from the source producer.
 * Reset the JPEG context before releasing its last snapshot. */
int live_jpeg_prepare_yuv(unsigned screen,const LiveJpegFrame *frame,size_t bytes);
#endif
