#include "live_jpeg_source.h"
#include "convert.h"
#include <stdlib.h>
#include <string.h>

struct LiveJpegSource {
    unsigned width,format,valid,owned;
    uint64_t revision;
    uint8_t *reference;
    LiveJpegFrame *frame;
};
static uint32_t next_source_id;

size_t live_jpeg_source_storage_size(unsigned width) {
    size_t frame=live_jpeg_frame_size(width);
    return frame?((sizeof(LiveJpegSource)+7u)&~(size_t)7u)+width*240u*4u+frame:0;
}
LiveJpegSource *live_jpeg_source_init(void *memory,size_t bytes,unsigned width) {
    size_t need=live_jpeg_source_storage_size(width);
    if(!memory||((uintptr_t)memory&7u)||!need||bytes<need)return NULL;
    LiveJpegSource *s=memory;
    memset(s,0,sizeof *s);
    s->reference=(uint8_t*)memory+((sizeof *s+7u)&~(size_t)7u);
    s->frame=(LiveJpegFrame*)(s->reference+width*240u*4u);
    memset(s->frame,0,live_jpeg_frame_size(width));
    s->width=s->frame->width=width;
    s->frame->source_id=__atomic_add_fetch(&next_source_id,1,__ATOMIC_RELAXED);
    if(!s->frame->source_id)return NULL;
    return s;
}
LiveJpegSource *live_jpeg_source_create(unsigned width) {
    size_t bytes=live_jpeg_source_storage_size(width);
    if(!bytes)return NULL;
    void *memory=malloc(bytes);
    LiveJpegSource *s=live_jpeg_source_init(memory,bytes,width);
    if(s)s->owned=1;else free(memory);
    return s;
}
void live_jpeg_source_destroy(LiveJpegSource *s) {
    if(s&&s->owned)free(s);
}
const LiveJpegFrame *live_jpeg_source_frame(const LiveJpegSource *s) {
    return s&&s->valid?s->frame:NULL;
}
static inline signed char luma(const uint8_t *p,unsigned fmt,int *rs,int *gs,int *bs) {
    int r,g,b;
    if(fmt==FB_RGBA8){r=p[3];g=p[2];b=p[1];}
    else if(fmt==FB_BGR8){r=p[2];g=p[1];b=p[0];}
    else {
        unsigned v=p[0]|(unsigned)p[1]<<8;
        if(fmt==FB_RGB565) {
            /* Match the pinned JPEGENC 565 expansion exactly. */
            r=((v&0xf800)>>8)|((v&0x3800)>>11);
            g=((v&0x07e0)>>3)|((v&0x0060)>>5);
            b=((v&0x001f)<<3)|(v&7);
        } else if(fmt==FB_RGB5A1) {
            r=(v>>11)&31;g=(v>>6)&31;b=(v>>1)&31;
            r=(r<<3)|(r>>2);g=(g<<3)|(g>>2);b=(b<<3)|(b>>2);
        } else {r=((v>>12)&15)*17;g=((v>>8)&15)*17;b=((v>>4)&15)*17;}
    }
    *rs+=r;*gs+=g;*bs+=b;
    return (signed char)(((r*1225+g*2404+b*467)>>12)-128);
}
static inline void convert_mcu(const uint8_t *src,unsigned stride,unsigned fmt,
                               unsigned bpp,unsigned x0,unsigned y0,signed char *dst) {
    for(unsigned y=0;y<16;y+=2)for(unsigned x=0;x<16;x+=2) {
        const uint8_t *a=src+(x0+x)*stride+(239-y0-y)*bpp,*b=a+stride;
        unsigned yi=((y>>3)*2+(x>>3))*64+(y&7)*8+(x&7),ci=(y>>1)*8+(x>>1);
        int r=0,g=0,blue=0;
        dst[yi]=luma(a,fmt,&r,&g,&blue);dst[yi+1]=luma(b,fmt,&r,&g,&blue);
        dst[yi+8]=luma(a-bpp,fmt,&r,&g,&blue);dst[yi+9]=luma(b-bpp,fmt,&r,&g,&blue);
        dst[256+ci]=(signed char)(((blue<<11)-r*691-g*1357)>>14);
        dst[320+ci]=(signed char)(((r<<11)-g*1715-blue*333)>>14);
    }
}
/* 512 read-only bytes: each entry packs the expanded channel in bits 20..27
 * and its unrounded luma contribution in bits 0..19. The largest Y sum is
 * 255*4096 < 2^20, so adding three entries cannot carry into the channel bits.
 * Entry = ((i<<shift)|(i&mask))*((1u<<20)+weight), with (shift,mask,weight)
 * R=(3,7,1225), G=(2,3,2404), B=(3,7,467). Retain the pinned 565 expansion
 * and round only after all channels are added. */
