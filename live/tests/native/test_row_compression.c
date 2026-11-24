#include "row_compression.h"
#include <assert.h>
#include <stdio.h>

enum { CAP = 64000 };
static uint8_t base[CAP], src[CAP], delta[CAP], packed[CAP], inner[CAP], restored[CAP];
static LZ4_stream_t dictionary, work;

static size_t fixture(unsigned bytes) {
    memcpy(base, "R17K\x01\x50\x40\x01", 8);
    size_t end = 8;
    uint32_t state = 777;
    for (unsigned r = 0; r < 15; ++r) {
        for (unsigned j = 0; j < bytes; ++j) {
            state = state*1664525u + 1013904223u;
            base[end++] = (uint8_t)((state >> 16) % 255u);
        }
        base[end++] = 0xff;
        base[end++] = (uint8_t)(0xd0 + (r & 7u));
    }
    base[end++] = 0xff;
    base[end++] = 0xd9;
    return end;
}

static void roundtrip(unsigned bytes) {
    size_t size = fixture(bytes);
    LiveJpegRows ix;
    assert(live_jpeg_rows_parse(&ix, base, size));
    assert(live_r20_dictionary(&dictionary, sizeof(dictionary), base, size) == &dictionary);
    for (unsigned r = 12; r < 15; ++r) {
        memcpy(src, base, size);
        memcpy(src, "R8J1", 4);
        src[ix.offsets[r]+123] ^= 1;
        if (src[ix.offsets[r]+123] == 0xff) src[ix.offsets[r]+123] = 1;
        size_t len = live_jpeg_rows_pack(delta, CAP, src, size, &ix, base, size, &ix, 1234);
        assert(len > 200);
        size_t n = live_r20_pack(packed, CAP, delta, len, 1234, &dictionary, &work);
        assert(n >= 21 && n < len/3 && !memcmp(packed,"R20D",4));
        assert(!memcmp(packed+4,delta+4,8) && live_rows_read32(packed+12)==len &&
               !live_rows_read32(packed+16));
        size_t window = size-8 < 32768 ? size-8 : 32768;
        int decoded = LZ4_decompress_safe_usingDict((const char *)packed+20,(char *)inner,
            (int)n-20,(int)len,(const char *)base+size-window,(int)window);
        assert(decoded == (int)len && !memcmp(inner,delta,len));
        assert(live_jpeg_rows_restore(restored,CAP,inner,len,base,size,&ix,1234)==size);
        assert(!memcmp(restored,src,size));
        assert(!live_r20_pack(packed,20,delta,len,1234,&dictionary,&work));
        assert(!live_r20_pack(packed,CAP,delta,len,1235,&dictionary,&work));
        delta[6] = 0x90;
        assert(!live_r20_pack(packed,CAP,delta,len,1234,&dictionary,&work));
        assert(!live_r20_pack(packed,CAP,src,size,1234,&dictionary,&work));
    }
    memcpy(base,"R8J1",4);
    assert(live_r20_dictionary(&dictionary,sizeof(dictionary),base,size));
    assert(!live_r20_dictionary(&dictionary,sizeof(dictionary)-1,base,size));
    base[6] = 0x90;
    assert(!live_r20_dictionary(&dictionary,sizeof(dictionary),base,size));
}

int main(void) {
    assert(!live_r20_time_available(10000,9999,1000000,0,0));
    assert(!live_r20_time_available(10000,11499,1000000,0,0));
    assert(live_r20_time_available(10000,11500,1000000,0,0));
    assert(!live_r20_time_available(10000,13000,1000000,9000,3000));
    assert(live_r20_time_available(10000,13500,1000000,9000,3000));
    assert(live_r20_time_available(1010000,1011500,1000000,9000,3000));
    roundtrip(512);
    roundtrip(4000);
    puts("R20 bottom JPEG dictionary round trips passed");
    return 0;
}
