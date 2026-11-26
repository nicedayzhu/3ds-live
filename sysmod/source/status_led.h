#ifndef STATUS_LED_H
#define STATUS_LED_H
#include <stdbool.h>
#include <stdint.h>

typedef enum {
	LED_STATE_OFF,
	LED_STATE_READY,
	LED_STATE_WAITING,
	LED_STATE_STREAMING,
	LED_STATE_ERROR
} LedState;

/* The launcher can exit while the background module remains ready or live. */
static inline LedState status_led_state(bool awake, bool running,
					bool receiving, LedState idleState)
{
	if (!awake)
		return LED_STATE_OFF;
	if (running)
		return receiving ? LED_STATE_STREAMING : LED_STATE_WAITING;
	return idleState;
}

int32_t status_led_apply(LedState state);

#endif
