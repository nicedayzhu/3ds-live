#include "live_receiver_feedback.h"
#include <assert.h>

int main(void) {
    const uint64_t timeout = 4000;
    LiveReceiverFeedback feedback = {0};
    assert(!live_receiver_recent(&feedback, 0, timeout));

    /* A nonzero count can be older than this connection. */
    live_receiver_observe(&feedback, 20000, 90, 1000, timeout);
    assert(!live_receiver_recent(&feedback, 1000, timeout));
    live_receiver_observe(&feedback, 21000, 92, 2000, timeout);
    assert(live_receiver_recent(&feedback, 2000, timeout));
    live_receiver_observe(&feedback, 22000, 92, 3000, timeout);
    live_receiver_observe(&feedback, 21900, 150, 4000, timeout);
    live_receiver_observe(&feedback, 22000, 150, 5000, timeout);
    assert(live_receiver_recent(&feedback, 5999, timeout));
    assert(!live_receiver_recent(&feedback, 6000, timeout));
    live_receiver_observe(&feedback, 24000, 94, 6000, timeout);
    assert(live_receiver_recent(&feedback, 6000, timeout));

    /* A receiver restart can reset counts even while its PC clock moves forward. */
    live_receiver_observe(&feedback, 25000, 20, 7000, timeout);
    assert(!live_receiver_recent(&feedback, 7000, timeout));
    live_receiver_observe(&feedback, 26000, 22, 8000, timeout);
    assert(live_receiver_recent(&feedback, 8000, timeout));

    /* Also recover from clock resets across an outage, ignoring the first
     * delayed or restarted sample until another one confirms fresh frames. */
    live_receiver_observe(&feedback, 1, 50, 12000, timeout);
    assert(!live_receiver_recent(&feedback, 12000, timeout));
    live_receiver_observe(&feedback, 1001, 52, 13000, timeout);
    assert(live_receiver_recent(&feedback, 13000, timeout));

    /* Session reset and normal 32-bit wrap of both PC fields. */
    feedback = (LiveReceiverFeedback){0};
    assert(!live_receiver_recent(&feedback, 13000, timeout));
    live_receiver_observe(&feedback, UINT32_MAX - 2, UINT32_MAX - 1, 14000, timeout);
    live_receiver_observe(&feedback, 997, 2, 15000, timeout);
    assert(live_receiver_recent(&feedback, 15000, timeout));
    return 0;
}
