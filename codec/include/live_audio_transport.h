#ifndef LIVE_AUDIO_TRANSPORT_H
#define LIVE_AUDIO_TRANSPORT_H
#include "live_audio.h"
#include <stdbool.h>
#include <string.h>

/* About 75 KB/s at the DSP's native cadence, including one batch of
 * redundancy and IPv4/UDP headers. Reserve 100 KB/s from the *total* cap;
 * the shared packet pacer remains authoritative even under capture jitter. */
#define LIVE_AUDIO_RESERVE_BYTES 100000u
#define LIVE_AUDIO_QUEUE_BLOCKS 32u
#define LIVE_AUDIO_BATCH_BLOCKS 3u
#define LIVE_AUDIO_BATCH_WAIT_US 12000u
#define LIVE_AUDIO_MAX_AGE_US 80000u

/* One redundant batch two datagrams after its original. Keep the first
 * transmissions immediate, including startup and after a peer change. */
typedef struct {
    uint8_t records[LIVE_AUDIO_BATCH_BLOCKS*LIVE_AUDIO_RECORD_BYTES];
    uint64_t first_capture;
    unsigned count;
} LiveAudioBatch;
typedef struct { LiveAudioBatch batches[2]; unsigned next; } LiveAudioHistory;

static inline void live_audio_history_reset(LiveAudioHistory *h) {
    h->batches[0].count=h->batches[1].count=h->next=0;
}
static inline const LiveAudioBatch *live_audio_history_repeat(const LiveAudioHistory *h,
                                                             uint64_t now,uint64_t max_age) {
    const LiveAudioBatch *batch=&h->batches[h->next];
    if(!batch->count||now<batch->first_capture||now-batch->first_capture>max_age)return NULL;
    return batch;
}
static inline void live_audio_history_store(LiveAudioHistory *h,const uint8_t *fresh,
                                            unsigned count,uint64_t first_capture) {
    LiveAudioBatch *batch=&h->batches[h->next];
    memcpy(batch->records,fresh,count*LIVE_AUDIO_RECORD_BYTES);
    batch->count=count;batch->first_capture=first_capture;
    h->next^=1u;
}

typedef struct {
    uint64_t queued_tick;
    uint32_t sequence,pts_us;
    int16_t pcm[LIVE_AUDIO_SAMPLES*LIVE_AUDIO_CHANNELS];
} LiveAudioCapture;
typedef struct {
    LiveAudioCapture slots[LIVE_AUDIO_QUEUE_BLOCKS];
    uint32_t head,tail,sequence,queued,dropped,expired;
} LiveAudioQueue;
_Static_assert(sizeof(LiveAudioQueue)<=24u*1024u,"Bounded live audio storage");

static inline uint32_t live_av_video_rate(uint32_t total,bool audio) {
    return audio?(total>LIVE_AUDIO_RESERVE_BYTES?total-LIVE_AUDIO_RESERVE_BYTES:0):total;
}
/* One capture producer, one network consumer. Initialize only while stopped.
 * Monotonic u32 positions deliberately wrap; the queue is much smaller than
 * half their range. Failed submission still advances the block sequence. */
static inline void live_audio_queue_init(LiveAudioQueue *q) { memset(q,0,sizeof *q); }
static inline bool live_audio_queue_push(LiveAudioQueue *q,const int16_t *pcm,uint32_t pts,uint64_t tick) {
    const uint32_t seq=++q->sequence;
    const uint32_t head=__atomic_load_n(&q->head,__ATOMIC_RELAXED);
    const uint32_t tail=__atomic_load_n(&q->tail,__ATOMIC_ACQUIRE);
    if(head-tail>=LIVE_AUDIO_QUEUE_BLOCKS) {
        __atomic_add_fetch(&q->dropped,1,__ATOMIC_RELAXED);return false;
    }
    LiveAudioCapture *item=&q->slots[head%LIVE_AUDIO_QUEUE_BLOCKS];
    item->sequence=seq;item->pts_us=pts;item->queued_tick=tick;
    memcpy(item->pcm,pcm,sizeof item->pcm);
    __atomic_add_fetch(&q->queued,1,__ATOMIC_RELAXED);
    __atomic_store_n(&q->head,head+1,__ATOMIC_RELEASE);return true;
}
/* Consumer-only queries/pop; slot ownership remains with the consumer until
 * its whole copy is finished. An expired block leaves an explicit seq gap. */
static inline uint32_t live_audio_queue_depth(const LiveAudioQueue *q) {
    return __atomic_load_n(&q->head,__ATOMIC_ACQUIRE)-__atomic_load_n(&q->tail,__ATOMIC_RELAXED);
}
static inline bool live_audio_queue_peek_tick(const LiveAudioQueue *q,uint64_t *tick) {
    const uint32_t tail=__atomic_load_n(&q->tail,__ATOMIC_RELAXED);
    if(tail==__atomic_load_n(&q->head,__ATOMIC_ACQUIRE))return false;
    *tick=q->slots[tail%LIVE_AUDIO_QUEUE_BLOCKS].queued_tick;return true;
}
static inline bool live_audio_queue_pop(LiveAudioQueue *q,LiveAudioCapture *out,uint64_t tick,uint64_t max_age) {
    for(;;) {
        const uint32_t tail=__atomic_load_n(&q->tail,__ATOMIC_RELAXED);
        if(tail==__atomic_load_n(&q->head,__ATOMIC_ACQUIRE))return false;
        const LiveAudioCapture *item=&q->slots[tail%LIVE_AUDIO_QUEUE_BLOCKS];
        /* The producer can publish after the consumer took its clock
         * snapshot. A newer tick is fresh, not an unsigned-age overflow. */
        const bool expired=tick>=item->queued_tick&&tick-item->queued_tick>max_age;
        if(!expired)memcpy(out,item,sizeof *out);
        __atomic_store_n(&q->tail,tail+1,__ATOMIC_RELEASE);
        if(!expired)return true;
        __atomic_add_fetch(&q->expired,1,__ATOMIC_RELAXED);
    }
}
#endif
