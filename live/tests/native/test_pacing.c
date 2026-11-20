#include "live_pacing.h"
#include <assert.h>
#include <stdio.h>
/* One pending slot, 30 publications/s. A full-size pair occupies the
 * sender for 40 ms, including 10 ms of fixed audio, FEC and scheduling. */
static unsigned simulate_mailbox(int adaptive) {
    LivePayloadControl control={.percent=100};
    uint64_t busy_until=0;
    unsigned pending_us=0, dropped=0;
    for(unsigned frame=0;frame<1800;frame++) {
        uint64_t now=(uint64_t)frame*1000000/30;
        if(pending_us && busy_until<=now) {
            busy_until+=pending_us;
            pending_us=0;
        }
        unsigned service_us=10000+300*control.percent;
        int replaced=pending_us!=0;
        if(busy_until<=now) busy_until=now+service_us;
        else pending_us=service_us;
        dropped+=replaced;
        if(adaptive) live_payload_observe(&control,30,replaced);
    }
    return dropped;
}

int main(void) {
    LivePayloadControl c={.percent=100};
    for(unsigned n=0;n<300;n++) live_payload_observe(&c,30,n%30==0);
    assert(c.percent==100); /* one isolated overwrite per window */
    for(unsigned n=0;n<300;n++) live_payload_observe(&c,30,1);
    assert(c.percent==60);
    for(unsigned n=0;n<119;n++) live_payload_observe(&c,30,0);
    assert(c.percent==60);
    live_payload_observe(&c,30,0);
    assert(c.percent==62);
    for(unsigned n=0;n<3000;n++) live_payload_observe(&c,30,0);
    assert(c.percent==100);
    unsigned baseline=simulate_mailbox(0),candidate=simulate_mailbox(1);
    assert(baseline>=290 && candidate<baseline/4);
    printf("60 s mailbox simulation: replaced %u -> %u / 1800 pairs\n",baseline,candidate);
    /* 1228 bytes at 800 kB/s: one packet every 1535 us. A 600 us IPC
     * call must consume that interval, not lower throughput to 575 kB/s. */
    uint64_t now=0,due=0;
    for(unsigned i=0;i<1000;i++) {
        if(now<due)now=due;
        due=live_packet_due(due,now,1228,800000,1000000);
        now+=600;
    }
    assert(due==1535000);
    assert(1228000ull*1000000/due==800000);
    /* Idle periods and scheduling delays don't authorize bursts. */
    now=due+100000;
    assert(live_packet_due(due,now,1228,800000,1000000)==now+1535);
    /* Blocking longer than one packet interval also incurs no extra penalty. */
    now=due+2500;
    assert(live_packet_due(due,now,1228,800000,1000000)==now+1535);
    /* Rounding is upward, including the smallest tail packet. */
    assert(live_packet_due(0,0,61,700000,1000000)==88);
    /* Legacy game: 28 successful SOC submissions taking 3 ms each. A 30 FPS
     * deadline truncates EVERY pair; finishing one must not require 30 FPS. */
    now=due=0;
    unsigned oldPackets=0,newPackets=0;
    const uint64_t deadline=live_pair_deadline(0,1000000);
    for(unsigned i=0;i<28;i++) {
        if(now<due)now=due;
        if(now<=1000000/30)oldPackets++;
        if(now<=deadline)newPackets++;
        due=live_packet_due(due,now,1228,600000,1000000);
        now+=3000;
    }
    assert(oldPackets<24 && newPackets==28);
    assert(now>80000 && now<deadline);
    assert(28ull*1228*1000000/now<=600000); /* no burst / cap increase */
    assert(deadline==150000);
    assert(live_pair_deadline(1234,268123480)==1234+40218522);
    puts("packet pacing: byte cap, IPC time, idle and rounding OK");
    return 0;
}
