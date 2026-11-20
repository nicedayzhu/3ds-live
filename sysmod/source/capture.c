// Capture only the displayed left-eye framebuffer of each LCD and the DSP final mix.
#include "capture.h"
#include "capture_schedule.h"
#include "buffers.h"
#include "live_stream.h"
#include <string.h>

#define VRAM_PHYS 0x18000000u
#define VRAM_MAPPED 0x1F000000u
#define VRAM_BYTES 0x00600000u
#define FCRAM_PHYS 0x20000000u
#define MAX_STRIDE (240u * 4u * 2u)
#define MIX_WORDS 160u

typedef enum { BUFFER_FREE, CONVERT_PENDING, CONVERTED, CONVERT_FAILED } ConvertState;
typedef struct {
    u8 *pixels;
    Handle dma;
    u32 addr, observedAddr, selection, mode, stride, bytes, capacity, submittedPts;
    u64 captured, modeChanged, retryAfter, lastPoll;
    u32 lastSwapUs;
    bool valid, wide, pendingChange, preciseSwap;
    u32 dmaMode, dmaStride;
    u64 dmaTick;
    ConvertState conversion;
} Screen;

static Screen screens[2];
static u64 epoch;
static u32 appPid, appBase;
static u64 nextAppLookup;
static Thread audioThread;
static Thread converter;
static LightEvent frameReady;
static bool stopping;
static bool audioReset;

static u32 timestamp(u64 ticks)
{
    const u64 elapsed = ticks - epoch;
    return (u32)((elapsed / SYSCLOCK_ARM11) * 1000000ull + (elapsed % SYSCLOCK_ARM11) * 1000000ull / SYSCLOCK_ARM11);
}

static void release_pixels(Screen *s)
{
    if (s->pixels) live_free_pages((u32)s->pixels, s->capacity);
    s->pixels = NULL;
    s->capacity = 0;
}

static bool in_vram(u32 addr)
{
    return addr >= VRAM_PHYS && addr - VRAM_PHYS < VRAM_BYTES;
}

static Handle open_display_application(void)
{
    Handle app;
    if (appPid && R_SUCCEEDED(svcOpenProcess(&app, appPid))) return app;
    appPid = appBase = 0;
    const u64 now = svcGetSystemTick();
    if (now < nextAppLookup) return 0;
    nextAppLookup = now + SYSCLOCK_ARM11 / 2;
    u32 pids[64];
    s32 count;
    if (R_FAILED(svcGetProcessList(&count, pids, 64))) return 0;
    for (s32 i = count < 64 ? count : 64; i > 0; --i) {
        if (R_FAILED(svcOpenProcess(&app, pids[i-1]))) continue;
        s64 title;
        if (R_SUCCEEDED(svcGetProcessInfo(&title, app, 0x10001))) {
            const u32 kind = (u64)title >> 32;
            if (kind == 0x00040000 || kind == 0x00040010 || kind == 0x00040002) {
                appPid = pids[i-1];
                return app;
            }
        }
        svcCloseHandle(app);
    }
    return 0;
}

u32 capture_application_pid(void)
{
    Handle app = open_display_application();
    if (app) svcCloseHandle(app);
    return appPid;
}

static bool framebuffer_mapped(Handle app, u32 va, u32 bytes)
{
    const u32 end = va + bytes;
    for (unsigned part = 0; part < 8 && va < end; ++part) {
        MemInfo info;
        PageInfo page;
        if (R_FAILED(svcQueryProcessMemory(&info, &page, app, va)) || !(info.perm & MEMPERM_READ) ||
            info.state == MEMSTATE_FREE || info.state == MEMSTATE_RESERVED || info.base_addr + info.size <= va)
            return false;
        va = info.base_addr + info.size;
    }
    return va >= end;
}

static Handle copy_display(const Screen *screen)
{
    Handle dma = 0;
    DmaConfig config;
    dmaConfigInitDefault(&config);
    config.flags = 0;
    if (in_vram(screen->addr)) {
        const u32 offset = screen->addr - VRAM_PHYS;
        if (screen->bytes > VRAM_BYTES - offset) return 0;
        if (R_FAILED(svcStartInterProcessDma(&dma, CUR_PROCESS_HANDLE, (u32)screen->pixels,
                     CUR_PROCESS_HANDLE, VRAM_MAPPED + offset, screen->bytes, &config))) return 0;
        return dma;
    }
    const u32 appBytes = OS_KernelConfig->memregion_sz[0];
    if (screen->addr < FCRAM_PHYS || screen->bytes > appBytes) return 0;
    const u32 offset = screen->addr - FCRAM_PHYS;
    if (offset > appBytes - screen->bytes) return 0;
    Handle app = open_display_application();
    if (!app) return 0;
    const u32 bases[] = {0x14000000u, 0x30000000u};
    for (unsigned i = 0; i < 2; ++i) {
        if (appBase && appBase != bases[i]) continue;
        const u32 va = bases[i] + offset;
        if (!framebuffer_mapped(app, va, screen->bytes)) continue;
        if (R_SUCCEEDED(svcStartInterProcessDma(&dma, CUR_PROCESS_HANDLE, (u32)screen->pixels,
                        app, va, screen->bytes, &config))) {
            appBase = bases[i];
            break;
        }
        dma = 0;
    }
    // The game must remain free to close or switch between any two display copies.
    svcCloseHandle(app);
    if (!dma) appPid = appBase = 0;
    return dma;
}

