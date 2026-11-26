#ifndef LIVE_AUDIO_H
#define LIVE_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* R9's custom independent stereo IMA blocks, not WAV IMA or RTP DVI4.
 * All wire integers are little-endian. The two channel headers are
 * {int16 first_sample, uint8 initial_index, uint8 reserved_zero}; remaining
 * bytes hold one stereo sample, left in the low nibble and right in the high.
 * The exact first sample is part of the 160 sample frames in each block. */
#define LIVE_AUDIO_RATE_MHZ 32728498u
#define LIVE_AUDIO_SAMPLES 160u
#define LIVE_AUDIO_CHANNELS 2u
#define LIVE_AUDIO_BLOCK_BYTES 167u
#define LIVE_AUDIO_RECORD_BYTES 175u
#define LIVE_AUDIO_HEADER_BYTES 24u
#define LIVE_AUDIO_MAX_RECORDS 6u
#define LIVE_AUDIO_MAX_PACKET_BYTES                                            \
	(LIVE_AUDIO_HEADER_BYTES +                                             \
	 LIVE_AUDIO_MAX_RECORDS * LIVE_AUDIO_RECORD_BYTES)

typedef struct {
	uint8_t index[LIVE_AUDIO_CHANNELS];
} LiveAudioState;

/* Encoder state contains only the two carried step indices. Each block
 * includes those starting indices, so decoding never needs earlier blocks.
 * A null state is allowed for init/reset and is ignored. */
void live_audio_init(LiveAudioState *state);
void live_audio_reset(LiveAudioState *state);

/* pcm contains interleaved signed 16-bit L/R samples. sample_frames must be
 * exactly LIVE_AUDIO_SAMPLES and dst_capacity at least BLOCK_BYTES.
 * Returns BLOCK_BYTES on success, zero on invalid input. Failure leaves
 * both state and dst unchanged. Input/output buffers may overlap; state
 * must not overlap either buffer. No allocation or floating point. */
size_t live_audio_encode(LiveAudioState *state, const int16_t *pcm,
			 size_t sample_frames, uint8_t *dst,
			 size_t dst_capacity);

/* src_bytes must be exactly BLOCK_BYTES; both reserved bytes must be zero
 * and both step indices at most 88. pcm_capacity_frames counts stereo
 * sample frames, not scalar int16 values. Returns SAMPLES on success, zero
 * on invalid input without modifying pcm. Input/output may overlap. */
size_t live_audio_decode(const uint8_t *src, size_t src_bytes, int16_t *pcm,
			 size_t pcm_capacity_frames);

#ifdef __cplusplus
}
#endif
#endif
