/* Adapter to the vendored Apache-2.0 JPEGENC sources. */
#define __LINUX__ 1
#include "../../third_party/JPEGENC/src/JPEGENC.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "../../third_party/JPEGENC/src/jpegenc.inl"
#pragma GCC diagnostic pop
#include "live_jpeg.h"
#include "live_jpeg_source.h"
#include <stdlib.h>
/* Upstream reserves 512 bytes ahead of each MCU. Separate physical headroom
 * from the network budget, otherwise a JPEG that fits can fail prematurely. */
typedef struct { JPEGE_IMAGE image; uint8_t staging[LIVE_JPEG_MAX_BYTES+1024]; } LiveJpegState;
static LiveJpegState states[2];
typedef struct {
    unsigned width;
    /* Keep the framebuffer in its native column-major layout. This makes an
     * unchanged frame one contiguous memcmp and avoids copying every MCU into
     * a temporary tile before discovering that it is unchanged. */
    /* Allocated only by the legacy RGB565 entry point, never by live YCbCr. */
    uint16_t *pixels;
    uint32_t source_id;
    uint64_t revisions[375];
    int16_t dct[375][6][64];
} LiveJpegCache;
static LiveJpegCache caches[2];
enum { JPEG_MCU_ROWS = 240 / 16 };
typedef struct {
    uint8_t jpeg[LIVE_JPEG_MAX_BYTES];
    uint32_t rowOffset[JPEG_MCU_ROWS+1];
    unsigned size,width,quality,dirtyRows;
} LiveJpegEntropy;
/* Keep only the last successful render, independent of the caller's buffers.
 * 256160 bytes across both contexts; no allocation in prepare/render. */
static LiveJpegEntropy entropy[2];
_Static_assert(sizeof entropy <= 256u*1024u,"JPEG row cache exceeds memory budget");
static int scaled_tables(LiveJpegState *state,unsigned quality) {
    JPEGE_IMAGE *image=&state->image;uint8_t *staging=state->staging;
    uint8_t tables[128];
    unsigned scale=quality<50?5000/quality:200-2*quality;
    for(unsigned i=0;i<128;i++) {
        unsigned value=((i<64?quant_lum[i]:quant_color[i-64])*scale+50)/100;
        tables[i]=(uint8_t)(value<1?1:value>255?255:value);
        image->sQuantTable[i]=tables[i];
    }
    /* Patch both on-wire DQT segments before entropy coding. The pinned library
     * stores quant_lum/quant_color in zigzag order, as required by JPEG DQT. */
    size_t end=(size_t)(image->pc.pOut-staging),at=2;unsigned found=0;
    while(at+4<=end && staging[at]==0xff) {
        unsigned marker=staging[at+1],len=(staging[at+2]<<8)|staging[at+3];
        if(len<2 || at+2+len>end)return 0;
        if(marker==0xda)break;
        if(marker==0xdb) {
            if(len!=67 || staging[at+4]>1)return 0;
            memcpy(staging+at+5,tables+64*staging[at+4],64);found++;
        }
        at+=2+len;
    }
    if(found!=2)return 0;
    JPEGFixQuantE(image);
    return 1;
}
static size_t encode(LiveJpegState *state,const uint16_t *src,unsigned w,unsigned h,unsigned quality,
                     unsigned scaled,uint8_t *out,size_t cap) {
    if(!src||!out||h!=240||(w!=400&&w!=320)||cap<1024||cap>LIVE_JPEG_MAX_BYTES)return 0;
    JPEGE_IMAGE *image=&state->image;uint8_t *staging=state->staging;
    memset(image,0,sizeof *image);
    image->pOutput=staging;image->iBufferSize=(int)cap+1024;image->pHighWater=staging+cap+512;
    JPEGENCODE enc;
    if(JPEGEncodeBegin(image,&enc,(int)w,(int)h,JPEGE_PIXEL_RGB565,JPEGE_SUBSAMPLE_420,(uint8_t)quality))return 0;
    if(scaled && !scaled_tables(state,scaled))return 0;
    _Alignas(4) uint16_t tile[256]={0};
    for(unsigned y=0;y<h;y+=16)for(unsigned x=0;x<w;x+=16) {
        for(unsigned dy=0;dy<16;dy++)for(unsigned dx=0;dx<16;dx++)tile[dy*16+dx]=src[(x+dx)*h+h-1-y-dy];
        if(JPEGAddMCU(image,&enc,(uint8_t*)tile,32))return 0;
    }
    int size=JPEGEncodeEnd(image);
    if(size<=0||(size_t)size>cap)return 0;
    memcpy(out,staging,(size_t)size);
    return (size_t)size;
}
size_t live_jpeg_encode(const uint16_t *src,unsigned w,unsigned h,unsigned quality,uint8_t *out,size_t cap) {
    if(quality>3)return 0;
    return encode(&states[0],src,w,h,quality,0,out,cap);
}
size_t live_jpeg_encode_scaled(const uint16_t *src,unsigned w,unsigned h,unsigned quality,uint8_t *out,size_t cap) {
    if(quality<1||quality>95)return 0;
    return encode(&states[0],src,w,h,JPEGE_Q_MED,quality,out,cap);
}
size_t live_jpeg_encode_screen(unsigned screen,const uint16_t *src,unsigned w,unsigned h,unsigned quality,
                              uint8_t *out,size_t cap) {
    if(screen>1||quality<1||quality>95)return 0;
    return encode(&states[screen],src,w,h,JPEGE_Q_MED,quality,out,cap);
}

