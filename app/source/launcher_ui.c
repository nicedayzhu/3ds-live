#include "launcher_ui.h"
#include <3ds.h>
#include <inttypes.h>
#include <stdio.h>

/* libctru presents BGR8 framebuffers as portrait columns, bottom pixel first. */
typedef struct { u8 *pixels; int width; } Screen;
typedef enum { SMALL, BODY, TITLE, HEADING } Face;
typedef struct { u32 offset, count; } FontFace;
typedef struct { u16 code; u8 advance, w, h; s8 left, top; u8 pad; u32 offset; } Glyph;
extern const u8 launcher_font[];
_Static_assert(sizeof(Glyph) == 12, "Font atlas record layout");

static void rect(Screen s, int x, int y, int w, int h, u32 color)
{
    for (int col = x; col < x + w && col < s.width; ++col) {
        if (col < 0) continue;
        for (int row = y; row < y + h && row < 240; ++row) {
            if (row < 0) continue;
            u8 *p = s.pixels + 3 * (240 * col + 239 - row);
            p[0] = (u8)color; p[1] = (u8)(color >> 8); p[2] = (u8)(color >> 16);
        }
    }
}

static void dot(Screen s, int x, int y, int radius, u32 color)
{
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx)
            if (dx*dx + dy*dy <= radius*radius) rect(s, x+dx, y+dy, 1, 1, color);
}

static void outline(Screen s, int x, int y, int w, int h, u32 color)
{
    rect(s,x,y,w,1,color); rect(s,x,y+h-1,w,1,color);
    rect(s,x,y+1,1,h-2,color); rect(s,x+w-1,y+1,1,h-2,color);
}

/* All strings in this UI use ASCII and BMP characters. */
static u16 next_code(const char **text)
{
    const u8 *p = (const u8 *)*text;
    u16 c = *p++;
    if (c >= 0xe0) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
    else if (c >= 0xc0) { c = ((c & 31) << 6) | (*p++ & 63); }
    *text = (const char *)p;
    return c;
}

static const Glyph *lookup(Face face, u16 code)
{
    const FontFace *f = (const FontFace *)launcher_font + face;
    const Glyph *table = (const Glyph *)(launcher_font + f->offset);
    u32 lo = 0, hi = f->count;
    while (lo < hi) {
        const u32 mid = lo + (hi-lo)/2;
        if (table[mid].code < code) lo = mid + 1;
        else hi = mid;
    }
    return lo < f->count && table[lo].code == code ? &table[lo] : NULL;
}

static int text_width(Face face, const char *text)
{
    int width = 0;
    while (*text) {
        const Glyph *g = lookup(face, next_code(&text));
        if (g) width += g->advance;
    }
    return width;
}

static void label(Screen s, int x, int baseline, Face face, u32 color, const char *text)
{
    while (*text) {
        const Glyph *g = lookup(face, next_code(&text));
        if (!g) continue;
        const u8 *mask = launcher_font + g->offset;
        for (int row = 0; row < g->h; ++row) {
            int y = baseline + g->top + row;
            if (y < 0 || y >= 240) continue;
            for (int col = 0; col < g->w; ++col) {
                int xx = x + g->left + col;
                if (xx < 0 || xx >= s.width) continue;
                int i = row*g->w + col;
                unsigned a = (mask[i/2] >> (4*(i%2))) & 15;
                if (!a) continue;
                u8 *dest = s.pixels + 3 * (240*xx + 239-y);
                for (int channel = 0; channel < 3; ++channel) {
                    unsigned src = (color >> (8*channel)) & 255;
                    dest[channel] = (u8)((src*a + dest[channel]*(15-a) + 7)/15);
                }
            }
        }
        x += g->advance;
    }
}

typedef struct {
    const char *title, *description, *action_key, *action;
    u32 accent;
} StateCopy;

enum {
    BLUE = 0x86BDFF, GREEN = 0x78DDAD, AMBER = 0xF3C178, RED = 0xFFA996,
    MUTED = 0xA9BFBC, PAPER = 0xEFF3ED, INK = 0x1B3533, SECONDARY = 0x425E59
};