static const uint32_t rgb565_y_r[32]={
    0,9448209,18896418,28344627,37792836,47241045,56689254,66137463,
    67187264,76635473,86083682,95531891,104980100,114428309,123876518,133324727,
    134374528,143822737,153270946,162719155,172167364,181615573,191063782,200511991,
    201561792,211010001,220458210,229906419,239354628,248802837,258251046,267699255
};
static const uint32_t rgb565_y_g[64]={
    0,5254900,10509800,15764700,16815680,22070580,27325480,32580380,
    33631360,38886260,44141160,49396060,50447040,55701940,60956840,66211740,
    67262720,72517620,77772520,83027420,84078400,89333300,94588200,99843100,
    100894080,106148980,111403880,116658780,117709760,122964660,128219560,133474460,
    134525440,139780340,145035240,150290140,151341120,156596020,161850920,167105820,
    168156800,173411700,178666600,183921500,184972480,190227380,195482280,200737180,
    201788160,207043060,212297960,217552860,218603840,223858740,229113640,234368540,
    235419520,240674420,245929320,251184220,252235200,257490100,262745000,267999900
};
static const uint32_t rgb565_y_b[32]={
    0,9441387,18882774,28324161,37765548,47206935,56648322,66089709,
    67138752,76580139,86021526,95462913,104904300,114345687,123787074,133228461,
    134277504,143718891,153160278,162601665,172043052,181484439,190925826,200367213,
    201416256,210857643,220299030,229740417,239181804,248623191,258064578,267505965
};
static inline signed char luma565(uint16_t v,int *rs,int *gs,int *bs) {
    uint32_t r=rgb565_y_r[v>>11],g=rgb565_y_g[(v>>5)&63],b=rgb565_y_b[v&31];
    *rs+=r>>20;*gs+=g>>20;*bs+=b>>20;
    return (signed char)((int)(((r+g+b)>>12)&255)-128);
}
static void convert565(const uint16_t *src,unsigned stride,unsigned x0,unsigned y0,signed char *dst) {
    for(unsigned y=0;y<16;y+=2)for(unsigned x=0;x<16;x+=2) {
        const uint16_t *a=src+(x0+x)*stride+239-y0-y,*b=a+stride;
        unsigned yi=((y>>3)*2+(x>>3))*64+(y&7)*8+(x&7),ci=(y>>1)*8+(x>>1);
        int r=0,g=0,blue=0;
        dst[yi]=luma565(a[0],&r,&g,&blue);dst[yi+1]=luma565(b[0],&r,&g,&blue);
        dst[yi+8]=luma565(a[-1],&r,&g,&blue);dst[yi+9]=luma565(b[-1],&r,&g,&blue);
        dst[256+ci]=(signed char)(((blue<<11)-r*691-g*1357)>>14);
        dst[320+ci]=(signed char)(((r<<11)-g*1715-blue*333)>>14);
    }
}
static int same_rgba(const uint8_t *a,const uint8_t *b,unsigned count) {
    uint32_t diff=0;
    for(unsigned i=0;i<count;i++) {
        uint32_t x,y;memcpy(&x,a+4*i,4);memcpy(&y,b+4*i,4);diff|=x^y;
    }
    return !(diff&0xffffff00u);
}
/* Exact comparisons against packed native columns. RGBA alpha is masked;
 * retaining word alignment is faster than comparing packed 3-byte references. */
