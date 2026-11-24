#ifndef LIVE_JPEG_ROWS_H
#define LIVE_JPEG_ROWS_H
#include "live_jpeg_wire.h"

#define LIVE_JPEG_ROWS_COUNT 15u
#define LIVE_JPEG_DELTA_HEADER 20u

/* Offsets refer to the compact JPEG including its 8-byte header. Each
 * independent scan row ends with its RST marker. EOI follows the last row. */
typedef struct { uint32_t offsets[LIVE_JPEG_ROWS_COUNT + 1u]; } LiveJpegRows;

static inline uint32_t live_rows_read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void live_rows_write32(uint8_t *p, uint32_t n) {
    p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); p[2]=(uint8_t)(n>>16); p[3]=(uint8_t)(n>>24);
}

static inline int live_rows_format_valid(const uint8_t *src) {
    unsigned width = src[6] | ((unsigned)src[7] << 8);
    return src[4] == LIVE_JPEG_WIRE_LAYOUT && src[5] >= 1u && src[5] <= 95u &&
           (width == 320u || width == 400u);
}

/* Returns the first byte after this row, rejecting premature EOI or any
 * marker other than the expected RST. FF00 is a stuffed entropy byte. */
static inline const uint8_t *live_rows_scan_end(const uint8_t *p, const uint8_t *end, unsigned row) {
    const uint8_t *start = p;
    while (p < end) {
        p = (const uint8_t *)memchr(p, 0xff, (size_t)(end-p));
        if (!p || end-p < 2) return NULL;
        if (p[1]) return p > start && p[1] == 0xd0u + (row & 7u) ? p+2 : NULL;
        p += 2;
    }
    return NULL;
}

static inline int live_jpeg_rows_parse(LiveJpegRows *rows, const uint8_t *src, size_t len) {
    if (!rows || !src || len < 10u || len > LIVE_JPEG_WIRE_MAX_PAYLOAD ||
        (memcmp(src,"R8J1",4) && memcmp(src,"R17K",4)) || !live_rows_format_valid(src) ||
        src[len-2] != 0xff || src[len-1] != 0xd9) return 0;
    const uint8_t *p = src+LIVE_JPEG_WIRE_HEADER_SIZE, *end = src+len-2;
    rows->offsets[0] = LIVE_JPEG_WIRE_HEADER_SIZE;
    for (unsigned row=0; row<LIVE_JPEG_ROWS_COUNT; ++row) {
        p = live_rows_scan_end(p,end,row);
        if (!p) return 0;
        rows->offsets[row+1] = (uint32_t)(p-src);
    }
    return p == end;
}

/* Parsed indexes belong to their immutable image. Bounds-check on use to
 * protect callers from stale indexes when an encoder buffer is recycled. */
static inline int live_rows_span_valid(const LiveJpegRows *rows, size_t len) {
    if (!rows || len < 10u || len > LIVE_JPEG_WIRE_MAX_PAYLOAD ||
        rows->offsets[0] != LIVE_JPEG_WIRE_HEADER_SIZE ||
        rows->offsets[LIVE_JPEG_ROWS_COUNT] != len-2u) return 0;
    for (unsigned i=0; i<LIVE_JPEG_ROWS_COUNT; ++i)
        if (rows->offsets[i] >= rows->offsets[i+1]) return 0;
    return 1;
}

/* Zero means send the independent compact JPEG. A NULL destination measures
 * the encoding without writing. A sender can overwrite src in place. */