static const StateCopy live_states[] = {
    [LAUNCHER_READY] = {"准备就绪", "在同一网络的电脑上打开 3DS Live。", "R + SELECT", "开始推流", BLUE},
    [LAUNCHER_STOPPED] = {"推流已停止", "可以留在这里，也可以回到游戏。", "R + SELECT", "重新开始推流", MUTED},
    [LAUNCHER_SEARCHING] = {"正在寻找电脑", "请打开 PC 播放器，并连接同一局域网。", "R + SELECT", "停止推流", AMBER},
    [LAUNCHER_PAIRED] = {"电脑已连接", "正在等待电脑确认收到完整画面。", "R + SELECT", "停止推流", AMBER},
    [LAUNCHER_RECEIVING] = {"正在投屏", "电脑已确认收到双屏画面。", "R + SELECT", "停止推流", GREEN},
    [LAUNCHER_SLEEP] = {"掌机已休眠", "开盖后会重新连接播放器。", "下一步", "开盖唤醒掌机", MUTED},
    [LAUNCHER_ERROR] = {"推流启动失败", "请检查无线连接，再尝试开启推流。", "R + SELECT", "重试推流", RED}
};

static const StateCopy startup_states[] = {
    [UI_STARTING] = {"正在启动", "正在检查 SD 卡和内置直播模块。", NULL, NULL, MUTED},
    [UI_WAIT_STATUS] = {"等待模块回应", "尚未读到当前直播模块的状态。", "状态文件", "/3ds/3ds-live/status.txt", AMBER},
    [UI_OLD_MODEL] = {"机型不支持", "当前掌机无法运行直播模块。", "支持机型", "New 3DS 或 New 2DS 系列", RED},
    [UI_ROMFS] = {"启动器读取失败", "无法打开启动器内的资源。", "下一步", "重新复制 3DSX 或安装 CIA", RED},
    [UI_BUNDLE] = {"模块文件无效", "启动器内的直播模块不完整。", "下一步", "重新复制 3DSX 或安装 CIA", RED},
    [UI_CONFLICT] = {"检测到文件冲突", "请核对 /luma/sysmodules/ 中的文件和备份。", "模块文件", "0004013000F1D402.cxi", AMBER},
    [UI_INSTALL] = {"模块写入失败", "无法将直播模块安装到 SD 卡。", "下一步", "检查 SD 卡空间和写入状态", RED},
    [UI_OLD_BUSY] = {"等待旧版停流", "新版模块已安装，旧版仍在使用中。", "下一步", "停流后重新打开启动器", AMBER},
    [UI_REBOOT] = {"需要重启掌机", "新版模块已安装，旧版尚未退出。", "下一步", "保存游戏进度后重启掌机", AMBER},
    [UI_LAUNCH] = {"模块未能启动", "请确认 Luma 已开启外部模块加载。", "下一步", "保存进度，检查设置后重启", RED},
    [UI_MODEL_QUERY] = {"无法识别机型", "系统未能返回掌机型号。", "下一步", "退出后重新打开启动器", RED}
};

static void battery(Screen s, const LauncherDevice *d)
{
    const char *caption = d->charging == 1 ? "充电中" : d->battery < 0 ? "电量未知" : "电量";
    label(s, 339 - text_width(SMALL, caption), 32, SMALL, MUTED, caption);
    outline(s, 347, 20, 28, 13, MUTED);
    rect(s, 375, 24, 2, 5, MUTED);
    if (d->battery < 0) {
        rect(s, 357, 26, 7, 1, MUTED);
        return;
    }
    for (int i = 0; i < d->battery && i < 5; ++i)
        rect(s, 350 + 5*i, 23, 3, 7, d->battery <= 1 && d->charging != 1 ? AMBER : GREEN);
}

