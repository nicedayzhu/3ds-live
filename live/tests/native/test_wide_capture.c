#include "convert.h"
#include "live_jpeg_source.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_wide(unsigned format, unsigned stride)
{
    const unsigned width = 400, rows = 240;
    const unsigned pixelBytes = format == FB_RGBA8 ? 4 : format == FB_BGR8 ? 3 : 2;
    const unsigned columnBytes = rows * pixelBytes;
    size_t packedSize = (size_t)width * columnBytes;
    size_t wideSize = (size_t)width * 2 * stride;
    unsigned char *packed = malloc(packedSize), *wide = malloc(wideSize);
    LiveJpegSource *source = live_jpeg_source_create(width);
    assert(packed && wide && source);
    memset(wide, 0x6b, wideSize);
    for (unsigned x = 0; x < width; ++x) {
        unsigned char *column = packed + x * columnBytes;
        for (unsigned i = 0; i < columnBytes; ++i) column[i] = (unsigned char)(i + 13 * x);
        memcpy(wide + x * 2 * stride, column, columnBytes);
    }
    assert(live_jpeg_source_update(source, packed, packedSize, format, columnBytes) == 375);
    assert(live_jpeg_source_update(source, wide, wideSize, format, 2 * stride) == 0);
    wide[stride + 1] ^= 0xff;                 /* unselected column */
    assert(live_jpeg_source_update(source, wide, wideSize, format, 2 * stride) == 0);
    wide[2 * stride + 1] ^= 0xff;             /* selected column, including RGBA colour */
    assert(live_jpeg_source_update(source, wide, wideSize, format, 2 * stride) == 1);
    assert(live_jpeg_source_update(source, wide, (width-1) * 2 * stride + columnBytes - 1,
                                   format, 2 * stride) == -1);
    live_jpeg_source_destroy(source);
    free(wide);
    free(packed);
}

int main(void)
{
    for (unsigned fmt = FB_RGBA8; fmt <= FB_RGBA4; ++fmt) {
        check_wide(fmt, 960);
        check_wide(fmt, 1920);
    }
    puts("Wide LCD source matches the even-column reference");
    return 0;
}
