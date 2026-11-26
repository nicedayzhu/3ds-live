#include "launcher_ui.h"
#include "launcher_text.h"
#include "launcher_logo.h"
#include <3ds.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* libctru presents BGR8 framebuffers as portrait columns, bottom pixel first.
 */
typedef struct {
	u8 *pixels;
	int width;
} Screen;
typedef enum { SMALL, BODY, TITLE, HEADING } Face;
typedef struct {
	u32 offset, count;
} FontFace;
typedef struct {
	u16 code;
	u8 advance, w, h;
	s8 left, top;
	u8 pad;
	u32 offset;
} Glyph;
extern const u8 launcher_font[];
_Static_assert(sizeof(Glyph) == 12, "Font atlas record layout");

static void rect(Screen s, int x, int y, int w, int h, u32 color)
{
	const int right = x + w < s.width ? x + w : s.width;
	const int bottom = y + h < 240 ? y + h : 240;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (bottom <= y)
		return;
	const u8 b = (u8)color, g = (u8)(color >> 8), r = (u8)(color >> 16);
	for (int col = x; col < right; ++col) {
		u8 *p = s.pixels + 3 * (240 * col + 240 - bottom);
		for (int row = y; row < bottom; ++row) {
			*p++ = b;
			*p++ = g;
			*p++ = r;
		}
	}
}

static void dot(Screen s, int x, int y, int radius, u32 color)
{
	for (int dy = -radius; dy <= radius; ++dy)
		for (int dx = -radius; dx <= radius; ++dx)
			if (dx * dx + dy * dy <= radius * radius)
				rect(s, x + dx, y + dy, 1, 1, color);
}

static void outline(Screen s, int x, int y, int w, int h, u32 color)
{
	rect(s, x, y, w, 1, color);
	rect(s, x, y + h - 1, w, 1, color);
	rect(s, x, y + 1, 1, h - 2, color);
	rect(s, x + w - 1, y + 1, 1, h - 2, color);
}

/* All strings in this UI use ASCII and BMP characters. */
static u16 next_code(const char **text)
{
	const u8 *p = (const u8 *)*text;
	u16 c = *p++;
	if (c >= 0xe0) {
		c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63);
		p += 2;
	} else if (c >= 0xc0) {
		c = ((c & 31) << 6) | (*p++ & 63);
	}
	*text = (const char *)p;
	return c;
}

