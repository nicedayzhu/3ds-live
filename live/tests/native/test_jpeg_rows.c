#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "live_jpeg_rows.h"
#include "live_row_refresh.h"

enum { ROW_COUNT = 15, BUFFER_SIZE = 2048 };
static const uint32_t anchor_seq = UINT32_C(0xff01cd03);

/* Syntactically valid restart-delimited scans, with different lengths, a
 * stuffed 0xff byte in each row and the pinned encoder's RST6 + EOI tail. */
static size_t fixture(uint8_t *dst, unsigned width, unsigned changed) {
    size_t end = 8;
    memcpy(dst, "R8J1", 4);
    dst[4] = 1;
    dst[5] = 80;
    dst[6] = (uint8_t)width;
    dst[7] = (uint8_t)(width >> 8);
    for (unsigned row = 0; row < ROW_COUNT; ++row) {
        unsigned different = (changed >> row) & 1u;
        unsigned n = 31 + row + different * (row % 3 + 2);
        memset(dst + end, (int)(4 * row + different), n);
        end += n;
        dst[end++] = 0xff;
        dst[end++] = 0x00;
        dst[end++] = 0xff;
        dst[end++] = (uint8_t)(0xd0 + (row & 7));
    }
    dst[end++] = 0xff;
    dst[end++] = 0xd9;
    assert(end < BUFFER_SIZE);
    return end;
}

static void all_masks(unsigned width) {
    uint8_t base[BUFFER_SIZE], src[BUFFER_SIZE], delta[BUFFER_SIZE];
    uint8_t inplace[BUFFER_SIZE], restored[BUFFER_SIZE];
    size_t base_len = fixture(base, width, 0);
    LiveJpegRows base_rows, rows;
    memcpy(base, "R17K", 4);
    assert(live_jpeg_rows_parse(&base_rows, base, base_len));
    for (unsigned mask = 0; mask < (1u << ROW_COUNT); ++mask) {
        size_t n = fixture(src, width, mask);
        assert(live_jpeg_rows_parse(&rows, src, n));
        size_t expected = 20;
        for (unsigned i = 0; i < ROW_COUNT; ++i)
            if ((mask >> i) & 1u)
                expected += 4 + rows.offsets[i+1] - rows.offsets[i];
        size_t length = live_jpeg_rows_pack(NULL, BUFFER_SIZE, src, n, &rows,
                                             base, base_len, &base_rows, anchor_seq);
        if (expected >= n) {
            assert(length == 0);
            continue;
        }
        assert(length == expected);
        memset(delta, 0xa5, sizeof delta);
        assert(live_jpeg_rows_pack(delta, length - 1, src, n, &rows,
                                   base, base_len, &base_rows, anchor_seq) == 0);
        assert(live_jpeg_rows_pack(delta, length, src, n, &rows,
                                   base, base_len, &base_rows, anchor_seq) == length);
        assert(delta[length] == 0xa5 && memcmp(delta, "R17D", 4) == 0);
        assert(delta[12] == (uint8_t)mask && delta[13] == (uint8_t)(mask >> 8));
        memcpy(inplace, src, n);
        inplace[n] = 0xa5;
        assert(live_jpeg_rows_pack(inplace, length, inplace, n, &rows,
                                   base, base_len, &base_rows, anchor_seq) == length);
        assert(memcmp(inplace, delta, length) == 0 && inplace[n] == 0xa5);
        restored[n] = 0x5a;
        assert(live_jpeg_rows_restore(restored, n - 1, delta, length, base,
                                      base_len, &base_rows, anchor_seq) == 0);
        assert(live_jpeg_rows_restore(restored, n, delta, length, base,
                                      base_len, &base_rows, anchor_seq) == n);
        assert(memcmp(restored, src, n) == 0 && restored[n] == 0x5a);
    }
}

