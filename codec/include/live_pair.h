#ifndef LIVE_PAIR_H
#define LIVE_PAIR_H
#include <stdint.h>
#include <string.h>
#define LIVE_CHUNK 1168u
#define LIVE_PARITY 2u
#define LIVE_PAIR_PARITY 4u
#define LIVE_MIN_RATE 400000u
#define LIVE_MAX_RATE 2000000u
#define LIVE_FPS_MASK ((1u<<15)|(1u<<20)|(1u<<24)|(1u<<30))
static inline int live_valid_fps(uint32_t fps) { return fps==15||fps==20||fps==24||fps==30; }
/* Two striped XOR packets per screen. Count all headers and parity in the
 * fixed budget. 4% headroom covers status and scheduling, never burst credit. */
static inline uint32_t live_screen_wire(uint32_t bytes) {
    return bytes+60*((bytes+LIVE_CHUNK-1)/LIVE_CHUNK)+LIVE_PARITY*(LIVE_CHUNK+60);
}
static inline uint32_t live_pair_budget(uint32_t rate,uint32_t fps) {
    uint32_t available=rate/fps*96/100-2*LIVE_PARITY*(LIVE_CHUNK+60);
    return (available-120)*LIVE_CHUNK/(LIVE_CHUNK+60);
}
/* R10 joins the two payloads before fragmentation. Four stripe packets still
 * cost exactly as much as the old two-per-screen FEC. Charge the actual wire
 * payload (including the compact JPEG header), not the reconstructed JPEG. */
static inline uint32_t live_combined_wire(uint32_t bytes) {
    return bytes+60*((bytes+LIVE_CHUNK-1)/LIVE_CHUNK)+LIVE_PAIR_PARITY*(LIVE_CHUNK+60);
}
static inline uint32_t live_combined_budget(uint32_t rate,uint32_t fps) {
    if(!fps)return 0;
    uint32_t available=rate/fps*96/100,parity=LIVE_PAIR_PARITY*(LIVE_CHUNK+60);
    if(available<=parity)return 0;
    available-=parity;
    uint32_t tail=available%(LIVE_CHUNK+60);
    return available/(LIVE_CHUNK+60)*LIVE_CHUNK+(tail>60?tail-60:0);
}
/* Copy one bounded chunk of the logical concatenation, without a frame-sized
 * extra allocation. Shared by the ARM sender and host protocol conformance QA. */
static inline uint32_t live_pair_fragment(uint8_t *out,const uint8_t *top,uint32_t nt,
                                         const uint8_t *bottom,uint32_t nb,uint32_t index) {
    uint64_t offset=(uint64_t)index*LIVE_CHUNK,total=(uint64_t)nt+nb;
    if(!out||!top||!bottom||offset>=total)return 0;
    uint32_t length=total-offset>LIVE_CHUNK?LIVE_CHUNK:(uint32_t)(total-offset);
    uint32_t first=offset<nt?nt-(uint32_t)offset:0;if(first>length)first=length;
    if(first)memcpy(out,top+(uint32_t)offset,first);
    if(length>first)memcpy(out+first,bottom+(uint32_t)(offset+first-nt),length-first);
    return length;
}
static inline unsigned live_lower_quality(unsigned quality,uint32_t size,uint32_t budget) {
    unsigned scale=quality<50?5000/quality:200-2*quality;
    uint32_t next=(uint32_t)((uint64_t)scale*size*110/(budget*100));
    unsigned q=next>100?5000/(next?next:1):100-next/2;
    if(q>=quality)q=quality>4?quality-4:1;
    return q<1?1:q;
}
#endif
