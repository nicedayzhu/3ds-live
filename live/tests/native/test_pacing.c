#include "live_pacing.h"
#include "live_pair.h"
#include "packet_report.h"
#include <assert.h>
#include <stdio.h>

static void frame_pacing(void) {
    const unsigned ceiling=1900000, clock=1000000;
    const unsigned small=live_combined_wire(40), medium=live_combined_wire(36000);
    assert(live_video_pacing_rate(small,1,30,ceiling)==ceiling);
    assert(live_video_pacing_rate(live_combined_wire(4*LIVE_CHUNK),4,15,ceiling)==ceiling);
    unsigned pace=live_video_pacing_rate(medium,31,30,ceiling);
    assert(pace<ceiling);
    uint64_t duration=live_packet_due(0,0,medium,pace,clock);
    assert(duration>=30665&&duration<=30667); /* 92% of a 30 FPS slot */
    assert(live_video_pacing_rate(medium,31,15,ceiling)==(uint64_t)(medium*15u*100u+91)/92);
    unsigned heavy=live_combined_wire(100000);
    assert(live_video_pacing_rate(heavy,86,30,ceiling)==ceiling);
    assert(live_packet_due(0,0,heavy,ceiling,clock)>clock/30);
    /* A late or repeated key spends the same bytes. Missing its slot cannot
     * turn the next packets into a burst or steal the audio allocation. */
    uint64_t late=clock/30+7000;
    uint64_t video=live_packet_due(duration,late,1228,pace,clock);
    uint64_t total=live_packet_due(0,late,1228,2000000,clock);
    assert(video>total&&video-late==(1228ull*clock+pace-1)/pace);
    total=live_packet_due(total,total,440,2000000,clock); /* intervening audio */
    uint64_t next=video>total?video:total;
    assert(next>=late+834&&next>=video);
    assert(live_packet_due(video,next+4000,1228,pace,clock)==next+4000+(video-late));
}

static void completion_pacing(void) {
    const uint64_t hz=1000000,begin=10000,spacing=614;
    uint64_t byte_due=live_packet_due(8000,begin,1228,2000000,hz);
    assert(byte_due==begin+spacing);
    /* Regular SOC work fits wholly inside the original byte spacing. */
    assert(live_packet_return_due(byte_due,begin+300,spacing,hz,1)==byte_due);
    assert(live_packet_return_due(byte_due,begin+464,spacing,hz,1)==byte_due);
    /* An IPC that used up a full slot leaves a bounded post-return gap.
     * Mode 0 still admits the next datagram immediately on return. */
    assert(live_packet_return_due(byte_due,begin+800,spacing,hz,1)==begin+950);
    assert(live_packet_return_due(byte_due,begin+800,spacing,hz,0)==byte_due);
    assert(live_packet_return_due(begin+20,begin+300,20,hz,1)==begin+305);
    assert(live_packet_return_due(begin+10000,begin+20000,10000,hz,1)==begin+20150);
    uint64_t old=0,guarded=0;
    const uint32_t wires[]={1228,1102,40,1228,72,1228,60};
    const uint32_t ipc[]={432,800,180,900,100,440,300};
    for(unsigned i=0;i<2100;i++) {
        unsigned k=i%7;
        uint64_t start=guarded;
        uint64_t old_due=live_packet_due(old,start,wires[k],2000000,hz);
        guarded=live_packet_return_due(old_due,start+ipc[k],old_due-start,hz,1);
        assert(guarded>=old_due&&guarded>=start+ipc[k]);
        old=old_due;
    }

    LivePacketReport report={0};
    live_packet_observe(&report,PACKET_VIDEO,1000,1800,150,hz);
    live_packet_observe(&report,PACKET_AUDIO,1801,1881,0,hz);
    live_packet_observe(&report,PACKET_STATUS,2100,2210,0,hz);
    live_packet_observe_sleep(&report,2220,2300,2360);
    live_packet_observe_sleep(&report,2360,2400,2400);
    report.flags=7;
    report.first=(LivePacketSample){0,55,2,1000,1800};
    report.nack=(LivePacketSample){.seq=UINT32_MAX,.pts=77,.begin=2000};
    report.repair=(LivePacketSample){123,66,4,3000,3400};
    uint32_t w[LIVE_PACING_WORDS];
    live_packet_snapshot(&report,w,57,11,1,500,hz);
    assert(w[0]==UINT32_C(0x54313252)&&w[1]==57&&w[2]==11&&w[3]==1);
    assert(w[4]==2&&w[5]==120&&w[6]==60&&w[7]==60&&w[8]==1&&w[9]==150);
    assert(w[10]==7&&w[11]==0&&w[12]==55&&w[13]==2&&w[14]==500&&w[15]==1300);
    assert(w[16]==UINT32_MAX&&w[17]==77&&w[18]==1500);
    assert(w[19]==123&&w[20]==66&&w[21]==4&&w[22]==2500&&w[23]==2900);
    assert(w[24]==1&&w[25]==800&&w[26]==800&&w[27]==0&&w[28]==0&&w[29]==0);
    assert(w[36]==1&&w[37]==80&&w[38]==80&&w[39]==0&&w[40]==1&&w[41]==1);
    assert(w[42]==1&&w[43]==110&&w[44]==110&&w[45]==1&&w[46]==0&&w[47]==0);
    live_packet_snapshot(&report,w,57,12,0,500,hz);
    assert(w[6]==60&&w[7]==0&&w[10]==0&&w[16]==0&&w[25]==800&&w[26]==0&&w[41]==1);
    assert(live_packet_ticks_us((UINT64_C(1)<<32)+5,hz)==5);
    puts("SOC return spacing, byte ceiling and classified R21 diagnostics OK");
}