void live_jpeg_reset(unsigned screen) {
    if(screen<2) {
        free(caches[screen].pixels);caches[screen].pixels=NULL;caches[screen].source_id=0;
        caches[screen].width=0;entropy[screen].size=0;
        entropy[screen].dirtyRows=(1u<<JPEG_MCU_ROWS)-1u;
    }
}
/* Exact RGB565 conversion for complete 16x16, 4:2:0 MCUs. Preserve the pinned
 * encoder's low-bit RGB expansion and its single signed chroma rounding step.
 * Chroma is linear before that step, so summing four pixels first reduces its
 * multiplications from 16 to 4 per 2x2 group (28 to 16 including unchanged Y).
 * Every intermediate fits signed 32 bits; no table or extra frame buffer. */
static inline signed char rgb565_luma_sum(uint16_t pixel,int *red,int *green,int *blue) {
    int r=((pixel&0xf800)>>8)|((pixel&0x3800)>>11);
    int g=((pixel&0x07e0)>>3)|((pixel&0x0060)>>5);
    int b=((pixel&0x001f)<<3)|(pixel&7);
    *red+=r;*green+=g;*blue+=b;
    return (signed char)(((r*1225+g*2404+b*467)>>12)-128);
}
static void rgb565_mcu_420(const uint16_t *tile,signed char *mcu) __attribute__((unused));
static void rgb565_mcu_420(const uint16_t *tile,signed char *mcu) {
    for(unsigned y=0;y<16;y+=2)for(unsigned x=0;x<16;x+=2) {
        unsigned source=y*16+x;
        unsigned luma=((y>>3)*2+(x>>3))*64+(y&7)*8+(x&7);
        unsigned chroma=(y>>1)*8+(x>>1);
        int r=0,g=0,b=0;
        mcu[luma]=rgb565_luma_sum(tile[source],&r,&g,&b);
        mcu[luma+1]=rgb565_luma_sum(tile[source+1],&r,&g,&b);
        mcu[luma+8]=rgb565_luma_sum(tile[source+16],&r,&g,&b);
        mcu[luma+9]=rgb565_luma_sum(tile[source+17],&r,&g,&b);
        mcu[256+chroma]=(signed char)(((b<<11)-r*691-g*1357)>>14);
        mcu[320+chroma]=(signed char)(((r<<11)-g*1715-b*333)>>14);
    }
}
static void rgb565_frame_mcu_420(const uint16_t *src,unsigned origin_x,unsigned origin_y,signed char *mcu) {
    for(unsigned y=0;y<16;y+=2)for(unsigned x=0;x<16;x+=2) {
        const uint16_t *column0=src+(origin_x+x)*240+239-origin_y-y;
        const uint16_t *column1=column0+240;
        unsigned luma=((y>>3)*2+(x>>3))*64+(y&7)*8+(x&7);
        unsigned chroma=(y>>1)*8+(x>>1);
        int r=0,g=0,b=0;
        mcu[luma]=rgb565_luma_sum(column0[0],&r,&g,&b);
        mcu[luma+1]=rgb565_luma_sum(column1[0],&r,&g,&b);
        mcu[luma+8]=rgb565_luma_sum(column0[-1],&r,&g,&b);
        mcu[luma+9]=rgb565_luma_sum(column1[-1],&r,&g,&b);
        mcu[256+chroma]=(signed char)(((b<<11)-r*691-g*1357)>>14);
        mcu[320+chroma]=(signed char)(((r<<11)-g*1715-b*333)>>14);
    }
}
static int same_mcu(const uint16_t *left,const uint16_t *right,unsigned x,unsigned y) {
    size_t offset=x*240u+240u-16u-y;
    for(unsigned column=0;column<16;column++,offset+=240u)
        if(memcmp(left+offset,right+offset,16u*sizeof *left))return 0;
    return 1;
}
static void copy_mcu(uint16_t *dest,const uint16_t *src,unsigned x,unsigned y) {
    size_t offset=x*240u+240u-16u-y;
    for(unsigned column=0;column<16;column++,offset+=240u)
        memcpy(dest+offset,src+offset,16u*sizeof *dest);
}
int live_jpeg_prepare(unsigned screen,const uint16_t *src,unsigned w,unsigned h) {
    if(screen>1||!src||h!=240||(w!=400&&w!=320))return -1;
    LiveJpegCache *cache=&caches[screen];
    if(!cache->pixels) {
        cache->pixels=malloc(400u*240u*sizeof *cache->pixels);
        if(!cache->pixels)return -1;
        cache->width=0;
    }
    if(cache->source_id){cache->width=0;cache->source_id=0;}
    JPEGE_IMAGE *image=&states[screen].image;
    image->ucPixelType=JPEGE_PIXEL_RGB565;
    if(cache->width==w&&!memcmp(cache->pixels,src,w*h*sizeof *src))return 0;
    unsigned index=0;int changed=0;
    for(unsigned y=0;y<h;y+=16)for(unsigned x=0;x<w;x+=16,index++) {
        if(cache->width==w&&same_mcu(cache->pixels,src,x,y))continue;
        /* Dirty state is relative to the last successful JPEG, not the last
         * prepare: failed renders and repeated prepares must not clear it. */
        entropy[screen].dirtyRows|=1u<<(y/16);
        copy_mcu(cache->pixels,src,x,y);
        rgb565_frame_mcu_420(src,x,y,image->MCUc);
        for(unsigned block=0;block<6;block++)JPEGFDCT(image->MCUc+64*block,cache->dct[index][block]);
        changed++;
    }
    cache->width=w;
    return changed;
}

