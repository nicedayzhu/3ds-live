#ifndef STATUS_LED_PATTERN_H
#define STATUS_LED_PATTERN_H
#include "status_led.h"
#include <stdint.h>
#include <string.h>

/* PTMSYSM:SetInfoLEDPattern accepts the MCU animation header and 32 samples
 * for each channel. One sample lasts (ticks_per_sample + 1) / 512 seconds. */
typedef struct {
	uint8_t ticks_per_sample, transition_ticks, last_sample_repeats,
	    reserved;
	uint8_t red[32], green[32], blue[32];
} StatusLedPattern;

_Static_assert(sizeof(StatusLedPattern) == 100,
	       "Unexpected notification LED pattern size");

static inline StatusLedPattern status_led_pattern(LedState state)
{
	StatusLedPattern pattern = {.ticks_per_sample = 15};
	switch (state) {
	case LED_STATE_OFF:
		break;
	case LED_STATE_READY:
		memset(pattern.blue, 48, sizeof(pattern.blue));
		break;
	case LED_STATE_WAITING:
		/* A two-second amber pulse generated entirely by the MCU. */
		pattern.ticks_per_sample = 31;
		pattern.transition_ticks = 31;
		for (unsigned i = 0; i < 32; ++i) {
			unsigned level = i < 16 ? i : 31 - i;
			pattern.red[i] = (uint8_t)(6 * level);
			pattern.green[i] = (uint8_t)(2 * level);
		}
		break;
	case LED_STATE_STREAMING:
		memset(pattern.green, 56, sizeof(pattern.green));
		break;
	case LED_STATE_ERROR:
		/* Two 188 ms red flashes, then hold the last (black) sample. */
		memset(pattern.red, 96, 6);
		memset(pattern.red + 12, 96, 6);
		pattern.last_sample_repeats = 0xff;
		break;
	}
	return pattern;
}
#endif