static void invalid_inputs(void) {
    uint8_t base[BUFFER_SIZE], src[BUFFER_SIZE], delta[BUFFER_SIZE];
    uint8_t corrupt[BUFFER_SIZE], restored[BUFFER_SIZE];
    LiveJpegRows base_rows, rows, discarded;
    size_t base_len = fixture(base, 400, 0);
    size_t n = fixture(src, 400, 1u | (1u << 14));
    assert(live_jpeg_rows_parse(&base_rows, base, base_len));
    assert(live_jpeg_rows_parse(&rows, src, n));
    for (size_t i = 0; i < n; ++i)
        assert(!live_jpeg_rows_parse(&discarded, src, i));
    memcpy(corrupt, src, n);
    corrupt[rows.offsets[8] - 1] = 0xd1;
    assert(!live_jpeg_rows_parse(&discarded, corrupt, n));
    corrupt[rows.offsets[8] - 1] = 0xd7;
    corrupt[n - 1] = 0;
    assert(!live_jpeg_rows_parse(&discarded, corrupt, n));
    memcpy(corrupt, src, n);
    memcpy(corrupt, "R17D", 4);
    assert(!live_jpeg_rows_parse(&discarded, corrupt, n));
    memcpy(corrupt, src, n);
    corrupt[4] = 0;
    assert(!live_jpeg_rows_parse(&discarded, corrupt, n));
    corrupt[4] = 1;
    corrupt[5] = 0;
    assert(!live_jpeg_rows_parse(&discarded, corrupt, n));
    corrupt[5] = 96;
    assert(!live_jpeg_rows_parse(&discarded, corrupt, n));
    corrupt[5] = 80;
    corrupt[6] = 0;
    assert(!live_jpeg_rows_parse(&discarded, corrupt, n));
    memcpy(corrupt, src, n);
    corrupt[8] = 0xff;
    corrupt[9] = 0xd0; /* An empty first scan row. */
    assert(!live_jpeg_rows_parse(&discarded, corrupt, n));
    corrupt[9] = 0xd9;
    assert(!live_jpeg_rows_parse(&discarded, corrupt, n));
    size_t len = live_jpeg_rows_pack(delta, sizeof delta, src, n, &rows,
                                      base, base_len, &base_rows, anchor_seq);
    assert(len > 20);
    for (size_t i = 0; i < len; ++i)
        assert(!live_jpeg_rows_restore(restored, sizeof restored, delta, i,
                                       base, base_len, &base_rows, anchor_seq));
    assert(!live_jpeg_rows_restore(restored, sizeof restored, delta, len + 1,
                                   base, base_len, &base_rows, anchor_seq));
    assert(!live_jpeg_rows_restore(restored, sizeof restored, delta, len,
                                   base, base_len, &base_rows, anchor_seq + 1u));
    /* Header fields, high mask bit, original length and row length. */
    const unsigned bad_offsets[] = {0, 4, 5, 6, 13, 14, 16, 20};
    for (size_t i = 0; i < sizeof bad_offsets / sizeof *bad_offsets; ++i) {
        memcpy(corrupt, delta, len);
        corrupt[bad_offsets[i]] ^= 0x80;
        assert(!live_jpeg_rows_restore(restored, sizeof restored, corrupt, len,
                                       base, base_len, &base_rows, anchor_seq));
    }
    memcpy(corrupt, delta, len);
    corrupt[len - 1] = 0;
    assert(!live_jpeg_rows_restore(restored, sizeof restored, corrupt, len,
                                   base, base_len, &base_rows, anchor_seq));
    memcpy(corrupt, delta, len);
    memset(corrupt + 20, 0xff, 4);
    assert(!live_jpeg_rows_restore(restored, sizeof restored, corrupt, len,
                                   base, base_len, &base_rows, anchor_seq));
    memset(corrupt + 20, 0, 4);
    assert(!live_jpeg_rows_restore(restored, sizeof restored, corrupt, len,
                                   base, base_len, &base_rows, anchor_seq));
    LiveJpegRows stale = base_rows;
    stale.offsets[10] = UINT32_MAX;
    assert(!live_jpeg_rows_restore(restored, sizeof restored, delta, len,
                                   base, base_len, &stale, anchor_seq));
    assert(!live_jpeg_rows_pack(NULL, 0, src, n, &rows, base, base_len,
                                &stale, anchor_seq));
    src[5] = 60;
    assert(live_jpeg_rows_parse(&rows, src, n));
    assert(!live_jpeg_rows_pack(delta, sizeof delta, src, n, &rows,
                                base, base_len, &base_rows, anchor_seq));
    base[5] = 60;
    assert(live_jpeg_rows_parse(&base_rows, base, base_len));
    assert(!live_jpeg_rows_restore(restored, sizeof restored, delta, len,
                                   base, base_len, &base_rows, anchor_seq));
}

static void maximum_size(void) {
    enum { MAX = LIVE_JPEG_WIRE_MAX_PAYLOAD };
    static uint8_t src[MAX + 1], base[MAX], delta[MAX], restored[MAX];
    uint8_t small[BUFFER_SIZE];
    size_t small_len = fixture(small, 400, 0);
    size_t extra = MAX - small_len;
    /* One large legal entropy segment forces a changed row length > 65535.
     * Remaining rows must still fit after the larger first row is moved. */
    memcpy(src, small, 8);
    memset(src + 8, 0x33, extra);
    memcpy(src + 8 + extra, small + 8, small_len - 8);
    memcpy(base, src, MAX);
    base[8] ^= 0x10;
    LiveJpegRows rows, base_rows;
    assert(live_jpeg_rows_parse(&rows, src, MAX));
    assert(live_jpeg_rows_parse(&base_rows, base, MAX));
    assert(!live_jpeg_rows_parse(&rows, src, MAX + 1));
    size_t n = live_jpeg_rows_pack(delta, MAX, src, MAX, &rows, base,
                                   MAX, &base_rows, anchor_seq);
    assert(n > UINT16_MAX && n < MAX);
    assert(live_jpeg_rows_restore(restored, MAX, delta, n, base, MAX,
                                  &base_rows, anchor_seq) == MAX);
    assert(memcmp(restored, src, MAX) == 0);
    assert(live_jpeg_rows_pack(src, n, src, MAX, &rows, base, MAX,
                               &base_rows, anchor_seq) == n);
    assert(memcmp(src, delta, n) == 0);
}

static void reference_refresh(void) {
    const unsigned full=42000;
    /* No probe is sent over the network. Continuous motion matches neither
     * base. A cut to a new menu matches the probe after four captures. */
    LiveRowTrial motion={0},menu={0},local={0},flicker={0},small={0};
    for(unsigned i=0;i<4;i++) {
        assert(live_row_trial_observe(&motion,39500,39500,full)==(i==3?-1:0));
        assert(live_row_trial_observe(&menu,full,5050,full)==(i==3?1:0));
        /* The old base still saves nearly half. A recently changed panel
         * with static surroundings makes another reference worthwhile. */
        assert(live_row_trial_observe(&local,23000,8200,full)==(i==3?1:0));
        assert(live_row_trial_observe(&flicker,full,i==0?5000:full,full)==(i==3?-1:0));
        /* Savings this small cannot repay up to two full key repeats. */
        assert(live_row_trial_observe(&small,8000,6000,full)==(i==3?-1:0));
    }
}

int main(void) {
    all_masks(400);
    all_masks(320);
    invalid_inputs();
    maximum_size();
    reference_refresh();
    puts("JPEG row deltas: both screen widths and all 32768 masks round-trip");
    return 0;
}
