#ifndef LIVE_FPS_H
#define LIVE_FPS_H
#include <stdint.h>

#define LIVE_FPS_MASK ((1u << 15) | (1u << 20) | (1u << 24) | (1u << 30))
static inline uint32_t live_fps_flag(uint32_t fps)
{
	switch (fps) {
	case 15:
	case 20:
	case 24:
	case 30:
		return 1u << fps;
	default:
		return 0;
	}
}
static inline int live_valid_fps(uint32_t fps)
{
	return live_fps_flag(fps) != 0;
}
/* Version 0.1.11 used the two low bits for its experimental capture rates.
 * Only stored settings and incoming media need to recognize these rates. */
#define LIVE_LEGACY_FPS_EXTRA 3u
static inline uint32_t live_media_fps_flag(uint32_t fps)
{
	return fps == 35 ? 1u : fps == 40 ? 2u : live_fps_flag(fps);
}
static inline uint32_t live_migrate_fps(uint32_t fps)
{
	return fps == 35 || fps == 40 ? 30 : fps;
}
#endif
