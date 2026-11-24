#ifndef LIVE_ROW_COMPRESSION_H
#define LIVE_ROW_COMPRESSION_H

#include "live_jpeg_rows.h"
#include "lz4.h"

/* A skipped optional pass leaves the original R17D publication intact.
 * After a long scheduling interruption, probe again with the startup cost
 * instead of permanently suppressing compression on a fast new scene. */
static inline int live_r20_time_available(uint64_t now, uint64_t next, uint64_t hz,
                                         uint64_t last_attempt, uint32_t mean_us) {
    if (next <= now || !hz) return 0;
    uint32_t cost = last_attempt && now-last_attempt < hz ? mean_us : 1000u;
    return next-now >= ((cost+500ull)*hz+999999u)/1000000u;
}

/* The immutable dictionary always ends at the compact reference's EOI. */
static inline LZ4_stream_t *live_r20_dictionary(void *storage, size_t capacity,
                                                const uint8_t *base, size_t len) {
    if (!base || len <= 8 || (memcmp(base, "R17K", 4) && memcmp(base, "R8J1", 4)) ||
        base[4] != 1 || base[6] != 0x40 || base[7] != 1 ||
        base[len-2] != 0xff || base[len-1] != 0xd9) return NULL;
    size_t bytes = len-8;
    if (bytes > 32768) bytes = 32768;
    LZ4_stream_t *state = LZ4_initStream(storage, capacity);
    if (!state || LZ4_loadDict(state, (const char *)base+len-bytes, (int)bytes) != (int)bytes)
        return NULL;
    return state;
}

/* Only the worker copy advances; consecutive frames retain exactly the
 * acknowledged reference dictionary. A zero result leaves R17D available. */
static inline size_t live_r20_pack(uint8_t *out, size_t cap, const uint8_t *delta, size_t len,
                                  uint32_t base_seq, const LZ4_stream_t *dictionary,
                                  LZ4_stream_t *work) {
    const size_t header = 20;
    if (!out || !dictionary || !work || !delta || len <= header+1 || len > 128000 ||
        memcmp(delta, "R17D", 4) || delta[4] != 1 || delta[6] != 0x40 || delta[7] != 1 ||
        live_rows_read32(delta+8) != base_seq) return 0;
    if (cap >= len) cap = len-1;
    if (cap <= header) return 0;
    memcpy(work, dictionary, sizeof(*work));
    int n = LZ4_compress_fast_continue(work, (const char *)delta, (char *)out+header,
                                      (int)len, (int)(cap-header), 4);
    if (n <= 0) return 0;
    memcpy(out, "R20D", 4);
    memcpy(out+4, delta+4, 8);  /* layout, Q, bottom width, base sequence */
    live_rows_write32(out+12, (uint32_t)len);
    live_rows_write32(out+16, 0);
    return header+(size_t)n;
}

#endif
