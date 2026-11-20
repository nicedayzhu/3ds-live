#include <assert.h>
#include <stdio.h>
#include "live_pair.h"
int main(void) {
    const unsigned rates[]={300000,400000,500000,600000,620000,700000,800000,900000,1000000,1200000,1400000,1600000,1800000,2000000};
    const unsigned frame_rates[]={15,20,24,30};
    for(unsigned ri=0;ri<sizeof rates/sizeof rates[0];ri++)for(unsigned fi=0;fi<4;fi++) {
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
    assert(!live_combined_budget(0,30)&&!live_combined_budget(800000,0));
    /* All four parity datagrams still occupy bandwidth when payload is empty. */
    assert(live_combined_wire(0)==LIVE_PAIR_PARITY*(LIVE_CHUNK+60u));
    assert(live_combined_budget(live_combined_wire(0)*30,30)==0);
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
