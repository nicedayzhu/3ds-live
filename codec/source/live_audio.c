#include "live_audio.h"

#include <string.h>

static const int16_t step_table[89] = {
    7,	   8,	  9,	 10,	11,    12,    13,    14,    16,	   17,
    19,	   21,	  23,	 25,	28,    31,    34,    37,    41,	   45,
    50,	   55,	  60,	 66,	73,    80,    88,    97,    107,   118,
    130,   143,	  157,	 173,	190,   209,   230,   253,   279,   307,
    337,   371,	  408,	 449,	494,   544,   598,   658,   724,   796,
    876,   963,	  1060,	 1166,	1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,	 3024,	3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,	 7845,	8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t index_delta[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static int clamp_sample(int sample)
{
	return sample < -32768 ? -32768 : sample > 32767 ? 32767 : sample;
}

static unsigned next_index(unsigned index, unsigned nibble)
{
	int value = (int)index + index_delta[nibble & 7u];
	return value < 0 ? 0u : value > 88 ? 88u : (unsigned)value;
}

static unsigned encode_sample(int sample, int *predictor, unsigned *index)
{
	int step = step_table[*index];
	int difference = sample - *predictor;
	unsigned code = 0;
	if (difference < 0) {
		code = 8;
		difference = -difference;
	}
	int change = step >> 3;
	if (difference >= step) {
		code |= 4;
		difference -= step;
		change += step;
	}
	step >>= 1;
	if (difference >= step) {
		code |= 2;
		difference -= step;
		change += step;
	}
	step >>= 1;
	if (difference >= step) {
		code |= 1;
		change += step;
	}
	*predictor =
	    clamp_sample(*predictor + ((code & 8u) ? -change : change));
	*index = next_index(*index, code);
	return code;
}

static int decode_sample(unsigned code, int *predictor, unsigned *index)
{
	const int step = step_table[*index];
	int change = step >> 3;
	if (code & 4u)
		change += step;
	if (code & 2u)
		change += step >> 1;
	if (code & 1u)
		change += step >> 2;
	*predictor =
	    clamp_sample(*predictor + ((code & 8u) ? -change : change));
	*index = next_index(*index, code);
	return *predictor;
}

void live_audio_reset(LiveAudioState *state)
{
	if (state)
		memset(state, 0, sizeof(*state));
}

void live_audio_init(LiveAudioState *state)
{
	live_audio_reset(state);
}

size_t live_audio_encode(LiveAudioState *state, const int16_t *pcm,
			 size_t sample_frames, uint8_t *dst,
			 size_t dst_capacity)
{
	if (!state || !pcm || !dst || sample_frames != LIVE_AUDIO_SAMPLES ||
	    dst_capacity < LIVE_AUDIO_BLOCK_BYTES || state->index[0] > 88 ||
	    state->index[1] > 88)
		return 0;
	uint8_t block[LIVE_AUDIO_BLOCK_BYTES];
	int predictor[2] = {pcm[0], pcm[1]};
	unsigned index[2] = {state->index[0], state->index[1]};
	for (unsigned channel = 0; channel < 2; ++channel) {
		uint16_t sample = (uint16_t)predictor[channel];
		block[channel * 4] = (uint8_t)sample;
		block[channel * 4 + 1] = (uint8_t)(sample >> 8);
		block[channel * 4 + 2] = (uint8_t)index[channel];
		block[channel * 4 + 3] = 0;
	}
	for (unsigned frame = 1; frame < LIVE_AUDIO_SAMPLES; ++frame) {
		unsigned left =
		    encode_sample(pcm[frame * 2], &predictor[0], &index[0]);
		unsigned right =
		    encode_sample(pcm[frame * 2 + 1], &predictor[1], &index[1]);
		block[7 + frame] = (uint8_t)(left | (right << 4));
	}
	memcpy(dst, block, sizeof block);
	state->index[0] = (uint8_t)index[0];
	state->index[1] = (uint8_t)index[1];
	return LIVE_AUDIO_BLOCK_BYTES;
}

size_t live_audio_decode(const uint8_t *src, size_t src_bytes, int16_t *pcm,
			 size_t pcm_capacity_frames)
{
	if (!src || !pcm || src_bytes != LIVE_AUDIO_BLOCK_BYTES ||
	    pcm_capacity_frames < LIVE_AUDIO_SAMPLES || src[2] > 88 ||
	    src[6] > 88 || src[3] || src[7])
		return 0;
	int16_t decoded[LIVE_AUDIO_SAMPLES * LIVE_AUDIO_CHANNELS];
	int predictor[2];
	unsigned index[2] = {src[2], src[6]};
	for (unsigned channel = 0; channel < 2; ++channel) {
		int sample =
		    src[channel * 4] | ((int)src[channel * 4 + 1] << 8);
		predictor[channel] = sample >= 32768 ? sample - 65536 : sample;
		decoded[channel] = (int16_t)predictor[channel];
	}
	for (unsigned frame = 1; frame < LIVE_AUDIO_SAMPLES; ++frame) {
		unsigned code = src[7 + frame];
		decoded[frame * 2] = (int16_t)decode_sample(
		    code & 15u, &predictor[0], &index[0]);
		decoded[frame * 2 + 1] =
		    (int16_t)decode_sample(code >> 4, &predictor[1], &index[1]);
	}
	memcpy(pcm, decoded, sizeof decoded);
	return LIVE_AUDIO_SAMPLES;
}
