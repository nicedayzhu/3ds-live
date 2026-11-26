#include <assert.h>
#include <stdio.h>
#include "live_pair.h"
int main(void) {
    const unsigned rates[]={300000,400000,500000,600000,620000,700000,800000,900000,1000000,1200000,1400000,1600000,1800000,2000000};
    const unsigned frame_rates[]={15,20,24,30};
    for(unsigned ri=0;ri<sizeof rates/sizeof rates[0];ri++)for(unsigned fi=0;fi<sizeof frame_rates/sizeof frame_rates[0];fi++) {
        unsigned rate=rates[ri],fps=frame_rates[fi];
        assert(live_valid_fps(fps));
        unsigned budget=live_pair_budget(rate,fps);
        unsigned combined=live_combined_budget(rate,fps);
        assert(live_combined_wire(combined)<=rate/fps*96/100);
        assert(live_combined_wire(combined+1)>rate/fps*96/100);
        assert(combined>=budget);
        assert(budget<=128000);
        for(unsigned top=1024;top<budget-1024;top++) {
            unsigned bottom=budget-top;
            assert((uint64_t)(live_screen_wire(top)+live_screen_wire(bottom))*fps+104<=rate);
        }
        for(unsigned q=2;q<=95;q++)assert(live_lower_quality(q,budget+1,budget)<q);
    }
    assert(!live_valid_fps(60)&&!live_valid_fps(0));
    assert(!live_valid_fps(35)&&!live_valid_fps(40)&&!live_valid_fps(45));
    assert(live_migrate_fps(35)==30&&live_migrate_fps(40)==30&&live_migrate_fps(24)==24);
    assert(live_media_fps_flag(35)==1&&live_media_fps_flag(40)==2&&!live_media_fps_flag(45));
    const uint8_t r6[20]="R6CF",r10[24]="R10C";
    uint8_t r16[28]="R16C";
    assert(live_config_jpeg_target(r6,sizeof r6,80)==80);
    assert(live_config_jpeg_target(r10,sizeof r10,20)==20);
    assert(live_config_jpeg_target(r16,sizeof r16,80)==0);
    r16[24]=20;
    assert(live_config_jpeg_target(r16,sizeof r16,0)==20);
    r16[25]=1;
    assert(!live_valid_quality_target(live_config_jpeg_target(r16,sizeof r16,0)));
    for(unsigned q=0;q<=100;q++)
        assert(live_valid_quality_target(q)==(q==0||q==15||q==20||q==25||q==30||q==35||q==40||q==50||q==60||q==70||q==80));
    const unsigned manual_targets[]={20,40,60,80};
    for(unsigned ri=0;ri<sizeof rates/sizeof rates[0];ri++)for(unsigned fi=0;fi<4;fi++) {
        uint32_t video=rates[ri],fps=frame_rates[fi];
        for(int combined=0;combined<=1;combined++) {
            uint32_t regular=live_quality_budget(video,fps,combined,0);
            assert(regular==(combined?live_combined_budget(video,fps):live_pair_budget(video,fps)));
            for(unsigned qi=0;qi<sizeof manual_targets/sizeof manual_targets[0];qi++) {
                unsigned target=manual_targets[qi];
                uint32_t cap=live_quality_budget(video,fps,combined,target);
                assert(cap>regular);
                assert(cap==live_quality_budget(video,15,combined,target));
                /* Heavy frames keep their explicit Q even above the window;
                 * automatic mode retains its bounded requantization. */
                for(unsigned attempt=0;attempt<3;attempt++)
                    assert(live_retry_quality(target,target,attempt,cap+1000,cap)==target);
                for(unsigned top=1;top<cap;top+=113) {
                    uint32_t bottom=cap-top;
                    uint32_t wire=combined?live_combined_wire(cap):live_screen_wire(top)+live_screen_wire(bottom);
                    uint64_t us=live_video_deadline(0,1000000,wire,video,1);
                    assert(us<=LIVE_MANUAL_SEND_MAX_MS*1000u);
                    assert(us*(video*96/100)>=(uint64_t)wire*1000000);
                }
            }
        }
    }
    assert(live_retry_quality(0,80,0,50000,20000)<80);
    assert(live_retry_quality(0,50,1,50000,20000)==1);
    assert(live_retry_quality(0,50,2,50000,20000)==50);
    /* At the lowest audio-enabled rate, a 68 KB Q80 pair exceeds the old
     * 100 ms budget. It can occupy a sender for more than 150 ms while
     * capture continues at exactly the requested 15/20/24/30 FPS. */
    const uint32_t wire=live_combined_wire(68000),video=300000;
    for(unsigned i=0;i<4;i++) {
        uint32_t fps=frame_rates[i];
        assert(68000u<=live_quality_budget(video,fps,1,80));
        uint64_t send_us=live_video_deadline(0,1000000,wire,video,1);
        assert(send_us>150000&&send_us<400000);
        assert(live_video_deadline(0,1000000,wire,video,0)==150000);
        assert(live_video_deadline(0,1000000,5000,video,1)==150000);
        uint64_t tick=0;
        for(unsigned j=0;j<fps;j++)tick=live_next_encode_tick(tick,tick,1000000,fps);
        assert(tick==fps*(1000000u/fps));
        uint64_t late=tick+send_us;
        assert(live_next_encode_tick(tick,late,1000000,fps)==late+1000000u/fps);
    }
    assert(live_video_deadline(0,1000000,UINT32_MAX,video,1)==LIVE_MANUAL_SEND_MAX_MS*1000u);
    assert(!live_combined_budget(0,30)&&!live_combined_budget(800000,0));
    /* Nonempty stripes keep their original full-size parity datagrams.
     * Verify every transition, including unused stripes for a tiny pair. */
    assert(live_screen_wire(0)==0&&live_combined_wire(0)==0);
    for(unsigned bytes=1;bytes<=6*LIVE_CHUNK;bytes++) {
        unsigned count=(bytes+LIVE_CHUNK-1)/LIVE_CHUNK,chunkWire=LIVE_CHUNK+60;
        unsigned combined=bytes+60*count+(count<4?count:4)*chunkWire;
        unsigned screen=bytes+60*count+(count<2?count:2)*chunkWire;
        assert(live_screen_wire(bytes)==screen&&live_combined_wire(bytes)==combined);
    }
    for(unsigned wire=0;wire<12*(LIVE_CHUNK+60);wire++) {
        unsigned limit=live_combined_payload_limit(wire);
        assert(live_combined_wire(limit)<=wire&&live_combined_wire(limit+1)>wire);
    }
    uint8_t top[3509],bottom[1267],chunk[LIVE_CHUNK+16];
    for(unsigned i=0;i<sizeof top;i++)top[i]=(uint8_t)(i%251);
    for(unsigned i=0;i<sizeof bottom;i++)bottom[i]=(uint8_t)(i%239);
    for(unsigned index=0;index<4;index++) {
        unsigned len=live_pair_fragment(chunk,top,sizeof top,bottom,0,index);
        assert(len==(index==3?5:LIVE_CHUNK)&&!memcmp(chunk,top+index*LIVE_CHUNK,len));
    }
    assert(!live_pair_fragment(chunk,top,sizeof top,bottom,0,4));
    for(unsigned nt=12;nt<=sizeof top;nt+=17)for(unsigned nb=12;nb<=sizeof bottom;nb+=19) {
        unsigned seen=0;
        for(unsigned index=0;;index++) {
            memset(chunk,0xa5,sizeof chunk);
            unsigned size=live_pair_fragment(chunk,top,nt,bottom,nb,index);
            if(!size)break;
            for(unsigned i=0;i<size;i++,seen++)assert(chunk[i]==(seen<nt?top[seen]:bottom[seen-nt]));
            for(unsigned i=size;i<sizeof chunk;i++)assert(chunk[i]==0xa5);
        }
        assert(seen==nt+nb);
        assert(!live_pair_fragment(chunk,top,nt,bottom,nb,UINT32_MAX));
    }
    puts("pair budget: all splits, parity, headers and status fit fixed rate");return 0;
}
