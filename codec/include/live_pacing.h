#ifndef LIVE_PACING_H
#define LIVE_PACING_H
#include <stdint.h>
/* A capture period is not a transmit deadline. Game scheduling and SOC IPC
 * can stretch a pair past 33 ms even when every datagram succeeds. Keep only
 * the latest pending pair, but allow an in-flight pair 150 ms to finish.
 * The PC reassembly timeout must exceed this (currently 200 ms). */
#define LIVE_SEND_TIMEOUT_MS 150u

/* Encoder-owned. A full one-pair mailbox at publication means a complete
 * encoded pair was replaced before the socket worker could pick it up. */
typedef struct {
    unsigned percent, samples, replacements, clean_windows;
} LivePayloadControl;

static inline void live_payload_observe(LivePayloadControl *control, unsigned fps, int replaced) {
    control->replacements += replaced != 0;
    if (++control->samples < fps) return;
    if (control->replacements >= 2) {
        control->percent = control->percent > 70 ? control->percent - 10 : 60;
        control->clean_windows = 0;
    } else if (control->replacements) {
        control->clean_windows = 0;
    } else if (++control->clean_windows == 4) {
        control->percent = control->percent < 98 ? control->percent + 2 : 100;
        control->clean_windows = 0;
    }
    control->samples = control->replacements = 0;
}
static inline uint64_t live_pair_deadline(uint64_t start,uint64_t ticks_per_second) {
    return start+ticks_per_second*LIVE_SEND_TIMEOUT_MS/1000;
}
/* Charge from submission start. Time spent inside sendto is already part of
 * this interval, not an additional sleep. Late wakes never accrue burst credit.
 * Caller validates rate>0; wire_bytes includes the IPv4 and UDP headers. */
static inline uint64_t live_packet_due(uint64_t previous_due,uint64_t start,
                                       uint32_t wire_bytes,uint32_t rate,uint64_t ticks_per_second) {
    uint64_t base=start>previous_due?start:previous_due;
    return base+((uint64_t)wire_bytes*ticks_per_second+rate-1)/rate;
}
#endif