// A screen's one DMA buffer stays owned by the converter until it publishes the result.
// The display poller can still observe swaps on both LCDs while RGB -> YCbCr is running.
static void convert_main(void *unused)
{
    (void)unused;
    while (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) {
        LightEvent_Wait(&frameReady);
        for (unsigned index = 0; index < 2; ++index) {
            Screen *s = &screens[index];
            if (__atomic_load_n(&s->conversion, __ATOMIC_ACQUIRE) != CONVERT_PENDING) continue;
            bool ok = R_SUCCEEDED(svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)s->pixels, s->bytes));
            if (ok) {
                // Read the same 400 even columns in 800-wide mode without rewriting the DMA buffer.
                const u32 stride = s->wide ? 2 * s->dmaStride : s->dmaStride;
                ok = live_submit(index, s->pixels, s->dmaMode, stride, timestamp(s->dmaTick), false);
            }
            __atomic_store_n(&s->conversion, ok ? CONVERTED : CONVERT_FAILED, __ATOMIC_RELEASE);
        }
    }
}

static void poll_screen(unsigned index, u64 now)
{
    Screen *s = &screens[index];
    const volatile u32 *lcd = (const volatile u32 *)(0x1EF00400u + index*0x100u);
    const u32 selection = lcd[0x78/4] & 1;
    const u32 addr = lcd[selection ? 0x6C/4 : 0x68/4];
    const u32 mode = lcd[0x70/4], stride = lcd[0x90/4];
    if (mode != s->mode || stride != s->stride || in_vram(addr) != in_vram(s->observedAddr)) {
        s->mode = mode;
        s->stride = stride;
        s->valid = false;
        s->captured = 0;
        s->modeChanged = now;
    }
    // Remember intervening swaps. A 60 Hz game can already be back on the same buffer when
    // the 30 Hz capture is due, even though its contents changed twice in the meantime.
    const bool changed = addr != s->observedAddr || selection != s->selection;
    const u32 nowUs = timestamp(now);
    if (changed) {
        s->pendingChange = true;
        s->lastSwapUs = nowUs;
        s->preciseSwap = now - s->lastPoll <= SYSCLOCK_ARM11 * 3ull / 1000;
    }
    s->lastPoll = now;
    s->observedAddr = addr;
    s->selection = selection;
    if (s->dma) {
        DmaState state;
        Result rc = svcGetDmaState(&state, s->dma);
        if (R_SUCCEEDED(rc) && state == DMASTATE_DONE) {
            svcCloseHandle(s->dma);
            s->dma = 0;
            if (s->captured == s->dmaTick) {
                __atomic_store_n(&s->conversion, CONVERT_PENDING, __ATOMIC_RELEASE);
                LightEvent_Signal(&frameReady);
            }
        } else if (R_FAILED(rc) || now - s->dmaTick > SYSCLOCK_ARM11 / 10) {
            svcStopDma(s->dma);
            svcCloseHandle(s->dma);
            s->dma = 0;
        } else return;
    }
    const ConvertState conversion = __atomic_load_n(&s->conversion, __ATOMIC_ACQUIRE);
    if (conversion == CONVERT_PENDING) return;
    if (conversion == CONVERTED && s->captured == s->dmaTick) {
        s->submittedPts = timestamp(s->dmaTick);
        s->valid = true;
    }
    if (conversion != BUFFER_FREE) __atomic_store_n(&s->conversion, BUFFER_FREE, __ATOMIC_RELEASE);
    if (now - s->modeChanged < SYSCLOCK_ARM11 / 5) return;
    const unsigned format = mode & 7;
    const u32 pixelBytes = format == 0 ? 4 : format == 1 ? 3 : 2;
    if (format > 4 || stride < pixelBytes * 240 || stride > MAX_STRIDE) return;
    if (s->captured && !live_capture_due(nowUs - timestamp(s->captured), live_fps(), changed,
            s->pendingChange, s->preciseSwap, nowUs - s->lastSwapUs, in_vram(addr))) return;
    s->wide = !index && !(mode & 0x60);
    s->bytes = (s->wide ? 800u : index ? 320u : 400u) * stride;
    s->addr = addr;
    if (now < s->retryAfter) return;
    if (s->capacity < s->bytes) {
        // Each LCD owns its own 2 MiB VA window, allocated to its current format only.
        release_pixels(s);
        const u32 size = (s->bytes + 4095u) & ~4095u;
        s->pixels = live_map_pages(0x0D000000u + index * 0x200000u, size);
        if (!s->pixels) {
            s->retryAfter = now + SYSCLOCK_ARM11 / 2;
            return;
        }
        s->capacity = size;
    }
    // Hand the buffer to DMA only after the previous reader has released it.
    if (R_FAILED(svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)s->pixels, s->bytes))) return;
    s->dma = copy_display(s);
    if (s->dma) {
        s->dmaTick = s->captured = now;
        s->dmaMode = mode;
        s->dmaStride = stride;
        s->pendingChange = false;
    }
}