static const Glyph *lookup(Face face, u16 code)
{
	const FontFace *f = (const FontFace *)launcher_font + face;
	const Glyph *table = (const Glyph *)(launcher_font + f->offset);
	u32 lo = 0, hi = f->count;
	while (lo < hi) {
		const u32 mid = lo + (hi - lo) / 2;
		if (table[mid].code < code)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < f->count && table[lo].code == code ? &table[lo] : NULL;
}

static int text_width(Face face, const char *text)
{
	int width = 0;
	while (*text) {
		const Glyph *g = lookup(face, next_code(&text));
		if (g)
			width += g->advance;
	}
	return width;
}

static void label(Screen s, int x, int baseline, Face face, u32 color,
		  const char *text)
{
	while (*text) {
		const Glyph *g = lookup(face, next_code(&text));
		if (!g)
			continue;
		const u8 *mask = launcher_font + g->offset;
		for (int row = 0; row < g->h; ++row) {
			int y = baseline + g->top + row;
			if (y < 0 || y >= 240)
				continue;
			for (int col = 0; col < g->w; ++col) {
				int xx = x + g->left + col;
				if (xx < 0 || xx >= s.width)
					continue;
				int i = row * g->w + col;
				unsigned a =
				    (mask[i / 2] >> (4 * (i % 2))) & 15;
				if (!a)
					continue;
				u8 *dest = s.pixels + 3 * (240 * xx + 239 - y);
				for (int channel = 0; channel < 3; ++channel) {
					unsigned src =
					    (color >> (8 * channel)) & 255;
					dest[channel] =
					    (u8)((src * a +
						  dest[channel] * (15 - a) +
						  7) /
						 15);
				}
			}
		}
		x += g->advance;
	}
}

enum {
	BLUE = 0x86BDFF,
	GREEN = 0x78DDAD,
	AMBER = 0xF3C178,
	RED = 0xFFA996,
	MUTED = 0xA9BFBC,
	PAPER = 0xEFF3ED,
	INK = 0x1B3533,
	SECONDARY = 0x425E59
};

static u32 state_accent(const LauncherView *v)
{
	switch (v->page) {
	case UI_LIVE:
		switch (v->module.state) {
		case LAUNCHER_READY:
			return BLUE;
		case LAUNCHER_SEARCHING:
		case LAUNCHER_PAIRED:
			return AMBER;
		case LAUNCHER_RECEIVING:
			return GREEN;
		case LAUNCHER_ERROR:
			return RED;
		default:
			return MUTED;
		}
	case UI_STARTING:
		return MUTED;
	case UI_WAIT_STATUS:
	case UI_CONFLICT:
	case UI_OLD_BUSY:
	case UI_REBOOT:
		return AMBER;
	default:
		return RED;
	}
}

static void battery(Screen s, const LauncherDevice *d, const LauncherText *text)
{
	const char *caption = d->charging == 1 ? text->charging
			      : d->battery < 0 ? text->battery_unknown
					       : text->battery;
	label(s, 339 - text_width(SMALL, caption), 32, SMALL, MUTED, caption);
	outline(s, 347, 20, 28, 13, MUTED);
	rect(s, 375, 24, 2, 5, MUTED);
	if (d->battery < 0) {
		rect(s, 357, 26, 7, 1, MUTED);
		return;
	}
	for (int i = 0; i < d->battery && i < 5; ++i)
		rect(s, 350 + 5 * i, 23, 3, 7,
		     d->battery <= 1 && d->charging != 1 ? AMBER : GREEN);
}

static void top_screen(Screen s, const LauncherView *v,
		       const LauncherText *text)
{
	const LauncherStateText *state = v->page == UI_LIVE
					     ? &text->live[v->module.state]
					     : &text->startup[v->page];
	const u32 accent = state_accent(v);
	rect(s, 0, 0, s.width, 240, LAUNCHER_HEADER_COLOR);
	const int logo_x = 21, logo_y = 11;
	for (int column = 0; column < LAUNCHER_LOGO_SIZE; ++column) {
		u8 *dest = s.pixels + 3 * ((logo_x + column) * 240 + 240 -
					   logo_y - LAUNCHER_LOGO_SIZE);
		memcpy(dest, launcher_logo[column],
		       sizeof launcher_logo[column]);
	}
	label(s, logo_x + LAUNCHER_LOGO_SIZE + 9, 35, HEADING, 0xF2F6EF,
	      "3DS Live");
	battery(s, &v->device, text);
	rect(s, 22, 52, 356, 1, 0x38504F);
	dot(s, 26, 75, 4, accent);
	label(s, 39, 80, SMALL, accent, text->status);
	label(s, 21, 127, TITLE, 0xF6F7EF, state->title);
	label(s, 23, 157, BODY, 0xC9D9D4, state->description);

	if (!state->key)
		return;
	rect(s, 0, 183, s.width, 57, 0x203A3B);
	const int key_width = 18 + text_width(SMALL, state->key);
	rect(s, 22, 197, key_width, 27, 0x314D4D);
	label(s, 31, 216, SMALL, 0xE0EBE4, state->key);
	label(s, 34 + key_width, 217, BODY, 0xEFF5ED, state->action);
}

static const char *led_reading(const LauncherView *v, const LauncherText *text,
			       u32 *color)
{
	*color = 0x526A68;
	if (v->page != UI_LIVE)
		return text->unknown;
	if (v->module.led_result >> 31)
		return text->led_failed;
	switch (v->module.state) {
	case LAUNCHER_READY:
		*color = 0x2869AD;
		return text->led_blue;
	case LAUNCHER_PAIRED:
	case LAUNCHER_SEARCHING:
		*color = 0x9B641C;
		return text->led_amber;
	case LAUNCHER_RECEIVING:
		*color = 0x18764C;
		return text->led_green;
	case LAUNCHER_ERROR:
		*color = 0xA54735;
		return text->led_red;
	default:
		return text->led_off;
	}
}

static void bottom_screen(Screen s, const LauncherView *v,
			  const LauncherText *text)
{
	rect(s, 0, 0, s.width, 240, PAPER);
	label(s, 20, 36, HEADING, INK, text->device);
	const int language_x = 300 - text_width(SMALL, text->switch_to);
	outline(s, language_x - 25, 20, 18, 18, 0x8BA496);
	label(s, language_x - 20, 34, SMALL, INK, "X");
	label(s, language_x, 34, SMALL, SECONDARY, text->switch_to);
	rect(s, 20, 50, 280, 1, 0xBBCBC2);
	label(s, 20, 84, BODY, SECONDARY, text->wifi);
	const char *wifi = v->device.wifi < 0 ? text->unknown
			   : v->device.wifi   ? text->connected
					      : text->disconnected;
	int wifi_x = 300 - text_width(BODY, wifi);
	label(s, wifi_x, 84, BODY, INK, wifi);
	if (v->device.wifi == 1 && v->device.strength >= 0) {
		for (int i = 0; i < 3; ++i) {
			int h = 4 + i * 4;
			rect(s, wifi_x - 24 + i * 5, 83 - h, 3, h,
			     i < v->device.strength ? 0x297259 : 0xA1B3A8);
		}
	}
	rect(s, 20, 99, 280, 1, 0xCFDAD2);
	const bool system_error =
	    v->page == UI_ROMFS || v->page == UI_LAUNCH ||
	    v->page == UI_MODEL_QUERY ||
	    (v->page == UI_LIVE && v->module.state == LAUNCHER_ERROR);
	if (system_error || v->page == UI_INSTALL) {
		char code[20];
		if (v->page == UI_INSTALL)
			snprintf(code, sizeof code, "%" PRIu32, v->detail);
		else
			snprintf(code, sizeof code, "0x%08" PRIX32,
				 v->page == UI_LIVE ? v->module.result
						    : v->detail);
		label(s, 20, 132, BODY, SECONDARY, text->error_code);
		label(s, 300 - text_width(BODY, code), 132, BODY, 0x99432F,
		      code);
	} else {
		u32 color;
		const char *reading = led_reading(v, text, &color);
		label(s, 20, 132, BODY, SECONDARY, text->led);
		const int x = 300 - text_width(BODY, reading);
		if (v->page == UI_LIVE)
			dot(s, x - 12, 125, 4, color);
		label(s, x, 132, BODY, INK, reading);
	}

	rect(s, 0, 158, s.width, 82, 0xDEE8DE);
	const int inset = v->page == UI_LIVE ? 0 : 12;
	outline(s, 20, 174 + inset, 58, 25, 0x8BA496);
	label(s, 29, 192 + inset, SMALL, INK, "START");
	label(s, 89, 193 + inset, BODY, INK, text->close);
	if (v->page == UI_LIVE)
		label(s, 20, 220, SMALL, SECONDARY, text->in_game);
}

void launcher_ui_draw(const LauncherView *v, const LauncherView *previous)
{
	const bool both =
	    !previous || v->language != previous->language ||
	    v->page != previous->page ||
	    (v->page == UI_LIVE && v->module.state != previous->module.state);
	const bool top = both ||
			 v->device.battery != previous->device.battery ||
			 v->device.charging != previous->device.charging;
	const bool bottom =
	    both || v->device.wifi != previous->device.wifi ||
	    v->device.strength != previous->device.strength ||
	    v->detail != previous->detail ||
	    v->module.led_result != previous->module.led_result ||
	    v->module.result != previous->module.result;
	const LauncherText *text = &launcher_text[v->language];
	if (top) {
		Screen s = {gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL),
			    400};
		top_screen(s, v, text);
		GSPGPU_FlushDataCache(s.pixels, s.width * 240 * 3);
	}
	if (bottom) {
		Screen s = {gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL),
			    320};
		bottom_screen(s, v, text);
		GSPGPU_FlushDataCache(s.pixels, s.width * 240 * 3);
	}
	if (top)
		gfxScreenSwapBuffers(GFX_TOP, false);
	if (bottom)
		gfxScreenSwapBuffers(GFX_BOTTOM, false);
}