static inline size_t live_jpeg_rows_pack(uint8_t *out, size_t cap,
        const uint8_t *src, size_t len, const LiveJpegRows *rows,
        const uint8_t *base, size_t base_len, const LiveJpegRows *base_rows, uint32_t base_seq) {
    if (!src || !base || !live_rows_span_valid(rows,len) ||
        !live_rows_span_valid(base_rows,base_len) || memcmp(src+4,base+4,4)) return 0;
    uint32_t lengths[LIVE_JPEG_ROWS_COUNT], mask=0;
    size_t total=LIVE_JPEG_DELTA_HEADER;
    for (unsigned i=0; i<LIVE_JPEG_ROWS_COUNT; ++i) {
        uint32_t n=rows->offsets[i+1]-rows->offsets[i];
        if (n == base_rows->offsets[i+1]-base_rows->offsets[i] &&
            !memcmp(src+rows->offsets[i],base+base_rows->offsets[i],n)) continue;
        mask |= 1u << i;
        lengths[i] = n;
        total += 4u+n;
    }
    if (total >= len || (out && cap < total)) return 0;
    if (!out) return total;
    uint8_t format[4];
    memcpy(format,src+4,4);
    if (out == src) {
        /* A run of changed rows at the front may grow before a later omitted
         * row creates space. First compact left, then insert lengths backwards. */
        size_t raw=0, end=total;
        for (unsigned i=0; i<LIVE_JPEG_ROWS_COUNT; ++i) if (mask & (1u<<i)) {
            memmove(out+raw,src+rows->offsets[i],lengths[i]);
            raw += lengths[i];
        }
        for (unsigned k=LIVE_JPEG_ROWS_COUNT; k>0; --k) if (mask & (1u<<(k-1))) {
            uint32_t n=lengths[k-1];
            raw-=n; end-=n;
            memmove(out+end,out+raw,n);
            end-=4;
            live_rows_write32(out+end,n);
        }
    } else {
        size_t pos=LIVE_JPEG_DELTA_HEADER;
        for (unsigned i=0; i<LIVE_JPEG_ROWS_COUNT; ++i) if (mask & (1u<<i)) {
            live_rows_write32(out+pos,lengths[i]);
            memcpy(out+pos+4,src+rows->offsets[i],lengths[i]);
            pos+=4u+lengths[i];
        }
    }
    memcpy(out,"R17D",4);
    memcpy(out+4,format,4);
    live_rows_write32(out+8,base_seq);
    live_rows_write32(out+12,mask); /* Upper 17 bits reserved. */
    live_rows_write32(out+16,(uint32_t)len);
    return total;
}

/* The base has already passed live_jpeg_rows_parse. out must be independent
 * of both inputs. Only a nonzero result may be published by the receiver. */
static inline size_t live_jpeg_rows_restore(uint8_t *out, size_t cap,
        const uint8_t *delta, size_t delta_len, const uint8_t *base, size_t base_len,
        const LiveJpegRows *base_rows, uint32_t expected_base_seq) {
    if (!out || !delta || !base || delta_len < LIVE_JPEG_DELTA_HEADER ||
        !live_rows_span_valid(base_rows,base_len) || memcmp(delta,"R17D",4) ||
        memcmp(delta+4,base+4,4) || live_rows_read32(delta+8) != expected_base_seq) return 0;
    uint32_t mask=live_rows_read32(delta+12), original=live_rows_read32(delta+16);
    if (mask >= (1u << LIVE_JPEG_ROWS_COUNT) || original < 10u || original > cap ||
        original > LIVE_JPEG_WIRE_MAX_PAYLOAD || delta_len >= original) return 0;
    size_t pos=LIVE_JPEG_DELTA_HEADER, written=LIVE_JPEG_WIRE_HEADER_SIZE;
    memcpy(out,"R8J1",4);
    memcpy(out+4,base+4,4);
    for (unsigned i=0; i<LIVE_JPEG_ROWS_COUNT; ++i) {
        const uint8_t *row=base+base_rows->offsets[i];
        uint32_t n=base_rows->offsets[i+1]-base_rows->offsets[i];
        if (mask & (1u<<i)) {
            if (delta_len-pos < 4u) return 0;
            n=live_rows_read32(delta+pos);
            pos+=4u;
            if (n > delta_len-pos) return 0;
            row=delta+pos;
            if (live_rows_scan_end(row,row+n,i) != row+n) return 0;
            pos+=n;
        }
        if (n > original-2u-written) return 0;
        memcpy(out+written,row,n);
        written+=n;
    }
    if (pos != delta_len || written != original-2u) return 0;
    out[written]=0xff; out[written+1]=0xd9;
    return original;
}
#endif
