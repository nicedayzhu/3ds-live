#ifndef LIVE_JPEG_H
#define LIVE_JPEG_H
#include <stddef.h>
#include <stdint.h>
#define LIVE_JPEG_MAX_BYTES 128000u
/* Legacy entry points use screen/context 0. quality 0=best .. 3=low. */
size_t live_jpeg_encode(const uint16_t *column_rgb565,unsigned width,unsigned height,
                        unsigned quality,uint8_t *out,size_t capacity);
/* Fine-grained JPEG quality 1..95. Same rotation and strict output capacity. */
size_t live_jpeg_encode_scaled(const uint16_t *column_rgb565,unsigned width,unsigned height,
                               unsigned quality,uint8_t *out,size_t capacity);
/* Two independent states; one producer per screen. 0 on overflow/failure.
 * Calls for different screens may run concurrently. */
size_t live_jpeg_encode_screen(unsigned screen,const uint16_t *column_rgb565,unsigned width,unsigned height,
                               unsigned quality,uint8_t *out,size_t capacity);
/* Retain exact RGB tiles, unquantized DCT and the last successful JPEG's restart
 * rows. Unchanged rows at the same quality reuse identical entropy bytes; dirty
 * rows accumulate across prepares and failed renders. No wire reference
 * dependency: every render is a complete, independently decodable JPEG. One
 * caller per context; prepare returns changed MCUs, or -1 on invalid input.
 * Output buffers need not persist. The legacy RGB reference allocates on first
 * prepare; reset frees it and invalidates DCT/JPEG caches. Live streaming uses
 * the native YCbCr snapshot interface in live_jpeg_source.h instead. */
void live_jpeg_reset(unsigned screen);
int live_jpeg_prepare(unsigned screen,const uint16_t *column_rgb565,unsigned width,unsigned height);
size_t live_jpeg_render(unsigned screen,unsigned quality,uint8_t *out,size_t capacity);
#endif
