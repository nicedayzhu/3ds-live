#include <3ds.h>
#include <stdio.h>
#include <sys/stat.h>
#include "buffers.h"
#include "capture.h"
#include "live_stream.h"
#include "live_timing.h"

// This is a system title with no APT foreground session. Initialize only its own heap, SRV and SD.
extern char *fake_heap_start, *fake_heap_end;
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;
void __sync_init(void);
void __sync_fini(void);
void __system_initSyscalls(void);
void __libc_init_array(void);
void __libc_fini_array(void);

static const u32 heapBytes = 384 * 1024;
static bool fsReady, sdReady, clockService;

void __system_allocateHeaps(void)
{
    u32 address = 0;
    if (R_FAILED(svcControlMemory(&address, 0x08000000, 0, heapBytes, MEMOP_ALLOC, MEMPERM_READWRITE))) svcExitProcess();
    __ctru_heap = address;
    __ctru_heap_size = heapBytes;
    __ctru_linear_heap = __ctru_linear_heap_size = 0;
    fake_heap_start = (char *)address;
    fake_heap_end = fake_heap_start + heapBytes;
}

void __appInit(void)
{
    Result rc;
    do {
        rc = srvInit();
        if (rc == (Result)0xD88007FA) svcSleepThread(500000);
    } while (rc == (Result)0xD88007FA);
    if (R_FAILED(rc)) svcExitProcess();
    srvSetBlockingPolicy(true);
    fsReady = R_SUCCEEDED(fsInit());
    sdReady = fsReady && R_SUCCEEDED(archiveMountSdmc());
    clockService = R_SUCCEEDED(ptmSysmInit());
}

void initSystem(void (*returnAddress)(void))
{
    (void)returnAddress;
    __sync_init();
    __system_initSyscalls();
    __system_allocateHeaps();
    __appInit();
    __libc_init_array();
}

void __appExit(void)
{
    if (clockService) ptmSysmExit();
    if (sdReady) archiveUnmountAll();
    if (fsReady) fsExit();
    srvExit();
}

void __ctru_exit(int code)
{
    (void)code;
    __libc_fini_array();
    __appExit();
    __sync_fini();
    svcExitProcess();
}

// These allocations belong to this process and exist only for the active stream.
// All callers release their references before the arena is freed, including after partial startup.
static u32 arenaNext = 0x09000000u;
static u32 arenaSizes[8], arenaCount;

void *live_map_pages(u32 address, u32 bytes)
{
    const u32 reserve = 4 * 1024 * 1024;
    if (osGetMemRegionFree(MEMREGION_SYSTEM) < bytes + reserve) return NULL;
    u32 result;
    if (R_FAILED(svcControlMemory(&result, address, 0, bytes, MEMOP_ALLOC, MEMPERM_READWRITE))) return NULL;
    return (void *)result;
}

void live_free_pages(u32 address, u32 bytes)
{
    u32 ignored;
    svcControlMemory(&ignored, address, 0, bytes, MEMOP_FREE, MEMPERM_DONTCARE);
}

static void *allocate(u32 bytes)
{
    if (!bytes || bytes > 0x01000000 || arenaCount == 8) return NULL;
    bytes = (bytes + 4095u) & ~4095u;
    if (arenaNext > 0x0C000000u - bytes) return NULL;
    void *address = live_map_pages(arenaNext, bytes);
    if (!address) return NULL;
    arenaSizes[arenaCount++] = bytes;
    arenaNext += bytes;
    return address;
}

static void release_arenas(void)
{
    while (arenaCount) {
        const u32 size = arenaSizes[--arenaCount];
        arenaNext -= size;
        live_free_pages(arenaNext, size);
    }
}

static void report(const char *state, Result result)
{
    if (!sdReady) return;
    FILE *file = fopen("sdmc:/3ds/3ds-live/status.txt", "w");
    if (file) {
        u32 pid = 0;
        svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
        fprintf(file, "pid=%lu\nstate=%s\nresult=%08lx\n", (unsigned long)pid, state, (unsigned long)(u32)result);
        fclose(file);
    }
}