static int same_tile(const uint8_t *ref,const uint8_t *src,unsigned stride,
                     unsigned bpp,unsigned cached_bpp,unsigned x,unsigned y) {
    for(unsigned col=0;col<16;col++) {
        const uint8_t *a=ref+((x+col)*240+224-y)*cached_bpp;
        const uint8_t *b=src+(x+col)*stride+(224-y)*bpp;
        if(bpp==4) {
            if(memcmp(a,b,64)&&!same_rgba(a,b,16))return 0;
        } else if(bpp==2?memcmp(a,b,32):memcmp(a,b,48))return 0;
    }
    return 1;
}
static void copy_tile(uint8_t *ref,const uint8_t *src,unsigned stride,
                      unsigned bpp,unsigned cached_bpp,unsigned x,unsigned y) {
    for(unsigned col=0;col<16;col++) {
        uint8_t *a=ref+((x+col)*240+224-y)*cached_bpp;
        const uint8_t *b=src+(x+col)*stride+(224-y)*bpp;
        if(bpp==4)memcpy(a,b,64);
        else if(bpp==2)memcpy(a,b,32);
        else memcpy(a,b,48);
    }
}
/* Keep the common aligned RGB565 loop free of per-tile format decisions.
 * Constant 32-byte column spans also let the compiler inline compare/copy. */
static int update565(LiveJpegSource *s,const uint8_t *src,unsigned stride,int reuse,uint64_t revision) {
    unsigned index=0;int changed=0;
    for(unsigned y=0;y<240;y+=16)for(unsigned x=0;x<s->width;x+=16,index++) {
        if(reuse&&same_tile(s->reference,src,stride,2,2,x,y))continue;
        copy_tile(s->reference,src,stride,2,2,x,y);
        convert565((const uint16_t*)src,stride/2,x,y,s->frame->mcus[index].samples);
        s->frame->mcus[index].revision=revision;changed++;
    }
    return changed;
}
int live_jpeg_source_update(LiveJpegSource *s,const void *pixels,size_t bytes,unsigned fmt,unsigned stride) {
    fmt&=7u;
    if(!s||!pixels||fmt>FB_RGBA4)return -1;
    unsigned bpp=fmt==FB_RGBA8?4:fmt==FB_BGR8?3:2,cbpp=bpp;
    /* A wide top LCD uses every second physical column, including its padding. */
    const unsigned maxStride = s->width == 400 ? 2u * 1920u : 1920u;
    if(stride<240*bpp||stride>maxStride||bytes<(size_t)(s->width-1)*stride+240*bpp||
       s->revision==UINT64_MAX)return -1;
    const uint8_t *src=pixels;
    int reuse=s->valid&&s->format==fmt;
    if(reuse&&stride==240*bpp&&!memcmp(s->reference,src,s->width*stride))return 0;
    uint64_t revision=++s->revision;
    if(fmt==FB_RGB565&&!(((uintptr_t)src|stride)&1u)) {
        int changed=update565(s,src,stride,reuse,revision);
        s->format=fmt;s->valid=1;
        return changed;
    }
    unsigned index=0;int changed=0;
    for(unsigned y=0;y<240;y+=16)for(unsigned x=0;x<s->width;x+=16,index++) {
        if(reuse&&same_tile(s->reference,src,stride,bpp,cbpp,x,y))continue;
        copy_tile(s->reference,src,stride,bpp,cbpp,x,y);
        signed char *dst=s->frame->mcus[index].samples;
        /* Constant format at each call lets GCC specialize the inner loop. */
        switch(fmt) {
        case FB_RGBA8: convert_mcu(src,stride,FB_RGBA8,4,x,y,dst);break;
        case FB_BGR8: convert_mcu(src,stride,FB_BGR8,3,x,y,dst);break;
        case FB_RGB565:
            convert_mcu(src,stride,FB_RGB565,2,x,y,dst);
            break;
        case FB_RGB5A1: convert_mcu(src,stride,FB_RGB5A1,2,x,y,dst);break;
        default: convert_mcu(src,stride,FB_RGBA4,2,x,y,dst);break;
        }
        s->frame->mcus[index].revision=revision;changed++;
    }
    s->format=fmt;s->valid=1;
    return changed;
}