int live_jpeg_prepare_yuv(unsigned screen,const LiveJpegFrame *frame,size_t bytes) {
    if(screen>1||!frame||bytes<sizeof *frame||!frame->source_id)return -1;
    size_t need=live_jpeg_frame_size(frame->width);
    if(!need||bytes<need)return -1;
    unsigned count=frame->width/16*15;
    /* Validate before touching DCT state, including partially initialized input. */
    for(unsigned i=0;i<count;i++)if(!frame->mcus[i].revision)return -1;
    LiveJpegCache *cache=&caches[screen];
    int reuse=cache->width==frame->width&&cache->source_id==frame->source_id,changed=0;
    free(cache->pixels);cache->pixels=NULL;
    for(unsigned i=0;i<count;i++) {
        if(reuse&&cache->revisions[i]==frame->mcus[i].revision)continue;
        entropy[screen].dirtyRows|=1u<<(i/(frame->width/16));
        for(unsigned block=0;block<6;block++)
            JPEGFDCT((signed char *)frame->mcus[i].samples+block*64,cache->dct[i][block]);
        cache->revisions[i]=frame->mcus[i].revision;changed++;
    }
    cache->source_id=frame->source_id;cache->width=frame->width;
    return changed;
}

/* JPEGENC's quantizer branches on the sign of every coefficient.  The DCT
 * range is safely below INT_MIN, so an arithmetic sign mask produces the same
 * rounded magnitude and sign without an unpredictable branch. */
static int live_jpeg_quantize(JPEGE_IMAGE *image,int16_t *coefficients,int chroma) {
    const int16_t *quant=image->sQuantTable+chroma*DCTSIZE;
    int nonzero=0;
    for(unsigned i=0;i<64;i++) {
        int value=coefficients[i];
        int sign=value>>31;
        int magnitude=(value^sign)-sign;
        int result=(((quant[i]>>1)+magnitude)*quant[i+128])>>16;
        result=(result^sign)-sign;
        coefficients[i]=(int16_t)result;
        if(i>=33)nonzero|=result;
    }
    return nonzero==0;
}