/* A 25 KiB pair approximates the exported session's 32 kB per-pair wire
 * average including parity. Fix SOC cost and wake latency explicitly; this
 * replay tests a scheduler property, not actual Wi-Fi throughput. */
static uint64_t paced_frame(int anchored,unsigned wake_lag) {
    const unsigned payload=25*1024,clock=1000000,total_rate=2000000;
    const unsigned data=live_data_packets(payload),packets=data+LIVE_PAIR_PARITY;
    LiveVideoPacing video={0};
    live_video_slot(&video,0,live_combined_wire(payload),data,30,1900000);
    uint64_t now=0,total_due=0,old_due=0,audio_ready=7000;
    for(unsigned i=0;i<packets;++i) {
        unsigned wire=(i+1==data?payload-i*LIVE_CHUNK:LIVE_CHUNK)+60;
        for(;;) {
            if(now>=audio_ready) {
                if(now<total_due)now=total_due+wake_lag;
                total_due=live_packet_due(total_due,now,1102,total_rate,clock);
                now+=432;
                audio_ready+=14666; /* three fresh 160-sample DSP blocks */
            }
            uint64_t video_due=anchored?live_video_due(&video):old_due;
            uint64_t due=video_due>total_due?video_due:total_due;
            if(now>=due)break;
            now=due+wake_lag;
        }
        assert(now>=total_due);
        total_due=live_packet_due(total_due,now,wire,total_rate,clock);
        if(anchored) {
            assert(now>=video.earliest);
            live_video_charge(&video,now,wire,clock);
            assert((video.earliest-now)*video.peak>=(uint64_t)wire*clock);
        } else old_due=live_packet_due(old_due,now,wire,video.rate,clock);
        now+=432;
    }
    return now;
}

static void slot_deadlines(void) {
    const unsigned clock=1000000,wire=live_combined_wire(25000);
    LiveVideoPacing video={0};
    live_video_slot(&video,0,wire,live_data_packets(25000),30,1900000);
    assert(video.peak==video.rate+video.rate/5);
    live_video_charge(&video,10000,1228,clock);
    uint64_t next=live_video_due(&video);
    assert(next>10000&&next<10000+((uint64_t)1228*clock+video.rate-1)/video.rate);
    /* A delayed frame and a new pair do not erase the last packet's
     * real-time spacing. A fully used ceiling admits no accelerated pace. */
    live_video_slot(&video,4000,live_combined_wire(100000),86,30,1900000);
    assert(video.peak==1900000&&video.rate==1900000&&live_video_due(&video)==next);
    live_video_charge(&video,next,1228,clock);
    assert(live_video_due(&video)==next+(1228ull*clock+1900000-1)/1900000);
    uint64_t old=paced_frame(0,150),candidate=paced_frame(1,150);
    assert(old>clock/30&&candidate<clock/30&&candidate<old);
    printf("25 KiB pair, simulated 150 us wake delay and audio: %llu -> %llu us\n",
           (unsigned long long)old,(unsigned long long)candidate);
}
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
    LiveConsumedPlan consumed={0};
    /* A missed source-buffer snapshot may retry the same stamp. After a
     * successful snapshot, stalled capture must never replay it. */
    assert(live_plan_available(&consumed,0)&&live_plan_available(&consumed,0));
    live_plan_consume(&consumed,0);
    for(unsigned i=0;i<1800;i++)assert(!live_plan_available(&consumed,0));
    assert(live_plan_available(&consumed,UINT32_MAX));
    live_plan_consume(&consumed,UINT32_MAX);
    assert(live_plan_available(&consumed,0)); /* new sample, even if its pixels match */
    frame_pacing();
    completion_pacing();
    slot_deadlines();
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
