#ifndef LIVE_PACKET_REPORT_H
#define LIVE_PACKET_REPORT_H

#include <stdint.h>
#include <string.h>

enum LivePacketKind { PACKET_VIDEO, PACKET_REPAIR, PACKET_AUDIO, PACKET_STATUS, PACKET_KINDS };
enum { LIVE_PACING_WORDS = 48 };

typedef struct {
    uint64_t ipc_ticks, peak_ticks;
    uint32_t calls, short_starts, short_returns, short_handoffs;
} LivePacketClass;

typedef struct {
    uint32_t seq,pts,index;
    uint64_t begin,end;
} LivePacketSample;

typedef struct {
    LivePacketClass kinds[PACKET_KINDS];
    uint64_t previous_start,previous_return;
    uint64_t requested_sleep,oversleep,peak_oversleep,guard_ticks;
    uint32_t sleeps,guards,flags;
    LivePacketSample first,nack,repair;
} LivePacketReport;

static inline uint32_t live_packet_ticks_us(uint64_t ticks,uint64_t hz) {
    return (uint32_t)((ticks/hz)*1000000u+((ticks%hz)*1000000u)/hz);
}

static inline void live_packet_observe(LivePacketReport *r,enum LivePacketKind kind,
                                       uint64_t start,uint64_t end,uint64_t guard,uint64_t hz) {
    LivePacketClass *c=&r->kinds[kind];
    uint64_t ipc=end-start;
    c->calls++;
    c->ipc_ticks+=ipc;
    if(ipc>c->peak_ticks)c->peak_ticks=ipc;
    if(r->previous_return) {
        uint64_t short_gap=(hz*300u+999999u)/1000000u;
        c->short_starts+=start-r->previous_start<short_gap;
        c->short_returns+=end-r->previous_return<short_gap;
        c->short_handoffs+=start-r->previous_return<(hz*150u+999999u)/1000000u;
    }
    r->previous_start=start;
    r->previous_return=end;
    if(guard){r->guards++;r->guard_ticks+=guard;}
}

static inline void live_packet_observe_sleep(LivePacketReport *r,uint64_t start,uint64_t due,uint64_t end) {
    r->sleeps++;
    r->requested_sleep+=due-start;
    if(end>due) {
        uint64_t late=end-due;
        r->oversleep+=late;
        if(late>r->peak_oversleep)r->peak_oversleep=late;
    }
}

static inline void live_packet_sample_words(uint32_t *w,const LivePacketSample *s,uint64_t epoch,uint64_t hz) {
    w[0]=s->seq;w[1]=s->pts;w[2]=s->index;
    w[3]=live_packet_ticks_us(s->begin-epoch,hz);w[4]=live_packet_ticks_us(s->end-epoch,hz);
}

/* Called on the socket thread before this round's status datagrams.
 * Cumulative words wrap naturally. Peaks and sample flags delimit reports. */
static inline void live_packet_snapshot(LivePacketReport *r,uint32_t w[LIVE_PACING_WORDS],uint32_t sid,
                                        uint32_t elapsed,uint32_t mode,uint64_t epoch,uint64_t hz) {
    memset(w,0,LIVE_PACING_WORDS*sizeof(*w));
    w[0]=UINT32_C(0x54313252); /* R21T in little endian */
    w[1]=sid;w[2]=elapsed;w[3]=mode;w[4]=r->sleeps;
    w[5]=live_packet_ticks_us(r->requested_sleep,hz);w[6]=live_packet_ticks_us(r->oversleep,hz);
    w[7]=live_packet_ticks_us(r->peak_oversleep,hz);w[8]=r->guards;
    w[9]=live_packet_ticks_us(r->guard_ticks,hz);w[10]=r->flags;
    if(r->flags&1u)live_packet_sample_words(w+11,&r->first,epoch,hz);
    if(r->flags&2u){w[16]=r->nack.seq;w[17]=r->nack.pts;w[18]=live_packet_ticks_us(r->nack.begin-epoch,hz);}
    if(r->flags&4u)live_packet_sample_words(w+19,&r->repair,epoch,hz);
    r->flags=0;r->peak_oversleep=0;
    for(unsigned k=0;k<PACKET_KINDS;k++) {
        LivePacketClass *c=&r->kinds[k];
        uint32_t *out=w+24+6*k;
        out[0]=c->calls;out[1]=live_packet_ticks_us(c->ipc_ticks,hz);out[2]=live_packet_ticks_us(c->peak_ticks,hz);
        out[3]=c->short_starts;out[4]=c->short_returns;out[5]=c->short_handoffs;
        c->peak_ticks=0;
    }
}

#endif