static Result start_stream(void)
{
    s64 newModel = 0;
    svcGetSystemInfo(&newModel, 0x10000, 0x201);
    if (!newModel && osGetApplicationMemType() < 6) return -1;
    if (!live_requested()) return -2;
    Result rc = live_start(allocate);
    if (R_SUCCEEDED(rc)) {
        if (capture_start()) {
            live_timing_prepare(allocate);
            return 0;
        }
        capture_stop();
        live_stop();
        rc = -7;
    }
    release_arenas();
    return rc;
}

static int clockBefore = -1;
static u32 clockPid;
static bool boostActive;
static u64 quietUntil;

static int clock_state(void)
{
    s64 mhz, cache;
    if (R_FAILED(svcGetSystemInfo(&mhz, 0x10001, 0)) || R_FAILED(svcGetSystemInfo(&cache, 0x10001, 2))) return -1;
    return (mhz > 300 ? 1 : 0) | (cache ? 2 : 0);
}

static void set_boost(bool streaming)
{
    if (!clockService || streaming == boostActive || (streaming && svcGetSystemTick() < quietUntil)) return;
    const int state = clock_state();
    if (streaming) {
        clockBefore = -1;
        clockPid = capture_application_pid();
        if (state >= 0 && state != 3 && R_SUCCEEDED(PTMSYSM_ConfigureNew3DSCPU(3))) clockBefore = state;
    } else {
        if (clockBefore >= 0 && state == 3 && clockPid == capture_application_pid())
            PTMSYSM_ConfigureNew3DSCPU((u8)clockBefore);
        clockBefore = -1;
    }
    boostActive = streaming;
}

static void stop_stream(void)
{
    capture_stop();
    live_stop();
    release_arenas();
    set_boost(false);
}

int main(void)
{
    if (sdReady) {
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/3ds-live", 0777);
    }
    report("idle", 0);
    Handle notification = 0;
    if (R_SUCCEEDED(srvEnableNotification(&notification))) {
        srvSubscribe(0x100); // PM requests termination.
        srvSubscribe(PTMNOTIFID_SHUTDOWN);
        srvSubscribe(0x10C); // NS may choose a different CPU mode for a newly started application.
        srvSubscribe(0x213); // Lid opened.
        srvSubscribe(0x214); // Lid closed.
    }
    const u32 toggleKeys = KEY_R | KEY_SELECT;
    bool running = false, held = false, awake = true, resume = false, quit = false, previousPeer = false;
    while (!quit) {
        while (notification && R_SUCCEEDED(svcWaitSynchronization(notification, 0))) {
            u32 note;
            if (R_FAILED(srvReceiveNotification(&note))) break;
            if (note == 0x100 || note == PTMNOTIFID_SHUTDOWN) { quit = true; break; }
            if (note == 0x10C) {
                if (running) capture_reset();
                clockBefore = -1;
                boostActive = false;
                quietUntil = svcGetSystemTick() + 15ull * SYSCLOCK_ARM11;
            }
            if (note == 0x214) {
                awake = false;
                if (running) {
                    stop_stream();
                    running = false;
                    resume = true;
                    report("sleep", 0);
                }
            }
            if (note == 0x213) awake = true;
        }
        if (quit) break;
        if (awake) {
            const bool pressed = (~*(const volatile u32 *)0x1EC46000u & toggleKeys) == toggleKeys;
            if ((pressed && !held) || resume) {
                resume = false;
                if (running) {
                    stop_stream();
                    running = false;
                    report("idle", 0);
                } else {
                    Result rc = start_stream();
                    running = R_SUCCEEDED(rc);
                    previousPeer = false;
                    report(running ? "searching" : "error", rc);
                }
            }
            held = pressed;
            if (running) {
                const bool peer = live_peer_ready();
                if (peer != previousPeer) {
                    if (!peer) capture_reset();
                    report(peer ? "streaming" : "searching", 0);
                    previousPeer = peer;
                }
                set_boost(peer);
                if (peer) capture_poll();
            }
        }
        svcSleepThread(running && previousPeer ? 2000000 : 12000000);
    }
    if (running) stop_stream();
    if (notification) svcCloseHandle(notification);
    report("stopped", 0);
    return 0;
}
