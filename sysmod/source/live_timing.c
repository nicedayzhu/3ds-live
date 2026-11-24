#include "live_timing.h"

#ifdef LIVE_FRAME_TIMING
#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* On 3DS the file uses little-endian values. Keep binary layout independent
 * of the host compiler reading it; times are native ARM11 system ticks. */
typedef struct {
    u32 seq, pts, outcome;
    u16 accepted, planned;
    u64 encoded, queued, claimed, firstCall, allData, lastAccepted, terminal;
} TimingFrame;
typedef struct {
    char magic[4];
    u32 version, headerBytes, recordBytes, session, tickHz;
    u32 capacity, retained, latestSeq, reserved;
    u64 startTick, stopTick;
} TimingHeader;
_Static_assert(sizeof(TimingFrame) == 72 && offsetof(TimingFrame, encoded) == 16, "R12T record layout");
_Static_assert(sizeof(TimingHeader) == 56 && offsetof(TimingHeader, startTick) == 40, "R12T header layout");

static TimingFrame *ring;
static TimingFrame encoding, pendingFrames[2], sendingFrame;
static u32 capacity, retained, latestSeq, dataFragments;
static u64 lastMarker;
static bool prepared;
static LightLock ringLock;

void live_timing_prepare(void *(*allocate)(u32)) {
    /* Called after R12's capture arenas are allocated, before capture is enabled.
     * The allocator keeps a reserve in the system memory region. */
    capacity = retained = latestSeq = 0;
    lastMarker = 0;
    ring = NULL;
    encoding = pendingFrames[0] = pendingFrames[1] = sendingFrame = (TimingFrame){0};
    LightLock_Init(&ringLock);
    for (u32 count = 16384; count >= 1024; count /= 2) {
        ring = allocate(count * sizeof(*ring));
        if (ring) {
            memset(ring, 0, count * sizeof(*ring));
            capacity = count;
            break;
        }
    }
    __atomic_store_n(&prepared, true, __ATOMIC_RELEASE);
}

void live_timing_begin(u32 seq, u32 pts) {
    if (!capacity) return;
    /* Zero outcome means vacant. Keep seq zero valid across the wire's wrap. */
    encoding = (TimingFrame){.seq = seq, .pts = pts, .outcome = UINT32_MAX};
    latestSeq = seq;
    if (retained < capacity) retained++;
}

void live_timing_encoded(void) {
    if (capacity) encoding.encoded = svcGetSystemTick();
}

static void complete(TimingFrame *frame, LiveTimingOutcome why) {
    if (!capacity || !frame->outcome) return;
    frame->terminal = svcGetSystemTick();
    frame->outcome = why;
    /* A socket return can arrive very late compared with the encoder. Do not
     * overwrite a newer completed frame in the same ring slot. */
    LightLock_Lock(&ringLock);
    TimingFrame *slot = &ring[(frame->seq - 1) & (capacity - 1)];
    if (!slot->outcome || (u32)(frame->seq - slot->seq) < 0x80000000u)
        *slot = *frame;
    LightLock_Unlock(&ringLock);
    frame->outcome = 0;
}

void live_timing_drop_encoded(LiveTimingOutcome why) { complete(&encoding, why); }
void live_timing_drop_pending(unsigned slot, LiveTimingOutcome why) { complete(&pendingFrames[slot], why); }

/* The existing pair lock serializes publish/replace/claim. Each other frame
 * object has a single thread owner during capture. */
void live_timing_publish(unsigned slot, u64 queued) {
    if (!capacity) return;
    pendingFrames[slot] = encoding;
    pendingFrames[slot].queued = queued;
    encoding.outcome = 0;
}
void live_timing_claim(unsigned slot) {
    if (!capacity) return;
    sendingFrame = pendingFrames[slot];
    sendingFrame.claimed = svcGetSystemTick();
    pendingFrames[slot].outcome = 0;
}
void live_timing_plan(u32 data, u32 all) {
    dataFragments = data;
    sendingFrame.planned = (u16)all;
}
void live_timing_packet(u64 call, u64 returned, bool accepted) {
    if (!capacity) return;
    if (!sendingFrame.firstCall) sendingFrame.firstCall = call;
    if (accepted) {
        sendingFrame.accepted++;
        sendingFrame.lastAccepted = returned;
        if (sendingFrame.accepted == dataFragments) sendingFrame.allData = returned;
    }
}
void live_timing_finish(LiveTimingOutcome why) { complete(&sendingFrame, why); }

bool live_timing_marker(u64 now, u32 *available) {
    if (!__atomic_load_n(&prepared, __ATOMIC_ACQUIRE)) return false;
    if (lastMarker && now - lastMarker < 10ull * SYSCLOCK_ARM11) return false;
    lastMarker = now;
    *available = capacity;
    return true;
}

bool live_timing_export(u32 session, u64 started) {
    __atomic_store_n(&prepared, false, __ATOMIC_RELEASE);
    if (!ring) return false;
    /* Both workers have joined. A pending pair can remain when the user stops. */
    live_timing_drop_encoded(TIMING_STOPPED);
    for (unsigned slot = 0; slot < 2; ++slot) live_timing_drop_pending(slot, TIMING_STOPPED);
    live_timing_finish(TIMING_STOPPED);
    TimingHeader head = {{'R','1','2','T'}, 1, sizeof(head), sizeof(*ring), session,
                         SYSCLOCK_ARM11, capacity, retained, latestSeq, 0, started, svcGetSystemTick()};
    char path[80], temp[84];
    snprintf(path, sizeof(path), "sdmc:/3ds/3ds-live/live-timing-%08lx.bin", (unsigned long)session);
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    FILE *out = fopen(temp, "wb");
    bool ok = false;
    if (out) {
        u32 oldest = (latestSeq - retained) & (capacity - 1);
        u32 first = retained < capacity - oldest ? retained : capacity - oldest;
        ok = fwrite(&head, sizeof(head), 1, out) == 1 &&
             fwrite(ring + oldest, sizeof(*ring), first, out) == first &&
             fwrite(ring, sizeof(*ring), retained - first, out) == retained - first;
        if (fclose(out) != 0) ok = false;
        if (ok) ok = rename(temp, path) == 0;
        if (!ok) remove(temp);
    }
    ring = NULL;
    capacity = 0;
    return ok;
}
#endif