size_t live_jpeg_render(unsigned screen,unsigned quality,uint8_t *out,size_t cap) {
    if(screen>1||quality<1||quality>95||!out||cap<1024||cap>LIVE_JPEG_MAX_BYTES)return 0;
    LiveJpegCache *cache=&caches[screen];unsigned w=cache->width;
    if(!w)return 0;
    LiveJpegEntropy *previous=&entropy[screen];
    const int reuse=previous->size&&previous->width==w&&previous->quality==quality;
    if(reuse&&!previous->dirtyRows) {
        if(previous->size>cap)return 0;
        memcpy(out,previous->jpeg,previous->size);return previous->size;
    }
    LiveJpegState *state=&states[screen];JPEGE_IMAGE *im=&state->image;
    memset(im,0,sizeof *im);im->pOutput=state->staging;
    im->iBufferSize=(int)cap+1024;im->pHighWater=state->staging+cap+512;
    JPEGENCODE enc;
    if(JPEGEncodeBegin(im,&enc,w,240,JPEGE_PIXEL_RGB565,JPEGE_SUBSAMPLE_420,JPEGE_Q_MED)||
       !scaled_tables(state,quality))return 0;
    uint32_t rowOffset[JPEG_MCU_ROWS+1];
    unsigned index=0;
    for(unsigned row=0;row<JPEG_MCU_ROWS;row++) {
        rowOffset[row]=(uint32_t)(im->pc.pOut-state->staging);
        if(reuse&&!(previous->dirtyRows&(1u<<row))) {
            size_t length=previous->rowOffset[row+1]-previous->rowOffset[row];
            if(length>=(size_t)(im->pHighWater-im->pc.pOut))return 0;
            memcpy(im->pc.pOut,previous->jpeg+previous->rowOffset[row],length);
            im->pc.pOut+=length;index+=w/16;
            /* Cached bytes include this row's flush padding and RST marker.
             * FlushCode empties the accumulator; each row starts with zero DC. */
            im->pc.iLen=0;im->pc.ulAcc=0;im->iRestart++;
        } else {
            for(unsigned x=0;x<w;x+=16,index++) {
                for(unsigned block=0;block<6;block++) {
                    memcpy(im->MCUs,cache->dct[index][block],sizeof cache->dct[index][block]);
                    int chroma=block>=4;
                    int sparse=live_jpeg_quantize(im,im->MCUs,chroma);
                    if(block<4)im->iDCPred0=JPEGEncodeMCU(0,im,im->MCUs,im->iDCPred0,sparse);
                    else if(block==4)im->iDCPred1=JPEGEncodeMCU(1,im,im->MCUs,im->iDCPred1,sparse);
                    else im->iDCPred2=JPEGEncodeMCU(1,im,im->MCUs,im->iDCPred2,sparse);
                }
                if(im->pc.pOut>=im->pHighWater)return 0;
            }
            /* Match the pinned encoder's restart interval and bytes exactly. */
            FlushCode(&im->pc);*im->pc.pOut++=0xff;*im->pc.pOut++=(uint8_t)(0xd0+(im->iRestart++&7));
        }
        im->iDCPred0=im->iDCPred1=im->iDCPred2=0;
        if(im->pc.pOut>=im->pHighWater)return 0;
    }
    rowOffset[JPEG_MCU_ROWS]=(uint32_t)(im->pc.pOut-state->staging);
    im->iDataSize=(int)(im->pc.pOut-state->staging);
    int n=JPEGEncodeEnd(im);
    if(n<=0||(size_t)n>cap)return 0;
    /* Commit only after all bounds checks. An overflow retry can still reuse
     * clean rows from the earlier successful frame at the earlier quality. */
    memcpy(previous->jpeg,state->staging,(size_t)n);
    memcpy(previous->rowOffset,rowOffset,sizeof rowOffset);
    previous->size=(unsigned)n;previous->width=w;previous->quality=quality;previous->dirtyRows=0;
    memcpy(out,state->staging,(size_t)n);return (size_t)n;
}
