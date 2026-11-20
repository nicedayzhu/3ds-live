#ifndef LIVE_BUFFERS_H
#define LIVE_BUFFERS_H

#include <3ds.h>

// All mappings are page-aligned, in this system title's own normal heap range.
void *live_map_pages(u32 address, u32 bytes);
void live_free_pages(u32 address, u32 bytes);

#endif