static bool dsp_running(void)
{
    return (*(const volatile u8 *)0x1EC41230u & 3u) == 3u;
}

static void audio_main(void *unused)
{
    (void)unused;
    struct Mix { u32 last[MIX_WORDS], prev[MIX_WORDS], data[MIX_WORDS]; u64 changed; } mix[2];
    bool primed = false;
    while (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) {
        if (__atomic_exchange_n(&audioReset, false, __ATOMIC_ACQ_REL)) primed = false;
        if (!live_peer_ready() || !live_audio_active() || !dsp_running()) {
            primed = false;
            svcSleepThread(5000000);
            continue;
        }
        const u64 now = svcGetSystemTick();
        bool stable[2] = {false, false};
        bool readable = true;
        for (unsigned r = 0; r < 2; ++r) {
            struct Mix *m = &mix[r];
            const u32 address = 0x1FF50A80u + r * 0x20000u;
            if (R_FAILED(svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, address, sizeof m->data))) {
                readable = false;
                break;
            }
            const volatile u32 *src = (const volatile u32 *)address;
            for (unsigned i = 0; i < MIX_WORDS; ++i) m->data[i] = src[i];
            if (!primed) memcpy(m->last, m->data, sizeof m->data);
            if (!memcmp(m->last, m->data, sizeof m->data)) m->changed = 0;
            else if (!m->changed) m->changed = now;
            else stable[r] = !memcmp(m->prev, m->data, sizeof m->data);
            memcpy(m->prev, m->data, sizeof m->data);
        }
        primed = readable && dsp_running();
        if (primed) {
            unsigned first = stable[1] && (!stable[0] || mix[1].changed < mix[0].changed);
            for (unsigned i = 0; i < 2; ++i) {
                unsigned r = first ^ i;
                if (!stable[r]) continue;
                struct Mix *m = &mix[r];
                live_audio_submit((const s16 *)m->data, timestamp(m->changed));
                memcpy(m->last, m->data, sizeof m->data);
                m->changed = 0;
            }
        }
        svcSleepThread(1000000);
    }
}

bool capture_start(void)
{
    memset(screens, 0, sizeof screens);
    appPid = appBase = 0;
    nextAppLookup = 0;
    epoch = svcGetSystemTick();
    __atomic_store_n(&stopping, false, __ATOMIC_RELEASE);
    LightEvent_Init(&frameReady, RESET_ONESHOT);
    converter = threadCreate(convert_main, NULL, 8192, 0x30, 2, false);
    if (!converter) converter = threadCreate(convert_main, NULL, 8192, 0x3f, 1, false);
    if (!converter) return false;
    // A DSP mix block lasts 4.89 ms. Poll above the display/DMA thread (0x28)
    // so video work cannot postpone both reads past the buffer's next overwrite.
    audioThread = threadCreate(audio_main, NULL, 8192, 0x18, 1, false);
    return audioThread != NULL;
}

void capture_reset(void)
{
    appPid = appBase = 0;
    nextAppLookup = 0;
    for (unsigned i = 0; i < 2; ++i) {
        Screen *s = &screens[i];
        if (s->dma) {
            svcStopDma(s->dma);
            svcCloseHandle(s->dma);
            s->dma = 0;
        }
        while (__atomic_load_n(&s->conversion, __ATOMIC_ACQUIRE) == CONVERT_PENDING)
            svcSleepThread(1000000);
        __atomic_store_n(&s->conversion, BUFFER_FREE, __ATOMIC_RELEASE);
        s->valid = false;
        s->captured = 0;
        s->retryAfter = 0;
        release_pixels(s);
        s->pendingChange = false;
        s->modeChanged = svcGetSystemTick();
    }
    __atomic_store_n(&audioReset, true, __ATOMIC_RELEASE);
    live_schedule(0, 0, 0, 0);
}

void capture_poll(void)
{
    const u64 now = svcGetSystemTick();
    poll_screen(0, now);
    poll_screen(1, svcGetSystemTick());
    const u32 mask = (screens[0].valid ? 1 : 0) | (screens[1].valid ? 2 : 0);
    live_schedule(timestamp(now), screens[0].submittedPts, screens[1].submittedPts, mask);
}

void capture_stop(void)
{
    __atomic_store_n(&stopping, true, __ATOMIC_RELEASE);
    if (converter) {
        LightEvent_Signal(&frameReady);
        threadJoin(converter, U64_MAX);
        threadFree(converter);
        converter = NULL;
    }
    if (audioThread) {
        threadJoin(audioThread, U64_MAX);
        threadFree(audioThread);
        audioThread = NULL;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (screens[i].dma) {
            svcStopDma(screens[i].dma);
            svcCloseHandle(screens[i].dma);
        }
        release_pixels(&screens[i]);
    }
    memset(screens, 0, sizeof screens);
}
