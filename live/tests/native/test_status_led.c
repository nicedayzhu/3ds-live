#include "status_led_pattern.h"
#include <assert.h>
#include <stddef.h>

int main(void)
{
    assert(status_led_state(true, false, false, LED_STATE_READY) == LED_STATE_READY);
    assert(status_led_state(true, false, true, LED_STATE_OFF) == LED_STATE_OFF);
    assert(status_led_state(true, false, false, LED_STATE_ERROR) == LED_STATE_ERROR);
    assert(status_led_state(true, true, false, LED_STATE_OFF) == LED_STATE_WAITING);
    assert(status_led_state(true, true, true, LED_STATE_OFF) == LED_STATE_STREAMING);
    assert(status_led_state(false, true, true, LED_STATE_READY) == LED_STATE_OFF);
    assert(status_led_state(false, false, false, LED_STATE_READY) == LED_STATE_OFF);
    assert(offsetof(StatusLedPattern, red) == 4);
    assert(offsetof(StatusLedPattern, green) == 36);
    assert(offsetof(StatusLedPattern, blue) == 68);
    const StatusLedPattern off = status_led_pattern(LED_STATE_OFF);
    const StatusLedPattern ready = status_led_pattern(LED_STATE_READY);
    const StatusLedPattern wait = status_led_pattern(LED_STATE_WAITING);
    const StatusLedPattern live = status_led_pattern(LED_STATE_STREAMING);
    const StatusLedPattern error = status_led_pattern(LED_STATE_ERROR);
    assert(off.ticks_per_sample && ready.ticks_per_sample && live.ticks_per_sample);
    assert(wait.ticks_per_sample == 31 && wait.last_sample_repeats == 0);
    assert(wait.transition_ticks == wait.ticks_per_sample);
    assert(error.ticks_per_sample == 15 && error.last_sample_repeats == 0xff);
    for (unsigned i = 0; i < 32; ++i) {
        assert(!off.red[i] && !off.green[i] && !off.blue[i]);
        assert(!ready.red[i] && !ready.green[i] && ready.blue[i] == ready.blue[0] && ready.blue[i]);
        assert(!live.red[i] && live.green[i] == live.green[0] && live.green[i] && !live.blue[i]);
        assert(wait.red[i] == wait.red[31 - i] && wait.green[i] == wait.green[31 - i]);
        assert(!wait.blue[i] && wait.red[i] == 3 * wait.green[i]);
        assert((error.red[i] != 0) == (i < 6 || (i >= 12 && i < 18)));
        assert(!error.green[i] && !error.blue[i]);
    }
    assert(!wait.red[0] && wait.red[15] > wait.red[8] && wait.red[8] > wait.red[1]);
    return 0;
}
