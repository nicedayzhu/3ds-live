#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "live_audio_transport.h"
#include "live_pair.h"
#include "live_pacing.h"

static LiveAudioQueue q;
int main(void) {
    int16_t pcm[LIVE_AUDIO_SAMPLES*LIVE_AUDIO_CHANNELS];
    LiveAudioCapture out;
    live_audio_queue_init(&q);
    for(unsigned i=0;i<LIVE_AUDIO_QUEUE_BLOCKS;i++) {
        for(unsigned j=0;j<sizeof pcm/sizeof *pcm;j++)pcm[j]=(int16_t)(i*400+j);
        assert(live_audio_queue_push(&q,pcm,1000+i,100+i));
    }
    assert(!live_audio_queue_push(&q,pcm,9999,200));
    assert(q.dropped==1&&q.sequence==33&&live_audio_queue_depth(&q)==32);
    for(unsigned i=0;i<32;i++) {
        assert(live_audio_queue_pop(&q,&out,200,100));
        assert(out.sequence==i+1&&out.pts_us==1000+i);
        for(unsigned j=0;j<sizeof pcm/sizeof *pcm;j++)assert(out.pcm[j]==(int16_t)(i*400+j));
    }
    assert(!live_audio_queue_pop(&q,&out,200,100));
    assert(live_audio_queue_push(&q,pcm,2000,210));
    assert(live_audio_queue_pop(&q,&out,210,100)&&out.sequence==34);
    live_audio_queue_init(&q);
    q.head=q.tail=UINT_MAX-3u;q.sequence=UINT_MAX-3u;
    for(unsigned i=0;i<9;i++)assert(live_audio_queue_push(&q,pcm,UINT_MAX-i,100+i));
    uint64_t tick=0;assert(live_audio_queue_peek_tick(&q,&tick)&&tick==100);
    for(unsigned i=0;i<9;i++) {
        assert(live_audio_queue_pop(&q,&out,110,20));
        assert(out.sequence==(uint32_t)(UINT_MAX-2u+i));
        assert(out.pts_us==UINT_MAX-i);
    }
    assert(live_audio_queue_depth(&q)==0);
    assert(live_audio_queue_push(&q,pcm,0,200));
    assert(live_audio_queue_push(&q,pcm,1,280));
    assert(live_audio_queue_pop(&q,&out,300,50)&&out.pts_us==1);
    assert(q.expired==1&&!live_audio_queue_depth(&q));
    assert(live_audio_queue_push(&q,pcm,2,301));
    assert(live_audio_queue_pop(&q,&out,300,50)&&out.pts_us==2);
    assert(q.expired==1); /* producer published after the consumer's now */

    const unsigned rates[]={400000,500000,600000,800000,1000000,1200000,1600000,2000000};
    const unsigned fpss[]={15,20,24,30};
    for(unsigned ri=0;ri<sizeof rates/sizeof *rates;ri++)for(unsigned fi=0;fi<4;fi++) {
        const unsigned rate=rates[ri],fps=fpss[fi],video=live_av_video_rate(rate,true);
        assert(live_av_video_rate(rate,false)==rate);
        unsigned budget=live_pair_budget(video,fps);
        for(unsigned top=1;top<budget;top++) {
            uint64_t wire=(uint64_t)(live_screen_wire(top)+live_screen_wire(budget-top))*fps;
            assert(wire+LIVE_AUDIO_RESERVE_BYTES+136<=rate);
        }
    }
    /* Video, audio and status must share the same due. This alternating
     * schedule cannot create an independent audio burst allowance. */
    const uint64_t clock=268111856ull;uint64_t due=0,charged=0;
    for(unsigned i=0;i<5000;i++) {
        unsigned bytes=i%11==0?136:i%7==0?LIVE_AUDIO_MAX_PACKET_BYTES+28:1228;
        uint64_t begin=due;
        due=live_packet_due(due,begin,bytes,1000000,clock);charged+=bytes;
        assert(due*1000000/clock>=charged);
    }
    assert(LIVE_AUDIO_MAX_PACKET_BYTES<=1200);
    puts("audio transport: FIFO ownership, overflow, wrap, expiry, total A/V cap and shared pacing OK");
    return 0;
}
