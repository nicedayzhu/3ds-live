#include "live_audio.h"
#include <math.h>

/* Place one independent 3DS audio block on the PC's 48 kHz stereo grid.
 * The caller supplies the rounded start and frame count from the A/V clock.
 * Output bytes are little endian regardless of the host's byte order. */
int live_audio_output_48k(const uint8_t *block, size_t bytes, double pts_us,
                          int64_t start, uint8_t *output, size_t frames) {
    int16_t pcm[LIVE_AUDIO_SAMPLES * LIVE_AUDIO_CHANNELS];
    const size_t max_frames = (LIVE_AUDIO_SAMPLES * UINT64_C(48000000) +
                               LIVE_AUDIO_RATE_MHZ - 1) / LIVE_AUDIO_RATE_MHZ;
    if (!output || !frames || frames > max_frames || !isfinite(pts_us) ||
        !live_audio_decode(block, bytes, pcm, LIVE_AUDIO_SAMPLES))
        return 0;
    for (size_t frame = 0; frame < frames; ++frame) {
        double source = (((double)start + frame) * 1000000.0 / 48000.0 - pts_us)
                        * LIVE_AUDIO_RATE_MHZ / 1000000000.0;
        if (source < 0) source = 0;
        if (source > LIVE_AUDIO_SAMPLES - 1) source = LIVE_AUDIO_SAMPLES - 1;
        size_t left = (size_t)source;
        size_t right = left < LIVE_AUDIO_SAMPLES - 1 ? left + 1 : left;
        for (size_t ch = 0; ch < LIVE_AUDIO_CHANNELS; ++ch) {
            int a = pcm[left * LIVE_AUDIO_CHANNELS + ch];
            int b = pcm[right * LIVE_AUDIO_CHANNELS + ch];
            uint16_t sample = (uint16_t)lrint(a + (b - a) * (source - left));
            *output++ = (uint8_t)sample;
            *output++ = (uint8_t)(sample >> 8);
        }
    }
    return 1;
}