static void top_screen(Screen s, const LauncherView *v)
{
    const StateCopy *state = v->page == UI_LIVE ? &live_states[v->module.state] : &startup_states[v->page];
    rect(s, 0, 0, s.width, 240, 0x14282A);
    // Two LCDs form the small product mark.
    outline(s, 22, 17, 21, 12, GREEN);
    outline(s, 24, 31, 17, 9, GREEN);
    label(s, 53, 35, HEADING, 0xF2F6EF, "3DS Live");
    battery(s, &v->device);
    rect(s, 22, 52, 356, 1, 0x38504F);
    dot(s, 26, 75, 4, state->accent);
    label(s, 39, 80, SMALL, state->accent, "直播状态");
    label(s, 21, 127, TITLE, 0xF6F7EF, state->title);
    label(s, 23, 157, BODY, 0xC9D9D4, state->description);

    if (!state->action_key) return;
    rect(s, 0, 183, s.width, 57, 0x203A3B);
    const int key_width = 18 + text_width(SMALL, state->action_key);
    rect(s, 22, 197, key_width, 27, 0x314D4D);
    label(s, 31, 216, SMALL, 0xE0EBE4, state->action_key);
    label(s, 34 + key_width, 217, BODY, 0xEFF5ED, state->action);
}

static const char *led_reading(const LauncherView *v, u32 *color)
{
    *color = 0x526A68;
    if (v->page != UI_LIVE) return "未知";
    if (v->module.led_result >> 31) return "设置失败";
    switch (v->module.state) {
        case LAUNCHER_READY: *color = 0x2869AD; return "蓝灯常亮";
        case LAUNCHER_PAIRED:
        case LAUNCHER_SEARCHING: *color = 0x9B641C; return "琥珀色呼吸";
        case LAUNCHER_RECEIVING: *color = 0x18764C; return "绿灯常亮";
        case LAUNCHER_ERROR: *color = 0xA54735; return "红灯短闪";
        default: return "熄灭";
    }
}

static void bottom_screen(Screen s, const LauncherView *v)
{
    rect(s, 0, 0, s.width, 240, PAPER);
    label(s, 20, 36, HEADING, INK, "设备信息");
    rect(s, 20, 50, 280, 1, 0xBBCBC2);
    label(s, 20, 84, BODY, SECONDARY, "无线网络");
    const char *wifi = v->device.wifi < 0 ? "未知" : v->device.wifi ? "已连接" : "未连接";
    int wifi_x = 300 - text_width(BODY, wifi);
    label(s, wifi_x, 84, BODY, INK, wifi);
    if (v->device.wifi == 1 && v->device.strength >= 0) {
        for (int i = 0; i < 3; ++i) {
            int h = 4 + i*4;
            rect(s, wifi_x-24 + i*5, 83-h, 3, h,
                 i < v->device.strength ? 0x297259 : 0xA1B3A8);
        }
    }
    rect(s, 20, 99, 280, 1, 0xCFDAD2);
    const bool system_error = v->page == UI_ROMFS || v->page == UI_LAUNCH || v->page == UI_MODEL_QUERY ||
        (v->page == UI_LIVE && v->module.state == LAUNCHER_ERROR);
    if (system_error || v->page == UI_INSTALL) {
        char code[20];
        if (v->page == UI_INSTALL) snprintf(code, sizeof code, "%" PRIu32, v->detail);
        else snprintf(code, sizeof code, "0x%08" PRIX32, v->page == UI_LIVE ? v->module.result : v->detail);
        label(s, 20, 132, BODY, SECONDARY, "错误代码");
        label(s, 300-text_width(BODY, code), 132, BODY, 0x99432F, code);
    } else {
        u32 color;
        const char *reading = led_reading(v, &color);
        label(s, 20, 132, BODY, SECONDARY, "通知灯");
        const int x = 300-text_width(BODY, reading);
        if (v->page == UI_LIVE) dot(s, x-12, 125, 4, color);
        label(s, x, 132, BODY, INK, reading);
    }

    rect(s, 0, 158, s.width, 82, 0xDEE8DE);
    const int inset = v->page == UI_LIVE ? 0 : 12;
    outline(s, 20, 174 + inset, 58, 25, 0x8BA496);
    label(s, 29, 192 + inset, SMALL, INK, "START");
    label(s, 89, 193 + inset, BODY, INK, "退出启动器");
    if (v->page == UI_LIVE)
        label(s, 20, 220, SMALL, SECONDARY, "退出后仍可在游戏里开始或停止推流。");
}

void launcher_ui_draw(const LauncherView *view)
{
    top_screen((Screen){gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), 400}, view);
    bottom_screen((Screen){gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL), 320}, view);
    gfxFlushBuffers();
    gfxSwapBuffers();
}
