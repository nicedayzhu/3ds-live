#include "live_stream.h"
#include "buffers.h"
#include "capture.h"
#include "live_timing.h"
#include "pending_queue.h"
#include "packet_report.h"
#include "row_compression.h"
#include "live_jpeg.h"
#include "live_jpeg_source.h"
#include "live_jpeg_wire.h"
#include "live_jpeg_rows.h"
#include "live_row_refresh.h"
#include "live_pacing.h"
#include "live_pair.h"
#include "live_repair.h"
#include "live_audio_transport.h"
#include "live_discovery.h"
#include "live_receiver_feedback.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Encoder 0 coordinates pairs, encoder 1 handles bottom. The socket worker
 * owns the socket and claims queued pairs in capture order. */
#define MAX_JPEG LIVE_JPEG_MAX_BYTES
static LiveJpegSource *rawSources[2];
static LiveJpegFrame *rawWorking[2];
static size_t rawBytes[2];
static _Alignas(LZ4_stream_t) u8 encoded[2][MAX_JPEG];
static u8 pendingBytes[2*MAX_JPEG],sendArena[2*MAX_JPEG];
static LivePendingQueue pending;
static LiveRepairHistory repairHistory;
static u32 repairCredit,repairRequests,repairPackets,repairWireBytes;
static u64 repairInterest,repairPeerAt,lastRepairPoll,sendActiveTicks;
/* The socket worker can pick up a NACK inside a send slot. A different
 * control packet is deferred intact until the slot has finished. */
static struct {
    int size;
    struct sockaddr_in from;
    u8 bytes[64];
} deferredControl;
static bool rawValid[2],enabled,socReady,compactJpeg,audioEnabled,combinedFec;
static volatile bool stopping;
static u32 rawPts[2],planPts[2],planStamp,planMask;
static u32 sequence,session;
static u64 started;
static LightLock lock,rawLocks[2];
static LightEvent events[3];
static Thread worker,encoders[2];
static s32 encoderCores[2];
static int fd=-1,lastErrno;
#define SOC_ARENA_BYTES 0x100000u
static struct sockaddr_in target;
static LiveDiscovery discovery;
static LiveReceiverFeedback receiverFeedback;
static bool receiverVideo;
static bool automaticHost,mediaReady;
static bool capturePaused;
static u64 lastProbeTick,lastPeerControl;
static u32 localAddress,localMask;
static struct {
    u32 polls,probeAttempts,probesSent,replies,accepted,hints,hintsAnswered,reportedBroadcast,lastDestination;
    int infoResult,infoErrno,socketErrno,bindErrno,sendErrno,receiveErrno;
    unsigned localPort;
    u64 savedAt,lastHintTick;
} netDiag;
static u32 fps=30,rate=800000,configEpoch,configSequence;
static unsigned qualityTarget;
static unsigned quality=60,spare;
static LivePayloadControl payloadControl;
static u32 jobKind,jobQuality,jobDone,jobSize[2],jobChanged[2],jobUs[2];
/* The bottom screen is mostly a map/status panel. Encoding it at 8 fps and a
 * lower quality leaves the top screen enough CPU to keep 30 fps while the game
 * is running. The previous JPEG is re-sent on skipped frames, so receivers need
 * no protocol change; at Q25 a reused bottom costs only a few KB. */
#define BOTTOM_FPS 8u
#define BOTTOM_REUSE_BYTES 24576u
static u8 bottomReuse[BOTTOM_REUSE_BYTES];
static u32 bottomReuseSize;
/* Bit 0 = top screen, bit 1 = bottom screen. A single-screen mask skips the
 * hidden encoder entirely and sends per-screen packets, so both ARM11 time and
 * airtime are released instead of rendering a panel nobody displays. */
static u32 screenMask=3;
static u32 attempts[2],jpegFrames[2],maxUs[2],maxBytes[2],convertCount[2],convertMaxUs[2];
static u64 sumUs[2],convertSumUs[2],changedMcus[2];
static u64 convertedMcus[2],snapshotSumUs;
static u32 snapshotCount,snapshotMaxUs;
static u32 sourceReuse[2],lastSource[2],frames[2],replaced,errors,expired,oversize,retries,pairs;
static u32 sendCalls,sendMaxUs,lastEncodeUs[2],feedbackCount,configChanges;
/* Socket-thread owned. Mode 0 is the original submission-start byte clock. */
static unsigned pacingMode=1;
static LivePacketReport packetReport;
static u32 ratePendingKept,rateEncodingKept;
static u64 sendSumUs,lastStatusTick,lastControlTick,videoWireBytes;
static u64 compactSavedBytes;
static u32 compactFrames,compactFallbacks;
/* Each optional reference bank holds up to 180 KiB. Larger pairs can still
 * travel as independent JPEGs within the per-screen and receive limits. */
#define ROW_PAIR_BYTES (180u * 1024u)
typedef struct {
    u32 seq,stamp,length[2];
    LiveJpegRows rows[2];
    _Alignas(LZ4_stream_t) u8 bytes[ROW_PAIR_BYTES];
} RowAnchor;
typedef struct {
    u32 compact[2],wire[2],trialSize,bottomDeltaBytes;
    const u8 *bottomDelta,*bottomCompressed;
    LiveJpegRows rows[2];
    bool valid;
} RowPairPlan;
static RowAnchor *rowBanks,*rowBase,*rowCandidate,*rowProbe;
/* Only the encoder builds/reuses this dictionary. Acknowledging a new
 * reference changes rowBase, but never modifies either bank's bytes. */
static const RowAnchor *r20Prepared;
static LZ4_stream_t *r20Dictionary;
static u64 r20LastAck,r20EncodeUs,r20LastAttempt,r20Saved;
static u32 r20Attempts,r20Panels,r20MaxUs,r20MeanUs,r20Skipped;
_Static_assert(MAX_JPEG%_Alignof(LZ4_stream_t)==0 && ROW_PAIR_BYTES%_Alignof(LZ4_stream_t)==0,
               "LZ4 workspaces at arena ends must be aligned");
static LiveRowTrial rowTrial;
static u64 probeAfter;
static bool rowsEnabled;
static unsigned rowSends;
static u64 rowLastKey,rowLastAck,rowBytesSaved;
static u32 rowKeys,rowRepeats,rowDeltas,rowAcks;
static u32 manualRenderFailures,manualWireDrops;
static LiveAudioQueue audioQueue;
static LiveAudioState audioCodec;
static LiveAudioHistory audioHistory;
static u64 audioEncodeSumUs,audioWireBytes;
static u32 audioPacketSequence,audioPacketsSent,audioSendErrors,audioEncodedBlocks,audioEncodeMaxUs;
static u32 get32(const u8 *p){return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24;}
static void put32(u8 *p,u32 n){for(unsigned i=0;i<4;i++)p[i]=(u8)(n>>(8*i));}
static void put16(u8 *p,u16 n){p[0]=(u8)n;p[1]=(u8)(n>>8);}
static bool stopped(void){return __atomic_load_n(&stopping,__ATOMIC_ACQUIRE);}
static bool capture_paused(void){return __atomic_load_n(&capturePaused,__ATOMIC_ACQUIRE);}

static void discard_pending(unsigned slot,LiveTimingOutcome why) {
    (void)why;
    live_timing_drop_pending(slot,why);
    live_pending_remove(&pending,slot);
}

static void clear_pending(void) {
    while(pending.count)discard_pending(pending.head,TIMING_RECONFIGURED);
}

void live_set_capture_paused(bool paused) {
    LightLock_Lock(&lock);
    if(paused&&!capture_paused()) {
        // Reject a JPEG still being encoded and forget the previous display plan.
        ++configEpoch;
        planMask=0;
        clear_pending();
    }
    __atomic_store_n(&capturePaused,paused,__ATOMIC_RELEASE);
    LightLock_Unlock(&lock);
}
bool live_requested(void) {
    if(worker)return enabled;
    enabled=true;compactJpeg=true;audioEnabled=true;combinedFec=true;fps=30;rate=800000;
    FILE *f=fopen("sdmc:/3ds/3ds-live/live.ini","r");
    char line[128],ip[64]="auto";unsigned port=8001;
    while(f&&fgets(line,sizeof line,f)) {
        char key[40],val[64];if(sscanf(line,"%39[^=]=%63s",key,val)!=2)continue;
        if(!strcmp(key,"enabled"))enabled=!strcmp(val,"on");
        else if(!strcmp(key,"host"))snprintf(ip,sizeof ip,"%s",val);
        else if(!strcmp(key,"port"))port=live_config_port(val);
        else if(!strcmp(key,"fps"))fps=live_migrate_fps((u32)strtoul(val,NULL,10));
        else if(!strcmp(key,"bytes_per_second"))rate=(u32)strtoul(val,NULL,10);
        else if(!strcmp(key,"compact_jpeg"))compactJpeg=!strcmp(val,"on");
        else if(!strcmp(key,"audio"))audioEnabled=strcmp(val,"off")!=0;
        else if(!strcmp(key,"combined_fec"))combinedFec=!strcmp(val,"on");
    }
    if(f)fclose(f);
    memset(&target,0,sizeof target);target.sin_family=AF_INET;
    automaticHost=!strcmp(ip,"auto");
    if(!automaticHost&&(inet_pton(AF_INET,ip,&target.sin_addr)!=1||
       !live_unicast_address(ntohl(target.sin_addr.s_addr))))enabled=false;
    if(port<1||port>65535)enabled=false;
    target.sin_port=htons((u16)port);
    if(!live_valid_fps(fps)||rate<LIVE_MIN_RATE||rate>LIVE_MAX_RATE)enabled=false;
    return enabled;
}
u32 live_fps(void){return __atomic_load_n(&fps,__ATOMIC_RELAXED);}
bool live_peer_ready(void){return !stopped()&&__atomic_load_n(&mediaReady,__ATOMIC_ACQUIRE);}
bool live_receiver_has_video(void){return !stopped()&&__atomic_load_n(&receiverVideo,__ATOMIC_ACQUIRE);}
u32 live_encoded_frames(unsigned sc){return sc<2?__atomic_load_n(&jpegFrames[sc],__ATOMIC_RELAXED):0;}
bool live_audio_active(void){return audioEnabled&&worker&&!stopped()&&!capture_paused();}
bool live_audio_submit(const s16 *pcm,u32 pts) {
    if(!pcm||!live_audio_active()||!live_peer_ready())return false;
    return live_audio_queue_push(&audioQueue,pcm,pts,svcGetSystemTick());
}
static void encode_job(unsigned sc,unsigned kind,unsigned q) {
    u64 begin=svcGetSystemTick();
    if(kind==1) {
        int changed=live_jpeg_prepare_yuv(sc,rawWorking[sc],rawBytes[sc]);
        if(changed<0){jobChanged[sc]=0;jobSize[sc]=0;return;}
        jobChanged[sc]=(u32)changed;
    }
    jobSize[sc]=(u32)live_jpeg_render(sc,q,encoded[sc],MAX_JPEG);
    jobUs[sc]+=(u32)((svcGetSystemTick()-begin)*1000000/SYSCLOCK_ARM11);
}
static bool run_single_job(unsigned kind,unsigned q,unsigned sc) {
    jobKind=kind;jobQuality=q;
    if(sc==0) { encode_job(0,kind,q); return !stopped(); }
    __atomic_store_n(&jobDone,0,__ATOMIC_RELEASE);LightEvent_Signal(&events[1]);
    while(!__atomic_load_n(&jobDone,__ATOMIC_ACQUIRE)&&!stopped())LightEvent_WaitTimeout(&events[2],1000000);
    return !stopped();
}

static bool run_pair_job(unsigned kind,unsigned q,bool bottom) {
    jobKind=kind;jobQuality=q;
    if(!bottom) {
        /* Bottom is not due: reuse its last JPEG and spend the time on top. */
        memcpy(encoded[1],bottomReuse,bottomReuseSize);
        jobSize[1]=bottomReuseSize;jobUs[1]=0;jobChanged[1]=0;
        encode_job(0,kind,q);
        return !stopped();
    }
    __atomic_store_n(&jobDone,0,__ATOMIC_RELEASE);LightEvent_Signal(&events[1]);
    encode_job(0,kind,q);
    while(!__atomic_load_n(&jobDone,__ATOMIC_ACQUIRE)&&!stopped())LightEvent_WaitTimeout(&events[2],1000000);
    if(jobSize[1]&&jobSize[1]<=BOTTOM_REUSE_BYTES) {
        memcpy(bottomReuse,encoded[1],jobSize[1]);
        bottomReuseSize=jobSize[1];
    } else {
        bottomReuseSize=0;
    }
    return !stopped();
}
static u32 payload_size(unsigned screen,unsigned q,bool combined) {
    u32 size=jobSize[screen];
    /* Budget exactly what packing will send, including a full-JPEG fallback. */
    if(combined&&compactJpeg&&live_jpeg_wire_size(size)&&
       encoded[screen][size-2]==0xff&&encoded[screen][size-1]==0xd9&&
       live_jpeg_wire_prefix_valid(encoded[screen],screen?320:400,q))
        return (u32)live_jpeg_wire_size(size);
    return size?size:MAX_JPEG+1;
}
static u32 pack_panel(unsigned sc,unsigned q,u8 *out,u32 capacity) {
    size_t packed=compactJpeg?live_jpeg_wire_pack(out,capacity,encoded[sc],
        jobSize[sc],sc?320:400,q):0;
    if(packed){compactFrames++;compactSavedBytes+=jobSize[sc]-packed;return (u32)packed;}
    memcpy(out,encoded[sc],jobSize[sc]);
    if(compactJpeg)compactFallbacks++;
    return jobSize[sc];
}

static const u8 *row_panel(const RowAnchor *ref,unsigned sc) {
    return ref->bytes+(sc?ref->length[0]:0);
}

static u32 row_size(const RowPairPlan *plan,unsigned sc,const RowAnchor *ref) {
    size_t delta=live_jpeg_rows_pack(NULL,0,encoded[sc],plan->compact[sc],&plan->rows[sc],
                    row_panel(ref,sc),ref->length[sc],&ref->rows[sc],ref->seq);
    return delta?(u32)delta:plan->compact[sc];
}

static bool r20_recent(u64 now) {
    return r20LastAck&&now>=r20LastAck&&now-r20LastAck<(u64)LIVE_PEER_LEASE_SECONDS*SYSCLOCK_ARM11;
}

/* Two dedicated encoder output arrays and the active reference bank have
 * unused tails for ordinary frames. Borrow those tails only when all three
 * live payloads fit. Large pairs retain the full original buffer capacity. */
static void compress_bottom(RowPairPlan *plan,RowAnchor *base,u64 nextEncode) {
    u32 len=plan->wire[1],compact=plan->compact[1];
    if(!base||len>=compact||len<=LIVE_JPEG_DELTA_HEADER+1||
       base->length[0]+base->length[1]>ROW_PAIR_BYTES-sizeof(LZ4_stream_t)||
       compact+len>MAX_JPEG-sizeof(LZ4_stream_t)||MAX_JPEG-plan->compact[0]<=20)return;
    u64 began=svcGetSystemTick();
    if(!live_r20_time_available(began,nextEncode,SYSCLOCK_ARM11,r20LastAttempt,r20MeanUs)) {
        r20Skipped++;
        return;
    }
    u8 *delta=encoded[1]+compact;
    if(live_jpeg_rows_pack(delta,len,encoded[1],compact,&plan->rows[1],row_panel(base,1),
                          base->length[1],&base->rows[1],base->seq)!=len)return;
    plan->bottomDelta=delta;
    plan->bottomDeltaBytes=len;
    if(r20Prepared!=base) {
        u8 *bankTail=base->bytes+ROW_PAIR_BYTES-sizeof(LZ4_stream_t);
        r20Dictionary=live_r20_dictionary(bankTail,sizeof(LZ4_stream_t),row_panel(base,1),base->length[1]);
        r20Prepared=base;
    }
    if(!r20Dictionary)return;
    u8 *out=encoded[0]+plan->compact[0];
    LZ4_stream_t *work=(LZ4_stream_t *)(encoded[1]+MAX_JPEG-sizeof(LZ4_stream_t));
    size_t bytes=live_r20_pack(out,MAX_JPEG-plan->compact[0],delta,len,base->seq,r20Dictionary,work);
    u32 us=(u32)((svcGetSystemTick()-began)*1000000/SYSCLOCK_ARM11);
    r20MeanUs=r20LastAttempt?(u32)((3ull*r20MeanUs+us+2)/4):us;
    r20LastAttempt=began;
    r20Attempts++;r20EncodeUs+=us;if(us>r20MaxUs)r20MaxUs=us;
    if(bytes){plan->bottomCompressed=out;plan->wire[1]=(u32)bytes;}
}

/* Only the encoder writes the two anchor banks, at publication. The network
 * thread can acknowledge a bank during an encode but cannot change its bytes. */
static void measure_pair(RowPairPlan *plan,unsigned q,bool combined,bool useRows,
                         const RowAnchor *base,const RowAnchor *probe,bool bottomRows) {
    *plan=(RowPairPlan){.valid=useRows};
    for(unsigned sc=0;sc<2;sc++) {
        unsigned qs=q;
        if(!useRows) {
            plan->wire[sc]=payload_size(sc,q,combined);
            continue;
        }
        plan->compact[sc]=live_jpeg_wire_pack(encoded[sc],MAX_JPEG,encoded[sc],jobSize[sc],sc?320:400,qs);
        plan->wire[sc]=plan->compact[sc]?plan->compact[sc]:(jobSize[sc]?jobSize[sc]:MAX_JPEG+1);
        if(!plan->compact[sc]||!live_jpeg_rows_parse(&plan->rows[sc],encoded[sc],plan->compact[sc]))
            plan->valid=false;
    }
    if(plan->valid)for(unsigned sc=0;sc<2;sc++) {
        /* A decimated bottom must be byte-identical between due frames so the PC
         * can skip decoding it; row deltas would carry a new sequence each pair. */
        if(sc==1&&!bottomRows)continue;
        if(base)plan->wire[sc]=row_size(plan,sc,base);
        if(probe)plan->trialSize+=row_size(plan,sc,probe);
    }
}

static void store_row_anchor(RowAnchor *ref,const RowPairPlan *plan,u32 seq,u32 stamp) {
    if(r20Prepared==ref){r20Prepared=NULL;r20Dictionary=NULL;}
    ref->seq=seq;ref->stamp=stamp;
    for(unsigned sc=0;sc<2;sc++) {
        ref->length[sc]=plan->compact[sc];
        ref->rows[sc]=plan->rows[sc];
        memcpy(ref->bytes+(sc?ref->length[0]:0),encoded[sc],ref->length[sc]);
    }
}

/* Called under the publication lock. A probe uses the existing spare bank
 * and never goes on the network. The acknowledged base remains immutable
 * until the PC acknowledges a real candidate. */
static bool needs_key(const RowPairPlan *plan,u64 now,u32 seq,u32 stamp,u32 budget) {
    u32 full=plan->compact[0]+plan->compact[1];
    if(!plan->valid||full>ROW_PAIR_BYTES||full>budget)return false;
    if(rowCandidate&&now-rowLastKey<SYSCLOCK_ARM11/2)return false;
    if(!rowBase)return true; /* Bootstrap and explicit receiver reset. */
    if(now-rowLastKey<SYSCLOCK_ARM11||now<probeAfter)return false;
    u32 wire=plan->wire[0]+plan->wire[1];
    if(!rowProbe) {
        /* Two chunks or less of remaining JPEG offer little to reclaim. */
        if(wire<=2*LIVE_CHUNK)return false;
        rowCandidate=NULL; /* The previous unacknowledged offer has expired. */
        rowProbe=rowBase==rowBanks?rowBanks+1:rowBanks;
        store_row_anchor(rowProbe,plan,seq,stamp);
        rowTrial=(LiveRowTrial){0};
        return false;
    }
    if(!plan->trialSize)return false;
    int decision=live_row_trial_observe(&rowTrial,live_combined_wire(wire),
                        live_combined_wire(plan->trialSize),live_combined_wire(full));
    if(!decision)return false;
    rowProbe=NULL;
    probeAfter=now+SYSCLOCK_ARM11/4;
    return decision>0;
}

/* Called with lock held. Full keys are eligible only when the complete wire
 * representation also fits the requested budget. Stale candidates are
 * replaced with a fresh capture rather than freezing the stream for an ACK. */
static void publish_rows(const RowPairPlan *plan,const RowAnchor *base,LivePendingPair *pair,bool key,u64 now) {
    if(key) {
        rowCandidate=rowBase==rowBanks?rowBanks+1:rowBanks;
        store_row_anchor(rowCandidate,plan,pair->seq,pair->stamp);
        for(unsigned sc=0;sc<2;sc++)
            memcpy(rowCandidate->bytes+(sc?rowCandidate->length[0]:0),"R17K",4);
        rowSends=0;rowLastKey=now;rowKeys++;
    }
    for(unsigned sc=0;sc<2;sc++) {
        u8 *out=pendingBytes+pair->offset+(sc?pair->length[0]:0);
        if(plan->compact[sc]){compactFrames++;compactSavedBytes+=jobSize[sc]-plan->compact[sc];}
        else compactFallbacks++;
        if(key) {
            memcpy(out,row_panel(rowCandidate,sc),pair->length[sc]);
        } else if(sc==1&&plan->bottomDelta) {
            memcpy(out,plan->bottomCompressed?plan->bottomCompressed:plan->bottomDelta,pair->length[sc]);
            rowDeltas++;
            rowBytesSaved+=plan->compact[sc]-plan->bottomDeltaBytes;
            if(plan->bottomCompressed){r20Panels++;r20Saved+=plan->bottomDeltaBytes-pair->length[sc];}
        } else if(base&&plan->compact[sc]&&plan->wire[sc]<plan->compact[sc]) {
            pair->length[sc]=live_jpeg_rows_pack(out,pair->length[sc],encoded[sc],plan->compact[sc],&plan->rows[sc],
                row_panel(base,sc),base->length[sc],&base->rows[sc],base->seq);
            rowDeltas++;
            rowBytesSaved+=plan->compact[sc]-pair->length[sc];
        } else {
            memcpy(out,encoded[sc],pair->length[sc]);
        }
    }
}

static u32 pair_payload_budget(u32 byteRate,u32 hz,bool combined,unsigned targetQ) {
    u32 videoRate=live_av_video_rate(byteRate,audioEnabled);
    return live_quality_budget(videoRate,hz,combined,targetQ);
}

/* Called by the network thread with lock held, between frame sends. A byte
 * ceiling changes the allowable JPEG size, not the meaning of its fragments. */
static void configure_stream(u32 hz,u32 bps,bool fec,unsigned targetQ) {
    bool formatChanged=fps!=hz||combinedFec!=fec||qualityTarget!=targetQ;
    if(!formatChanged&&rate==bps)return;
    if(formatChanged) {
        configEpoch++;
        rowBase=rowCandidate=rowProbe=NULL;
        live_repair_clear(&repairHistory);
        repairCredit=0;
    }
    u32 budget=pair_payload_budget(bps,hz,fec,targetQ);
    unsigned count=pending.count,head=pending.head;
    for(unsigned i=0;i<count;i++) {
        unsigned slot=head^i;
        if(!live_pending_compatible(&pending.slots[slot],configEpoch,budget))
            discard_pending(slot,TIMING_RECONFIGURED);
        else ratePendingKept++;
    }
    __atomic_store_n(&fps,hz,__ATOMIC_RELAXED);
    rate=bps;combinedFec=fec;qualityTarget=targetQ;configChanges++;
    /* A new quality/rate invalidates the reused bottom JPEG. */
    bottomReuseSize=0;
}

static void encoder(void *arg) {
    if((uintptr_t)arg) {
        while(!stopped()) {
            LightEvent_WaitTimeout(&events[1],100000000);
            if(stopped())break;
            if(__atomic_load_n(&jobDone,__ATOMIC_ACQUIRE))continue;
            encode_job(1,jobKind,jobQuality);
            __atomic_store_n(&jobDone,1,__ATOMIC_RELEASE);LightEvent_Signal(&events[2]);
        }
        return;
    }
    u64 next=svcGetSystemTick();
    LiveConsumedPlan consumed={0};
    u32 budgetEpoch=0,budgetRate=0;
    unsigned previousTarget=0;
    while(!stopped()) {
        if(!live_peer_ready()||capture_paused()){svcSleepThread(10000000);continue;}
        u64 now=svcGetSystemTick();
        if(now<next){svcSleepThread((next-now)*1000000000ull/SYSCLOCK_ARM11);continue;}
        bool ready;u32 stamp=0,source[2]={0,0},wanted[2],jobRate,jobFps,epoch;
        LightLock_Lock(&lock);
        stamp=planStamp;ready=planMask==3&&live_plan_available(&consumed,stamp);
        wanted[0]=planPts[0];wanted[1]=planPts[1];
        jobRate=rate;jobFps=fps;epoch=configEpoch;
        bool combined=combinedFec&&screenMask==3;
        unsigned targetQ=qualityTarget;
        bool useRows=rowsEnabled&&combined&&compactJpeg;
        bool bottomRows=combined;
        bool useR20=useRows&&bottomRows&&r20_recent(now);
        RowAnchor *base=useRows?rowBase:NULL;
        RowAnchor *probe=useRows?rowProbe:NULL;
        LightLock_Unlock(&lock);
        if(stopped())break;
        /* The capture thread converts directly into each pending buffer.  A
         * per-screen lock keeps that relatively long operation off the socket
         * and configuration lock while preserving one complete pair here. */
        u64 snapshotBegin=svcGetSystemTick();
        for(unsigned i=0;ready&&i<2;i++) {
            LightLock_Lock(&rawLocks[i]);
            if(!rawValid[i]||rawPts[i]!=wanted[i])ready=false;
            else {memcpy(rawWorking[i],live_jpeg_source_frame(rawSources[i]),rawBytes[i]);source[i]=rawPts[i];}
            LightLock_Unlock(&rawLocks[i]);
        }
        if(!ready){LightEvent_WaitTimeout(&events[0],1000000);continue;}
        live_plan_consume(&consumed,stamp);
        u32 snapshotUs=(u32)((svcGetSystemTick()-snapshotBegin)*1000000/SYSCLOCK_ARM11);
        snapshotCount++;snapshotSumUs+=snapshotUs;if(snapshotUs>snapshotMaxUs)snapshotMaxUs=snapshotUs;
        next=live_next_encode_tick(next,now,SYSCLOCK_ARM11,jobFps);
        const u32 initialRate=jobRate;
        u32 budget=pair_payload_budget(jobRate,jobFps,combined,targetQ);
        if(budgetEpoch!=epoch||budgetRate!=jobRate) {
            payloadControl=(LivePayloadControl){.percent=100};
            spare=0;
            budgetEpoch=epoch;
            budgetRate=jobRate;
            if(targetQ)quality=targetQ;
            else if(previousTarget)quality=60;
            previousTarget=targetQ;
        }
        u32 targetBytes=budget*payloadControl.percent/100;
        for(unsigned i=0;i<2;i++){sourceReuse[i]+=pairs&&source[i]==lastSource[i];lastSource[i]=source[i];jobUs[i]=0;}
        pairs++;u32 seq=++sequence;unsigned q=targetQ?targetQ:quality;
        live_timing_begin(seq,stamp);
        if(screenMask!=3) {
            const unsigned sc=(screenMask&1)?0:1;
            if(!run_single_job(1,q,sc))break;
            live_timing_encoded();
            const size_t packed=compactJpeg?
                live_jpeg_wire_pack(encoded[sc],MAX_JPEG,encoded[sc],jobSize[sc],sc?320:400,q):0;
            const u32 wire=packed?(u32)packed:jobSize[sc];
            if(!jobSize[sc]||wire>budget) { oversize++; continue; }
            LivePendingPair incoming={.length={sc==0?wire:0,sc==1?wire:0},
                                      .seq=seq,.stamp=stamp,.epoch=epoch,.r20=false};
            u32 bytes=live_pending_bytes(&incoming);
            while(!live_pending_fits(&pending,bytes,sizeof pendingBytes)) {
                discard_pending(pending.head,TIMING_REPLACED);
                replaced++;
            }
            unsigned slot=live_pending_append(&pending,pendingBytes,sizeof pendingBytes,incoming);
            LivePendingPair *pair=&pending.slots[slot];
            memcpy(pendingBytes+pair->offset,encoded[sc],wire);
            continue;
        }
        const bool bottom=true;
        if(!run_pair_job(1,q,bottom))break;
        live_timing_encoded();
        RowPairPlan plan;
        /* Bounded requantization: reuse DCT, at most 3 entropy passes. */
        for(unsigned retry=0;;retry++) {
            /* A rate cut may arrive during the DCT. Reuse it for the normal
             * bounded entropy retries instead of abandoning the frame. */
            LightLock_Lock(&lock);
            u32 currentRate=rate;
            LightLock_Unlock(&lock);
            if(currentRate!=jobRate) {
                jobRate=currentRate;
                budget=pair_payload_budget(jobRate,jobFps,combined,targetQ);
                targetBytes=budget*payloadControl.percent/100;
            }
            measure_pair(&plan,q,combined||targetQ,useRows,base,probe,bottomRows);
            if(useR20&&plan.valid)compress_bottom(&plan,base,next);
            u32 total=plan.wire[0]+plan.wire[1];
            if(jobSize[0]&&jobSize[1]&&total<=budget)break;
            unsigned nextQ=live_retry_quality(targetQ,q,retry,total,budget);
            if(nextQ==q||capture_paused())break;
            retries++;
            q=nextQ;
            if(!run_pair_job(2,q,bottom))break;
            live_timing_encoded();
        }
        if(stopped())break;
        u32 payloadTotal=plan.wire[0]+plan.wire[1];
        bool fits=jobSize[0]&&jobSize[1]&&payloadTotal<=budget;
        for(unsigned i=0;i<2;i++) {
            attempts[i]++;sumUs[i]+=jobUs[i];changedMcus[i]+=jobChanged[i];
            if(jobUs[i]>maxUs[i])maxUs[i]=jobUs[i];
            __atomic_store_n(&lastEncodeUs[i],jobUs[i],__ATOMIC_RELAXED);
            if(fits&&(i==0||bottom)){__atomic_add_fetch(&jpegFrames[i],1,__ATOMIC_RELAXED);if(jobSize[i]>maxBytes[i])maxBytes[i]=jobSize[i];}
        }
        quality=q;
        if(!fits) {
            if(targetQ) {
                if(!jobSize[0]||!jobSize[1])manualRenderFailures++;
                else manualWireDrops++;
            }
            live_timing_drop_encoded(TIMING_OVERSIZE);oversize++;spare=0;continue;
        }
        if(!targetQ) {
            if(payloadTotal>targetBytes){quality=live_lower_quality(q,payloadTotal,targetBytes);spare=0;}
            else if(payloadTotal<targetBytes*9/10){if(++spare>=4){quality=quality<83?quality+2:85;spare=0;}}
            else spare=0;
        }
        LightLock_Lock(&lock);
        u32 currentBudget=pair_payload_budget(rate,jobFps,combined,targetQ);
        if(plan.bottomCompressed&&!r20_recent(svcGetSystemTick())) {
            plan.bottomCompressed=NULL;
            plan.wire[1]=plan.bottomDeltaBytes;
            payloadTotal=plan.wire[0]+plan.wire[1];
        }
        if(epoch==configEpoch&&(!base||base==rowBase)&&payloadTotal<=currentBudget&&payloadTotal<=sizeof pendingBytes) {
            if(initialRate!=rate)rateEncodingKept++;
            u64 now=svcGetSystemTick();
            bool key=useRows&&needs_key(&plan,now,seq,stamp,currentBudget);
            LivePendingPair incoming={.length={key?plan.compact[0]:plan.wire[0],key?plan.compact[1]:plan.wire[1]},
                                      .seq=seq,.stamp=stamp,.epoch=epoch,.r20=!key&&plan.bottomCompressed};
            u32 bytes=live_pending_bytes(&incoming);
            bool overflow=!live_pending_fits(&pending,bytes,sizeof pendingBytes);
            while(!live_pending_fits(&pending,bytes,sizeof pendingBytes)) {
                discard_pending(pending.head,TIMING_REPLACED);
                replaced++;
            }
            live_payload_observe(&payloadControl,jobFps,overflow);
            unsigned slot=live_pending_append(&pending,pendingBytes,sizeof pendingBytes,incoming);
            LivePendingPair *pair=&pending.slots[slot];
            if(useRows)publish_rows(&plan,base,pair,key,now);
            else {
                u8 *out=pendingBytes+pair->offset;
                for(unsigned i=0;i<2;i++) {
                    pair->length[i]=pack_panel(i,q,out,pair->length[i]);
                    out+=pair->length[i];
                }
            }
            pair->queued=svcGetSystemTick();
            live_timing_publish(slot,pair->queued);
        } else live_timing_drop_encoded(TIMING_RECONFIGURED);
        LightLock_Unlock(&lock);
    }
}
static void pacing_sleep_until(u64 due) {
    u64 start=svcGetSystemTick();
    while(start<due) {
        svcSleepThread(((due-start)*1000000000ull+SYSCLOCK_ARM11-1)/SYSCLOCK_ARM11);
        u64 end=svcGetSystemTick();
        live_packet_observe_sleep(&packetReport,start,due,end);
        start=end;
    }
}

static void pacing_charge(u64 *due,u64 begin,u64 returned,u32 wire,enum LivePacketKind kind) {
    u64 base=begin>*due?begin:*due;
    u64 byteDue=live_packet_due(*due,begin,wire,rate,SYSCLOCK_ARM11);
    *due=live_packet_return_due(byteDue,returned,byteDue-base,SYSCLOCK_ARM11,pacingMode);
    live_packet_observe(&packetReport,kind,begin,returned,*due-byteDue,SYSCLOCK_ARM11);
}

/* Only the network thread calls this. Audio is serviced between video
 * datagrams, not after a whole 150 ms video send. Previous-batch redundancy
 * repairs one lost audio datagram without a retransmission round trip. */
static void send_audio(u64 *due) {
    if(!audioEnabled||stopped())return;
    u64 now=svcGetSystemTick(),oldest;
    if(!live_audio_queue_peek_tick(&audioQueue,&oldest))return;
    if(live_audio_queue_depth(&audioQueue)<LIVE_AUDIO_BATCH_BLOCKS&&
       (now<oldest||now-oldest<(u64)SYSCLOCK_ARM11*LIVE_AUDIO_BATCH_WAIT_US/1000000ull))return;
    u8 packet[LIVE_AUDIO_MAX_PACKET_BYTES],fresh[LIVE_AUDIO_BATCH_BLOCKS*LIVE_AUDIO_RECORD_BYTES];
    LiveAudioCapture item;unsigned count=0;u64 firstCapture=0;
    while(count<LIVE_AUDIO_BATCH_BLOCKS&&live_audio_queue_pop(&audioQueue,&item,svcGetSystemTick(),
          (u64)SYSCLOCK_ARM11*LIVE_AUDIO_MAX_AGE_US/1000000ull)) {
        u8 *record=fresh+count*LIVE_AUDIO_RECORD_BYTES;
        put32(record,item.sequence);put32(record+4,item.pts_us);
        const u64 begin=svcGetSystemTick();
        const size_t bytes=live_audio_encode(&audioCodec,item.pcm,LIVE_AUDIO_SAMPLES,
                                             record+8,LIVE_AUDIO_BLOCK_BYTES);
        const u32 us=(u32)((svcGetSystemTick()-begin)*1000000/SYSCLOCK_ARM11);
        audioEncodeSumUs+=us;if(us>audioEncodeMaxUs)audioEncodeMaxUs=us;
        if(bytes!=LIVE_AUDIO_BLOCK_BYTES)continue;
        if(!count)firstCapture=item.queued_tick;
        audioEncodedBlocks++;count++;
    }
    if(!count)return;
    const LiveAudioBatch *repeat=live_audio_history_repeat(&audioHistory,svcGetSystemTick(),
        (u64)SYSCLOCK_ARM11*LIVE_AUDIO_MAX_AGE_US/1000000ull);
    unsigned repeatCount=repeat?repeat->count:0;
    memcpy(packet,"R9A1",4);put32(packet+4,session);put32(packet+8,++audioPacketSequence);
    put16(packet+12,(u16)(count+repeatCount));put16(packet+14,0);
    put32(packet+16,LIVE_AUDIO_RATE_MHZ);put16(packet+20,LIVE_AUDIO_SAMPLES);put16(packet+22,LIVE_AUDIO_CHANNELS);
    if(repeat)memcpy(packet+LIVE_AUDIO_HEADER_BYTES,repeat->records,repeatCount*LIVE_AUDIO_RECORD_BYTES);
    memcpy(packet+LIVE_AUDIO_HEADER_BYTES+repeatCount*LIVE_AUDIO_RECORD_BYTES,fresh,count*LIVE_AUDIO_RECORD_BYTES);
    const u32 bytes=LIVE_AUDIO_HEADER_BYTES+(count+repeatCount)*LIVE_AUDIO_RECORD_BYTES;
    pacing_sleep_until(*due);
    if(stopped())return;
    const u64 begin=svcGetSystemTick();
    const int sent=sendto(fd,packet,bytes,0,(struct sockaddr*)&target,sizeof target);
    pacing_charge(due,begin,svcGetSystemTick(),bytes+28,PACKET_AUDIO);
    if(sent==(int)bytes){audioPacketsSent++;audioWireBytes+=bytes+28;}
    else audioSendErrors++;
    /* A failed first send can still recover in its later redundancy slot. */
    live_audio_history_store(&audioHistory,fresh,count,firstCapture);
}
static const char *format_address(u32 address,char *out) {
    struct in_addr ip={.s_addr=htonl(address)};
    return inet_ntop(AF_INET,&ip,out,INET_ADDRSTRLEN)?out:"?";
}

enum { NETWORK_REPORT_BYTES = 768 };
static LightLock networkReportLock;
static char pendingNetworkReport[NETWORK_REPORT_BYTES];
static u32 networkReportLength,networkReportVersion;
static u64 networkReportRetryAt;

static size_t network_report(char *text,size_t capacity) {
    char local[INET_ADDRSTRLEN],mask[INET_ADDRSTRLEN],destination[INET_ADDRSTRLEN],peer[INET_ADDRSTRLEN];
    int n=snprintf(text,capacity,"session=%08lx\nautomatic=%u\npeer_ready=%u\nnetwork_polls=%lu\n"
            "local=%s\nmask=%s\nlast_probe_destination=%s\npeer=%s\nlocal_port=%u\nmedia_port=%u\n"
            "ip_info_result=%d\nip_info_errno=%d\nsocket_errno=%d\nbind_errno=%d\n"
            "probe_attempts=%lu\nprobes_sent=%lu\nprobe_errno=%d\nreceive_errno=%d\n"
            "received_datagrams=%lu\naccepted_replies=%lu\ninvites=%lu\ninvites_answered=%lu\n",
            session,automaticHost,mediaReady,netDiag.polls,format_address(localAddress,local),
            format_address(localMask,mask),format_address(netDiag.lastDestination,destination),
            format_address(ntohl(target.sin_addr.s_addr),peer),netDiag.localPort,ntohs(target.sin_port),
            netDiag.infoResult,netDiag.infoErrno,netDiag.socketErrno,netDiag.bindErrno,netDiag.probeAttempts,
            netDiag.probesSent,netDiag.sendErrno,netDiag.receiveErrno,netDiag.replies,netDiag.accepted,
            netDiag.hints,netDiag.hintsAnswered);
    return n>0&&(size_t)n<capacity?(size_t)n:0;
}

/* The socket worker owns the live network counters. Publish a complete copy
 * before handing it off, and never hold this lock during filesystem I/O. */
static void queue_network_report(void) {
    char text[NETWORK_REPORT_BYTES];
    size_t size=network_report(text,sizeof text);
    if(!size)return;
    LightLock_Lock(&networkReportLock);
    memcpy(pendingNetworkReport,text,size);
    networkReportVersion++;
    __atomic_store_n(&networkReportLength,(u32)size,__ATOMIC_RELEASE);
    LightLock_Unlock(&networkReportLock);
    netDiag.savedAt=svcGetSystemTick();
}

/* Single consumer: startup and fallback run on main, otherwise the status
 * worker owns all writes through its final flush before joining. */
void live_flush_network_report(bool force) {
    if(!__atomic_load_n(&networkReportLength,__ATOMIC_ACQUIRE))return;
    if(!force&&networkReportRetryAt&&svcGetSystemTick()<networkReportRetryAt)return;
    char text[NETWORK_REPORT_BYTES];
    LightLock_Lock(&networkReportLock);
    u32 size=__atomic_load_n(&networkReportLength,__ATOMIC_RELAXED),version=networkReportVersion;
    memcpy(text,pendingNetworkReport,size);
    LightLock_Unlock(&networkReportLock);
    FILE *f=fopen("sdmc:/3ds/3ds-live/network-status.txt","w");
    bool ok=false;
    if(f) {
        size_t written=fwrite(text,1,size,f);
        ok=fclose(f)==0&&written==size;
    }
    if(!ok){networkReportRetryAt=svcGetSystemTick()+SYSCLOCK_ARM11;return;}
    networkReportRetryAt=0;
    LightLock_Lock(&networkReportLock);
    if(version==networkReportVersion)__atomic_store_n(&networkReportLength,0,__ATOMIC_RELEASE);
    LightLock_Unlock(&networkReportLock);
}

/* A fixed port receives invitations from PCs that can broadcast to the console.
 * If occupied, the first outgoing probe still obtains an ephemeral source port. */
static int open_stream_socket(void) {
    int sock=socket(AF_INET,SOCK_DGRAM,0);
    netDiag.socketErrno=sock<0?errno:0;
    if(sock<0)return -1;
    struct sockaddr_in local={.sin_family=AF_INET,.sin_port=htons(automaticHost?LIVE_DISCOVERY_PORT:0)};
    if(fcntl(sock,F_SETFL,O_NONBLOCK)<0) {
        netDiag.socketErrno=errno;
        close(sock);
        return -1;
    }
    netDiag.bindErrno=bind(sock,(struct sockaddr*)&local,sizeof local)<0?errno:0;
    if(!netDiag.bindErrno)netDiag.localPort=ntohs(local.sin_port);
    return sock;
}

static bool send_discovery_probe(const u8 *probe,u32 address,u64 *due) {
    struct sockaddr_in destination=target;
    destination.sin_addr.s_addr=htonl(address);
    netDiag.lastDestination=address;
    netDiag.probeAttempts++;
    bool paired=mediaReady&&address==discovery.peer;
    if(paired&&pacingMode)pacing_sleep_until(*due);
    u64 begin=paired?svcGetSystemTick():0;
    int sent=sendto(fd,probe,LIVE_DISCOVERY_BYTES,0,(struct sockaddr*)&destination,sizeof destination);
    if(paired) {
        u64 returned=svcGetSystemTick();
        if(pacingMode)pacing_charge(due,begin,returned,LIVE_DISCOVERY_BYTES+28,PACKET_STATUS);
        else live_packet_observe(&packetReport,PACKET_STATUS,begin,returned,0,SYSCLOCK_ARM11);
    }
    if(sent!=LIVE_DISCOVERY_BYTES){netDiag.sendErrno=errno;return false;}
    netDiag.probesSent++;
    netDiag.sendErrno=0;
    if(!netDiag.localPort) {
        struct sockaddr_in local={0};socklen_t size=sizeof local;
        if(getsockname(fd,(struct sockaddr*)&local,&size)==0)netDiag.localPort=ntohs(local.sin_port);
    }
    return true;
}

static void reset_receiver_feedback(void) {
    receiverFeedback=(LiveReceiverFeedback){0};
    __atomic_store_n(&receiverVideo,false,__ATOMIC_RELEASE);
}

static void row_peer(bool enable,bool forget) {
    /* Called with lock held. An encode begun under the previous negotiation
     * must not publish a delta after a peer forgets its base. */
    if(enable==rowsEnabled&&!(forget&&rowBase))return;
    rowsEnabled=enable;
    rowBase=rowCandidate=rowProbe=NULL;
    configEpoch++;
    clear_pending();
}

static void reset_peer_config(void) {
    pacingMode=1;
    packetReport.flags=0;
    live_audio_history_reset(&audioHistory);
    live_repair_clear(&repairHistory);
    repairInterest=repairPeerAt=repairCredit=0;
    deferredControl.size=0;
    lastPeerControl=0;
    rowLastAck=0;
    configSequence=0;
    LightLock_Lock(&lock);
    r20LastAck=0;
    if(rowsEnabled)row_peer(false,false);
    else {
        ++configEpoch;
        clear_pending();
    }
    LightLock_Unlock(&lock);
}

static void poll_discovery(u64 now,u64 *due) {
    bool expired=live_discover_expire(&discovery,now,SYSCLOCK_ARM11);
    if(!expired&&lastProbeTick&&now-lastProbeTick<SYSCLOCK_ARM11)return;
    lastProbeTick=now;
    netDiag.polls++;
    if(fd<0)fd=open_stream_socket();
    /* A live reply on this socket verifies the route. Avoid a synchronous
     * SOC network-configuration query on each second of an active stream. */
    if(!expired&&mediaReady&&lastPeerControl&&now>=lastPeerControl&&
       now-lastPeerControl<(u64)LIVE_PEER_LEASE_SECONDS*SYSCLOCK_ARM11) {
        if(automaticHost&&discovery.peer) {
            u8 probe[LIVE_DISCOVERY_BYTES];
            live_discover_invited_probe(&discovery,session,probe);
            send_discovery_probe(probe,discovery.peer,due);
        }
        return;
    }
    struct in_addr ip={0},mask={0},broadcast={0};
    netDiag.infoResult=SOCU_GetIPInfo(&ip,&mask,&broadcast);
    netDiag.infoErrno=netDiag.infoResult?errno:0;
    if(!ip.s_addr) {
        long host=gethostid();
        if(host!=-1)ip.s_addr=(u32)host;
    }
    u32 address=ntohl(ip.s_addr),netmask=ntohl(mask.s_addr);
    bool moved=address&&localAddress&&(address!=localAddress||(netmask&&localMask&&netmask!=localMask));
    if(expired||moved) {
        __atomic_store_n(&mediaReady,false,__ATOMIC_RELEASE);
        reset_receiver_feedback();
        discovery.peer=0;
        discovery.outstanding=false;
        if(automaticHost)target.sin_addr.s_addr=0;
        reset_peer_config();
        if(fd>=0){close(fd);fd=-1;}
        netDiag.localPort=0;
        fd=open_stream_socket();
    }
    if(address) {
        if(address!=localAddress||netmask)localMask=netmask;
        localAddress=address;
    }
    netDiag.reportedBroadcast=ntohl(broadcast.s_addr);
    if(!mediaReady&&(netDiag.polls==2||now-netDiag.savedAt>=5ull*SYSCLOCK_ARM11))queue_network_report();
    if(fd<0)return;
    if(!automaticHost) {
        bool usable=live_peer_address(ntohl(target.sin_addr.s_addr),localAddress,localMask);
        if(!usable&&mediaReady)reset_receiver_feedback();
        if(lastPeerControl&&(!usable||now-lastPeerControl>=(u64)LIVE_PEER_LEASE_SECONDS*SYSCLOCK_ARM11))
            reset_peer_config();
        bool wasReady=mediaReady;
        __atomic_store_n(&mediaReady,usable,__ATOMIC_RELEASE);
        if(wasReady!=usable)queue_network_report();
        return;
    }
    u8 probe[LIVE_DISCOVERY_BYTES];
    live_discover_probe(&discovery,session,probe);
    bool sent;
    if(discovery.peer)sent=send_discovery_probe(probe,discovery.peer,due);
    else {
        /* Routers and SOC implementations differ in handling subnet vs limited
         * broadcast. Both probes share one challenge and expose no media. */
        u32 subnet=netDiag.reportedBroadcast;
        if(!subnet&&localAddress&&localMask)subnet=live_broadcast_address(localAddress,localMask);
        sent=subnet&&subnet!=UINT32_MAX&&send_discovery_probe(probe,subnet,due);
        if(send_discovery_probe(probe,UINT32_MAX,due))sent=true;
    }
    if(!sent) {
        discovery.outstanding=false;
        /* Even if SOC refuses outbound broadcasts, the fixed listener must
         * stay open so a PC invitation can initiate the unicast handshake. */
        if(!discovery.peer&&netDiag.bindErrno){close(fd);fd=-1;netDiag.localPort=0;}
    }
}

static u64 send_status(u64 *due,const u8 *msg,u32 size) {
    pacing_sleep_until(*due);
    u64 begin=svcGetSystemTick();
    sendto(fd,msg,size,0,(struct sockaddr*)&target,sizeof target);
    pacing_charge(due,begin,svcGetSystemTick(),size+28,PACKET_STATUS);
    return begin;
}

/* Call only after address, port, session and control payload validation. */
static void peer_activity(u64 now) {
    lastPeerControl=now;
    if(automaticHost)live_discover_activity(&discovery,ntohl(target.sin_addr.s_addr),now);
}

static void accept_repair(const u8 *msg,int size,u64 now) {
    if(!combinedFec||size<=20||size>20+(int)LIVE_REPAIR_MAX||get32(msg+4)!=session)return;
    u32 n=get32(msg+16);
    if(n!=(u32)size-20)return;
    repairInterest=now;
    if(live_repair_request(&repairHistory,get32(msg+8),get32(msg+12),msg+20,n,LIVE_CHUNK,now,
                           (u64)SYSCLOCK_ARM11*LIVE_REPAIR_SERVICE_MS/1000)) {
        peer_activity(now);
        repairRequests++;
        packetReport.nack=(LivePacketSample){.seq=get32(msg+8),.pts=get32(msg+12),.begin=now};
        packetReport.flags|=2;
    }
}

static void poll_frame_feedback(u64 now) {
    if(!repairInterest||now-repairInterest>2ull*SYSCLOCK_ARM11||deferredControl.size||
       now-lastRepairPoll<SYSCLOCK_ARM11/250)return;
    lastRepairPoll=now;
    struct sockaddr_in from;socklen_t size=sizeof from;
    int n=recvfrom(fd,deferredControl.bytes,sizeof deferredControl.bytes,0,(struct sockaddr*)&from,&size);
    if(n<=0||size<sizeof from||from.sin_family!=AF_INET||from.sin_port!=target.sin_port||
       from.sin_addr.s_addr!=target.sin_addr.s_addr)return;
    if(n>=4&&!memcmp(deferredControl.bytes,"R19N",4))accept_repair(deferredControl.bytes,n,now);
    else {
        deferredControl.from=from;
        deferredControl.size=n;
    }
}

static void control(u64 *due) {
    u64 now=svcGetSystemTick();
    if(fd<0)poll_discovery(now,due);
    if(fd<0)return;
    if(rowLastAck&&now-rowLastAck>=(u64)LIVE_PEER_LEASE_SECONDS*SYSCLOCK_ARM11) {
        rowLastAck=0;
        LightLock_Lock(&lock);
        row_peer(false,false);
        LightLock_Unlock(&lock);
    }
    if(deferredControl.size||now-lastControlTick>=SYSCLOCK_ARM11/100||now-lastProbeTick>=SYSCLOCK_ARM11) {
        lastControlTick=now;
        for(unsigned i=0;i<16;i++) {
            u8 msg[64];struct sockaddr_in from;socklen_t size=sizeof from;
            int n;
            if(deferredControl.size) {
                n=deferredControl.size;from=deferredControl.from;
                memcpy(msg,deferredControl.bytes,n);
                deferredControl.size=0;
            } else n=recvfrom(fd,msg,sizeof msg,0,(struct sockaddr*)&from,&size);
            if(n<0) {
                if(errno!=EAGAIN&&errno!=EWOULDBLOCK)netDiag.receiveErrno=errno;
                break;
            }
            now=svcGetSystemTick();
            netDiag.replies++;
            if(size<sizeof from||from.sin_family!=AF_INET||from.sin_port!=target.sin_port)continue;
            if(automaticHost&&!discovery.peer&&live_discover_invitation(msg,(size_t)n)) {
                u32 address=ntohl(from.sin_addr.s_addr);
                if(!live_reply_address(address,localAddress,localMask)||address==netDiag.reportedBroadcast)continue;
                netDiag.hints++;
                if(netDiag.lastHintTick&&now-netDiag.lastHintTick<SYSCLOCK_ARM11/5)continue;
                netDiag.lastHintTick=now;
                u8 probe[LIVE_DISCOVERY_BYTES];
                live_discover_invited_probe(&discovery,session,probe);
                if(send_discovery_probe(probe,address,due))netDiag.hintsAnswered++;
                continue;
            }
            if(n<8||get32(msg+4)!=session)continue;
            if(automaticHost&&n>=4&&!memcmp(msg,"3DLA",4)) {
                bool first=!discovery.peer;
                u32 address=ntohl(from.sin_addr.s_addr);
                if(live_reply_address(address,localAddress,localMask)&&address!=netDiag.reportedBroadcast&&
                   live_discover_ack(&discovery,address,session,msg,(size_t)n,now)) {
                    netDiag.accepted++;
                    lastPeerControl=now;
                    if(first) {
                        target.sin_addr=from.sin_addr;
                        lastStatusTick=0;
                        __atomic_store_n(&mediaReady,true,__ATOMIC_RELEASE);
                        queue_network_report();
                    }
                }
                continue;
            }
            if(!mediaReady||from.sin_addr.s_addr!=target.sin_addr.s_addr)continue;
            if(n==16&&!memcmp(msg,"R21C",4)&&get32(msg+8)==1&&get32(msg+12)<=1) {
                unsigned requested=get32(msg+12);
                if(pacingMode!=requested){pacingMode=requested;lastStatusTick=0;}
                peer_activity(now);
                continue;
            }
            if(rowBanks&&compactJpeg&&combinedFec&&n==12&&!memcmp(msg,"R20A",4)&&get32(msg+8)==1) {
                LightLock_Lock(&lock);
                r20LastAck=now;
                LightLock_Unlock(&lock);
                peer_activity(now);
                continue;
            }
            if(n==12&&!memcmp(msg,"R23C",4)) {
                u32 mask=get32(msg+8);
                if(mask>=1&&mask<=3) {
                    peer_activity(now);
                    LightLock_Lock(&lock);
                    if(mask!=screenMask) {
                        screenMask=mask;
                        while(pending.count)discard_pending(pending.head,TIMING_RECONFIGURED);
                        bottomReuseSize=0;
                        rowBase=rowCandidate=NULL;
                    }
                    LightLock_Unlock(&lock);
                }
                continue;
            }
            if(n==12&&!memcmp(msg,"R19A",4)&&get32(msg+8)==1) {
                repairPeerAt=now;
                peer_activity(now);
                continue;
            }
            if(n>=4&&!memcmp(msg,"R19N",4)) { accept_repair(msg,n,now); continue; }
            if(rowBanks&&compactJpeg&&n==16&&!memcmp(msg,"R17A",4)) {
                u32 flags=get32(msg+8),seq=get32(msg+12);
                if(flags>1||(!flags&&seq))continue;
                rowLastAck=now;
                peer_activity(now);
                LightLock_Lock(&lock);
                row_peer(true,!flags);
                if(flags&&rowCandidate&&rowCandidate->seq==seq) {
                    rowBase=rowCandidate;
                    rowCandidate=NULL;
                    rowProbe=NULL;
                    rowAcks++;
                }
                LightLock_Unlock(&lock);
                continue;
            }
            if(n==28&&!memcmp(msg,"R3FB",4)) {
                peer_activity(now);
                feedbackCount++;
                live_receiver_observe(&receiverFeedback,get32(msg+8),get32(msg+16),now,
                                      (u64)LIVE_PEER_LEASE_SECONDS*SYSCLOCK_ARM11);
            }
            bool qualityConfig=n==28&&!memcmp(msg,"R16C",4);
            if(qualityConfig||(n==20&&!memcmp(msg,"R6CF",4))||(n==24&&!memcmp(msg,"R10C",4))) {
                u32 seq=get32(msg+8),hz=get32(msg+12),bps=get32(msg+16);
                bool nextCombined=n>=24?get32(msg+20)!=0:combinedFec;
                unsigned targetQ=live_config_jpeg_target(msg,n,qualityTarget);
                if(!live_valid_fps(hz)||bps<LIVE_MIN_RATE||bps>LIVE_MAX_RATE||!seq||
                   !live_valid_quality_target(targetQ))continue;
                if(n>=24&&get32(msg+20)>1)continue;
                if(configSequence&&(u32)(seq-configSequence)>=0x80000000u)continue;
                peer_activity(now);
                configSequence=seq;
                LightLock_Lock(&lock);
                configure_stream(hz,bps,nextCombined,targetQ);
                LightLock_Unlock(&lock);lastStatusTick=0;
            }
        }
        const bool hasVideo=live_receiver_recent(&receiverFeedback,now,(u64)LIVE_PEER_LEASE_SECONDS*SYSCLOCK_ARM11);
        if(hasVideo!=__atomic_load_n(&receiverVideo,__ATOMIC_RELAXED))
            __atomic_store_n(&receiverVideo,hasVideo,__ATOMIC_RELEASE);
    }
    /* Consume queued acknowledgements before expiring the lease, rotating
     * the challenge or closing the only socket that can receive them. */
    poll_discovery(svcGetSystemTick(),due);
    now=svcGetSystemTick();
    if(!mediaReady||now-lastStatusTick<SYSCLOCK_ARM11)return;
    u8 msg[108];const u32 msgBytes=audioEnabled?108:76;memcpy(msg,audioEnabled?"R9ST":"R7ST",4);
    const u32 values[]={session,(u32)((now-started)*1000/SYSCLOCK_ARM11),rate,live_av_video_rate(rate,audioEnabled),fps,
        live_encoded_frames(0),live_encoded_frames(1),frames[0],frames[1],errors,expired,feedbackCount,
        (u32)encoderCores[0],(u32)encoderCores[1],
        __atomic_load_n(&lastEncodeUs[0],__ATOMIC_RELAXED),__atomic_load_n(&lastEncodeUs[1],__ATOMIC_RELAXED),
        LIVE_MAX_RATE,LIVE_FPS_MASK,
        audioEnabled?1u:0u,LIVE_AUDIO_RATE_MHZ,LIVE_AUDIO_CHANNELS,LIVE_AUDIO_SAMPLES,
        __atomic_load_n(&audioQueue.queued,__ATOMIC_RELAXED),
        __atomic_load_n(&audioQueue.dropped,__ATOMIC_RELAXED)+__atomic_load_n(&audioQueue.expired,__ATOMIC_RELAXED),
        audioPacketsSent,live_audio_queue_depth(&audioQueue)};
    for(unsigned i=0;i<(msgBytes-4)/4;i++)put32(msg+4+4*i,values[i]);
    uint32_t pacingWords[LIVE_PACING_WORDS];
    live_packet_snapshot(&packetReport,pacingWords,session,values[1],pacingMode,started,SYSCLOCK_ARM11);
    lastStatusTick=send_status(due,msg,msgBytes);
    memcpy(msg,"R16S",4);put32(msg+4,session);put32(msg+8,1);put32(msg+12,qualityTarget);put32(msg+16,values[1]);
    send_status(due,msg,20);
    memcpy(msg,"R16Q",4);put32(msg+4,session);put32(msg+8,1);put32(msg+12,LIVE_JPEG_EXTRA_QUALITY);
    send_status(due,msg,16);
    /* Cumulative accepted media IPv4 bytes, including FEC and repeated keys.
     * Status, discovery and failed sends do not count. Totals wrap at 2^32. */
    memcpy(msg,"R17T",4);put32(msg+4,session);put32(msg+8,values[1]);
    put32(msg+12,(u32)(audioWireBytes+videoWireBytes));put32(msg+16,(u32)videoWireBytes);
    send_status(due,msg,20);
    if(rowBanks&&compactJpeg) {
        memcpy(msg,"R17S",4);put32(msg+4,session);put32(msg+8,1);
        send_status(due,msg,12);
        if(combinedFec){memcpy(msg,"R20S",4);send_status(due,msg,12);}
    }
    if(combinedFec) {
        memcpy(msg,"R19S",4);put32(msg+4,session);put32(msg+8,1);put32(msg+12,LIVE_REPAIR_MAX);
        send_status(due,msg,16);
    }
    LightLock_Lock(&lock);u32 overwritten=replaced;LightLock_Unlock(&lock);
    const u32 timing[]={session,values[1],overwritten,rowRepeats,repairRequests,repairPackets,
        repairWireBytes,audioQueue.dropped,audioQueue.expired,
        (u32)(sendActiveTicks/SYSCLOCK_ARM11*1000000ull+(sendActiveTicks%SYSCLOCK_ARM11)*1000000ull/SYSCLOCK_ARM11)};
    memcpy(msg,"R19T",4);
    for(unsigned i=0;i<sizeof timing/sizeof timing[0];i++)put32(msg+4+4*i,timing[i]);
    send_status(due,msg,4+sizeof timing);
    memcpy(msg,"R21S",4);put32(msg+4,session);put32(msg+8,1);put32(msg+12,pacingMode);
    send_status(due,msg,16);
    /* ARM11 and the R21T word layout are both little endian. */
    send_status(due,(const u8 *)pacingWords,sizeof pacingWords);
#ifdef LIVE_FRAME_TIMING
    u32 capacity;
    if(live_timing_marker(lastStatusTick,&capacity)) {
        u8 marker[24];memcpy(marker,"R12D",4);put32(marker+4,session);put32(marker+8,1);put32(marker+12,capacity);
        pacing_sleep_until(*due);
        u64 call=svcGetSystemTick();
        put32(marker+16,(u32)call);put32(marker+20,(u32)(call>>32));
        sendto(fd,marker,sizeof marker,0,(struct sockaddr*)&target,sizeof target);
        pacing_charge(due,call,svcGetSystemTick(),sizeof marker+28,PACKET_STATUS);
    }
#endif
}
typedef struct { u64 totalDue; LiveVideoPacing video; bool firstPacket; } PacketPacing;

/* Use spare wire time between fresh packets. A repair shares the strict
 * total clock with audio and never advances the fresh video's slot clock. */
static bool send_repair(u64 *totalDue,u64 now,u64 nextFreshDue) {
    if(!repairCredit||now<*totalDue||stopped()||capture_paused())return false;
    LiveRepairFrame *f=live_repair_next(&repairHistory,now);
    if(!f)return false;
    u32 index=f->indices[f->next],off=index*LIVE_CHUNK;
    u32 len=f->top+f->bottom-off;if(len>LIVE_CHUNK)len=LIVE_CHUNK;
    u32 wire=len+60;
    if(wire>repairCredit)return false;
    if(nextFreshDue&&live_packet_due(*totalDue,now,wire,rate,SYSCLOCK_ARM11)>nextFreshDue)return false;
    u8 packet[LIVE_CHUNK+32];
    memcpy(packet,"R19P",4);put32(packet+4,session);put32(packet+8,f->seq);put32(packet+12,f->pts);
    put32(packet+16,f->top);put32(packet+20,f->bottom);put32(packet+24,index);put32(packet+28,f->fps);
    memcpy(packet+32,sendArena+f->offset+off,len);
    u64 begin=svcGetSystemTick();
    int sent=sendto(fd,packet,32+len,0,(struct sockaddr*)&target,sizeof target);
    u64 returned=svcGetSystemTick();
    pacing_charge(totalDue,begin,returned,wire,PACKET_REPAIR);
    if(!f->next&&sent==(int)(32+len)) {
        packetReport.repair=(LivePacketSample){f->seq,f->pts,index,begin,returned};
        packetReport.flags|=4;
    }
    repairCredit-=wire;f->next++;
    if(sent==(int)(32+len)){repairPackets++;repairWireBytes+=wire;videoWireBytes+=wire;}
    return true;
}

static bool send_packet(PacketPacing *pacing,u64 deadline,const char *magic,u32 seq,u32 stamp,
                        u32 sc,u32 total,u32 offset,const u8 *data,u32 len) {
    bool usedGap=false;
    for(;;) {
        send_audio(&pacing->totalDue);
        u64 now=svcGetSystemTick();
        poll_frame_feedback(now);
        now=svcGetSystemTick();
        if(stopped()||now>deadline) {
            live_timing_finish(stopped()?TIMING_STOPPED:TIMING_DEADLINE);
            return false;
        }
        u64 due=live_video_due(&pacing->video);
        if(!usedGap&&repairCredit&&pacing->totalDue<due&&now<due&&live_repair_next(&repairHistory,now)) {
            if(send_repair(&pacing->totalDue,now,due)){usedGap=true;continue;}
            if(now<pacing->totalDue)due=pacing->totalDue;
        }
        if(due<pacing->totalDue)due=pacing->totalDue;
        if(now>=due)break;
        u64 wait=due-now;
        /* A sparse video pair must not keep a ready audio batch waiting. */
        if(audioEnabled&&wait>SYSCLOCK_ARM11/500)wait=SYSCLOCK_ARM11/500;
        pacing_sleep_until(now+wait);
    }
    const u32 header=32;
    u8 packet[LIVE_CHUNK+32];memcpy(packet,magic,4);put32(packet+4,session);put32(packet+8,seq);put32(packet+12,stamp);
    put32(packet+16,sc);put32(packet+20,total);put32(packet+24,offset);put32(packet+28,fps);
    memcpy(packet+header,data,len);u64 begin=svcGetSystemTick();
    int sent=sendto(fd,packet,len+header,0,(struct sockaddr*)&target,sizeof target);
    u64 returned=svcGetSystemTick();
    u32 us=(u32)((returned-begin)*1000000/SYSCLOCK_ARM11);
    live_timing_packet(begin,returned,sent==(int)(len+header));
    sendCalls++;sendSumUs+=us;if(us>sendMaxUs)sendMaxUs=us;
    pacing_charge(&pacing->totalDue,begin,returned,len+header+28,PACKET_VIDEO);
    live_video_charge(&pacing->video,begin,len+header+28,SYSCLOCK_ARM11);
    if(sent!=(int)(len+header)){lastErrno=errno;errors++;live_timing_finish(TIMING_SOCKET_ERROR);return false;}
    if(pacing->firstPacket) {
        packetReport.first=(LivePacketSample){seq,stamp,offset,begin,returned};
        packetReport.flags|=1;
        pacing->firstPacket=false;
    }
    videoWireBytes+=len+header+28;
    return true;
}
static void sender(void *unused) {
    (void)unused;PacketPacing pacing={.totalDue=svcGetSystemTick()};
    bool sentFrame=false;u32 lastSentSeq=0;
    while(!stopped()) {
        control(&pacing.totalDue);
        if(!mediaReady||capture_paused()){svcSleepThread(10000000);continue;}
        send_audio(&pacing.totalDue);
        u32 sizes[2]={0,0},seq=0,stamp=0,repeat=0;u64 queued=0;bool ready;
        LiveRepairFrame *cached=NULL;
        u8 *sending[2]={NULL,NULL};
        LightLock_Lock(&lock);
        u64 now=svcGetSystemTick();
        while(pending.count) {
            const LivePendingPair *head=&pending.slots[pending.head];
            if(head->epoch!=configEpoch||(head->r20&&!r20_recent(now)))
                discard_pending(pending.head,TIMING_RECONFIGURED);
            else if(live_pending_expired(head,now,SYSCLOCK_ARM11,fps)) {
                discard_pending(pending.head,TIMING_TOO_OLD);
                expired++;
            } else break;
        }
        ready=pending.count!=0;
        if(ready) {
            unsigned slot=pending.head;
            const LivePendingPair *pair=&pending.slots[slot];
            seq=pair->seq;stamp=pair->stamp;queued=pair->queued;
            /* Selective repair protects the original key without sacrificing
             * a new capture slot. Legacy receivers still get adjacent copies. */
            bool selective=repairPeerAt&&now-repairPeerAt<(u64)LIVE_PEER_LEASE_SECONDS*SYSCLOCK_ARM11;
            bool sendKey=rowCandidate&&(selective?(rowSends==0&&rowCandidate->seq==seq):
                                        rowSends<3&&(u32)(seq-rowCandidate->seq)<0x80000000u);
            if(sendKey&&(svcGetSystemTick()-rowLastKey>SYSCLOCK_ARM11/4||
                         rowCandidate->length[0]+rowCandidate->length[1]>pair_payload_budget(rate,fps,combinedFec,qualityTarget))) {
                sendKey=false;
                rowSends=3;
            }
            if(sendKey) {
                seq=rowCandidate->seq;stamp=rowCandidate->stamp;
                repeat=rowSends++;
                if(repeat)rowRepeats++;
            }
            if(seq==pair->seq)live_timing_claim(slot);
            else {live_timing_drop_pending(slot,TIMING_REPLACED);replaced++;}
            for(unsigned sc=0;sc<2;sc++)sizes[sc]=sendKey?rowCandidate->length[sc]:pair->length[sc];
            u64 reserved=svcGetSystemTick();
            u64 until=reserved+(u64)SYSCLOCK_ARM11*LIVE_REPAIR_HISTORY_MS/1000;
            static LiveRepairFrame singleFrame;
            if(sizes[0]&&sizes[1]) {
                cached=live_repair_reserve(&repairHistory,sizeof sendArena,sizes[0],sizes[1],seq,stamp,fps,reserved,until);
            } else {
                /* A single-screen stream has no second panel, so it cannot share
                 * the two-panel repair arena; send it directly without repair. */
                singleFrame=(LiveRepairFrame){0};
                singleFrame.top=sizes[0];
                singleFrame.bottom=sizes[1];
                cached=&singleFrame;
            }
            if(cached) {
                sending[0]=sendArena+(sizes[0]&&sizes[1]?cached->offset:0);
                sending[1]=sending[0]+sizes[0];
            }
            for(unsigned sc=0;cached&&sc<2;sc++) {
                const u8 *panel=pendingBytes+pair->offset+(sc?pair->length[0]:0);
                memcpy(sending[sc],sendKey?row_panel(rowCandidate,sc):panel,sizes[sc]);
            }
            live_pending_remove(&pending,slot);
        }
        LightLock_Unlock(&lock);
        if(!ready){if(!send_repair(&pacing.totalDue,svcGetSystemTick(),0))svcSleepThread(500000);continue;}
        if(!cached){expired++;live_timing_finish(TIMING_TOO_OLD);continue;}
        u32 screenChunks[2]={live_data_packets(sizes[0]),live_data_packets(sizes[1])};
        const bool shared=combinedFec&&sizes[0]&&sizes[1];
        u32 dataCount=shared?live_data_packets(sizes[0]+sizes[1]):screenChunks[0]+screenChunks[1];
        u32 wire=shared?live_combined_wire(sizes[0]+sizes[1]):
            live_screen_wire(sizes[0])+live_screen_wire(sizes[1]);
        repairCredit=shared?live_repair_allowance(live_av_video_rate(rate,audioEnabled),fps,wire):0;
        live_video_slot(&pacing.video,queued,wire,dataCount,fps,live_av_video_rate(rate,audioEnabled));
#ifdef LIVE_FRAME_TIMING
        u32 parityCount=shared?live_parity_packets(dataCount,LIVE_PAIR_PARITY):
            live_parity_packets(screenChunks[0],LIVE_PARITY)+live_parity_packets(screenChunks[1],LIVE_PARITY);
        live_timing_plan(dataCount,dataCount+parityCount);
#endif
        u64 begin=svcGetSystemTick(),deadline=live_video_deadline(begin,SYSCLOCK_ARM11,wire,
                                            live_av_video_rate(rate,audioEnabled),qualityTarget!=0);
        if(begin-queued>SYSCLOCK_ARM11/fps*2){expired++;live_timing_finish(TIMING_TOO_OLD);continue;}
        pacing.firstPacket=true;
        u8 parity[LIVE_PAIR_PARITY][LIVE_CHUNK]={0};bool ok=true;
        if(shared) {
            u8 fragment[LIVE_CHUNK];u32 count=dataCount;
            /* Shift repeated keys by a third so the same short Wi-Fi fade
             * need not erase the same offsets on each pass. */
            u32 index=repeat*count/3;
            for(u32 step=0;ok&&step<count;step++) {
                u32 len=live_pair_fragment(fragment,sending[0],sizes[0],sending[1],sizes[1],index);
                u8 *stripe=parity[index%LIVE_PAIR_PARITY];
                for(u32 j=0;j<len;j++)stripe[j]^=fragment[j];
                ok=send_packet(&pacing,deadline,"R10P",seq,stamp,sizes[0],sizes[1],index,fragment,len);
                if(++index==count)index=0;
            }
            for(unsigned order=0;order<LIVE_PAIR_PARITY&&ok;order++) {
                unsigned stripe=(count+order)%LIVE_PAIR_PARITY;
                if(stripe>=count)continue;
                ok=send_packet(&pacing,deadline,"R10F",seq,stamp,sizes[0],sizes[1],stripe,parity[stripe],LIVE_CHUNK);
            }
        } else {
            for(u32 off=0;ok&&(off<sizes[0]||off<sizes[1]);off+=LIVE_CHUNK)for(unsigned sc=0;sc<2&&ok;sc++) {
                if(off>=sizes[sc])continue;
                u32 len=sizes[sc]-off>LIVE_CHUNK?LIVE_CHUNK:sizes[sc]-off;
                u8 *stripe=parity[sc*LIVE_PARITY+off/LIVE_CHUNK%LIVE_PARITY];
                for(u32 j=0;j<len;j++)stripe[j]^=sending[sc][off+j];
                ok=send_packet(&pacing,deadline,"R3P2",seq,stamp,sc,sizes[sc],off,sending[sc]+off,len);
            }
            for(unsigned stripe=0;stripe<LIVE_PARITY&&ok;stripe++)for(unsigned sc=0;sc<2&&ok;sc++) {
                if(stripe>=screenChunks[sc])continue;
                ok=send_packet(&pacing,deadline,"R3F2",seq,stamp,sc,sizes[sc],stripe,parity[sc*LIVE_PARITY+stripe],LIVE_CHUNK);
            }
        }
        sendActiveTicks+=svcGetSystemTick()-begin;
        cached->complete=ok&&shared;
        if(ok){
            if(!sentFrame||lastSentSeq!=seq){if(sizes[0])frames[0]++;if(sizes[1])frames[1]++;}
            sentFrame=true;lastSentSeq=seq;
            live_timing_finish(TIMING_SENT);
        }else expired++;
    }
}
Result live_start(void *(*allocate)(u32 bytes)) {
    if(!allocate||worker||encoders[0]||encoders[1])return -1;
    LightLock_Init(&lock);for(unsigned i=0;i<2;i++)LightLock_Init(&rawLocks[i]);
    for(unsigned i=0;i<3;i++)LightEvent_Init(&events[i],RESET_ONESHOT);
    started=svcGetSystemTick();stopping=false;jobDone=1;pending=(LivePendingQueue){0};planMask=0;
    pacingMode=1;packetReport=(LivePacketReport){0};
    __atomic_store_n(&capturePaused,false,__ATOMIC_RELEASE);
    memset(rawValid,0,sizeof rawValid);memset(attempts,0,sizeof attempts);memset(jpegFrames,0,sizeof jpegFrames);
    memset(maxUs,0,sizeof maxUs);memset(maxBytes,0,sizeof maxBytes);memset(sumUs,0,sizeof sumUs);
    memset(convertCount,0,sizeof convertCount);memset(convertMaxUs,0,sizeof convertMaxUs);memset(convertSumUs,0,sizeof convertSumUs);
    memset(convertedMcus,0,sizeof convertedMcus);snapshotCount=snapshotMaxUs=0;snapshotSumUs=0;
    memset(changedMcus,0,sizeof changedMcus);memset(sourceReuse,0,sizeof sourceReuse);memset(lastSource,0,sizeof lastSource);
    memset(frames,0,sizeof frames);memset(lastEncodeUs,0,sizeof lastEncodeUs);
    sequence=replaced=errors=expired=oversize=retries=pairs=sendCalls=sendMaxUs=feedbackCount=0;
    configEpoch=configSequence=configChanges=ratePendingKept=rateEncodingKept=0;sendSumUs=0;lastErrno=0;qualityTarget=0;quality=60;spare=0;bottomReuseSize=0;screenMask=3;
    payloadControl=(LivePayloadControl){.percent=100};
    compactSavedBytes=0;compactFrames=compactFallbacks=0;
    rowsEnabled=false;rowBase=rowCandidate=rowProbe=rowBanks=NULL;
    r20Prepared=NULL;r20Dictionary=NULL;r20LastAck=r20EncodeUs=r20LastAttempt=r20Saved=0;
    r20Attempts=r20Panels=r20MaxUs=r20MeanUs=r20Skipped=0;
    rowKeys=rowRepeats=rowDeltas=rowAcks=rowSends=0;
    manualRenderFailures=manualWireDrops=0;
    repairHistory=(LiveRepairHistory){0};
    repairRequests=repairPackets=repairWireBytes=repairCredit=0;
    deferredControl.size=0;lastRepairPoll=repairInterest=repairPeerAt=sendActiveTicks=0;
    probeAfter=0;
    rowLastKey=rowLastAck=rowBytesSaved=0;
    live_audio_queue_init(&audioQueue);live_audio_init(&audioCodec);live_audio_history_reset(&audioHistory);
    audioPacketSequence=audioPacketsSent=audioSendErrors=audioEncodedBlocks=audioEncodeMaxUs=0;
    audioEncodeSumUs=audioWireBytes=0;
    videoWireBytes=0;
    session=(u32)started;if(!session)session=1;
    lastStatusTick=lastControlTick=started;
    discovery=(LiveDiscovery){.challenge=(u32)(started>>16)};
    reset_receiver_feedback();
    lastProbeTick=lastPeerControl=0;
    localAddress=localMask=0;
    memset(&netDiag,0,sizeof netDiag);
    __atomic_store_n(&mediaReady,false,__ATOMIC_RELEASE);
    LightLock_Init(&networkReportLock);
    queue_network_report();
    live_flush_network_report(true);
    for(unsigned i=0;i<2;i++) {
        live_jpeg_reset(i);rawBytes[i]=live_jpeg_frame_size(i?320:400);
        size_t sourceBytes=live_jpeg_source_storage_size(i?320:400);
        void *arena=allocate((u32)(sourceBytes+rawBytes[i]));
        rawSources[i]=live_jpeg_source_init(arena,sourceBytes,i?320:400);
        rawWorking[i]=arena?(LiveJpegFrame*)((u8*)arena+sourceBytes):NULL;
        if(!rawSources[i]||!rawWorking[i]){live_stop();return -7;}
    }
    u32 *socStorage=allocate(SOC_ARENA_BYTES);
    if(!socStorage){live_stop();return -7;}
    Result rc=socInit(socStorage,SOC_ARENA_BYTES);if(R_FAILED(rc)){live_stop();return rc;}
    socReady=true;
    /* DMA buffers are mapped later when the LCD modes become known. Leave
     * room for both at maximum stride before taking any optional memory. */
    const u32 rowPages=(2*sizeof(*rowBanks)+4095u)&~4095u;
    if(compactJpeg&&live_pages_available(rowPages+LIVE_CAPTURE_MAX_DMA_BYTES))
        rowBanks=allocate(rowPages);
    for(int i=1;i>=0;i--) {
        encoderCores[i]=i+2;
        encoders[i]=threadCreate(encoder,(void*)(uintptr_t)i,8192,0x3E,encoderCores[i],false);
        if(!encoders[i]){encoderCores[i]=1;encoders[i]=threadCreate(encoder,(void*)(uintptr_t)i,8192,0x3E,1,false);}
        if(!encoders[i]){live_stop();return -6;}
    }
    worker=threadCreate(sender,NULL,16384,0x3E,1,false);if(!worker){live_stop();return -5;}return 0;
}
void live_stop(void) {
    __atomic_store_n(&stopping,true,__ATOMIC_RELEASE);
    for(unsigned i=0;i<3;i++)LightEvent_Signal(&events[i]);
    for(unsigned i=0;i<2;i++)if(encoders[i]){threadJoin(encoders[i],U64_MAX);threadFree(encoders[i]);encoders[i]=NULL;}
    if(worker){threadJoin(worker,U64_MAX);threadFree(worker);worker=NULL;}
    queue_network_report();
    rowBase=rowCandidate=rowProbe=rowBanks=NULL;
#ifdef LIVE_FRAME_TIMING
    bool timingSaved=live_timing_export(session,started);
#endif
    if(fd>=0){close(fd);fd=-1;}if(socReady){socExit();socReady=false;}
    for(unsigned i=0;i<2;i++) {
        live_jpeg_reset(i);live_jpeg_source_destroy(rawSources[i]);rawSources[i]=NULL;
        rawWorking[i]=NULL; /* Caller releases the complete SYSTEM arena. */
    }
    FILE *f=fopen("sdmc:/3ds/3ds-live/live-stats.txt","w");if(!f)return;
    char networkText[NETWORK_REPORT_BYTES];
    size_t networkBytes=network_report(networkText,sizeof networkText);
    fwrite(networkText,1,networkBytes,f);
#ifdef LIVE_FRAME_TIMING
    fprintf(f,"timing_session=%08lx\ntiming_saved=%u\n",(unsigned long)session,timingSaved?1u:0u);
#endif
    fprintf(f,"version=%s\nsent_top=%lu\nsent_bottom=%lu\nreplaced=%lu\nsend_errors=%lu\nrate=%lu\nfps=%lu\n",
        combinedFec?"live-r10-fec4-candidate":"live-r9-audio",frames[0],frames[1],replaced,errors,rate,fps);
    fprintf(f,"duration_ms=%llu\nexpired_pairs=%lu\nlast_send_errno=%d\nadaptive=0\npair_jpeg_budget=%lu\n",
        (svcGetSystemTick()-started)*1000/SYSCLOCK_ARM11,expired,lastErrno,(u32)live_pair_budget(live_av_video_rate(rate,audioEnabled),fps));
    fprintf(f,"payload_target_percent=%u\n",payloadControl.percent);
    fprintf(f,"r20_attempts=%lu\nr20_skipped_for_time=%lu\nr20_panels_queued=%lu\nr20_bytes_saved_queued=%llu\nr20_encode_mean_us=%llu\nr20_encode_max_us=%lu\n",
        r20Attempts,r20Skipped,r20Panels,r20Saved,r20Attempts?r20EncodeUs/r20Attempts:0,r20MaxUs);
    fprintf(f,"combined_fec=%u\npair_payload_budget=%lu\nquality_target=%u\n",combinedFec?1u:0u,
        pair_payload_budget(rate,fps,combinedFec,qualityTarget),qualityTarget);
    fprintf(f,"rate_pending_kept=%lu\nrate_encoding_kept=%lu\n",ratePendingKept,rateEncodingKept);
    fprintf(f,"pairs_attempted=%lu\npairs_oversize=%lu\nrequantize_passes=%lu\nquality=%u\nfec_stripes=%u\nconfig_changes=%lu\n",
        pairs,oversize,retries,quality,combinedFec?LIVE_PAIR_PARITY:LIVE_PARITY,configChanges);
    fprintf(f,"manual_render_failures=%lu\nmanual_wire_budget_drops=%lu\n",manualRenderFailures,manualWireDrops);
    fprintf(f,"repair_requests=%lu\nrepair_packets=%lu\nrepair_ipv4_bytes=%lu\n",repairRequests,repairPackets,repairWireBytes);
    fprintf(f,"repair_nack_cache_miss=%lu\nrepair_nack_stale=%lu\nrepair_nack_duplicate=%lu\nrepair_nack_invalid=%lu\n"
              "repair_fragments_expired=%lu\nrepair_fragments_evicted=%lu\n",repairHistory.missing,repairHistory.stale,
            repairHistory.duplicate,repairHistory.invalid,repairHistory.expired_fragments,repairHistory.evicted_fragments);
    fprintf(f,"fec_scope=%s\nbudget_basis=%s\n",combinedFec?"pair":"screen",combinedFec?"wire_payload":"full_jpeg");
    fprintf(f,"send_calls=%lu\nsend_mean_us=%llu\nsend_max_us=%lu\nfeedback_count=%lu\n",
        sendCalls,sendCalls?sendSumUs/sendCalls:0,sendMaxUs,feedbackCount);
    fprintf(f,"video_wire_bytes=%llu\n",videoWireBytes);
    fprintf(f,"send_timeout_ms=%u\n",LIVE_SEND_TIMEOUT_MS);
    fprintf(f,"source_layout=ycbcr420_mcu\nsnapshot_mean_us=%llu\nsnapshot_max_us=%lu\n",
        snapshotCount?snapshotSumUs/snapshotCount:0,snapshotMaxUs);
    fprintf(f,"compact_jpeg=%u\ncompact_frames_queued=%lu\ncompact_bytes_saved_queued=%llu\ncompact_fallbacks=%lu\n",
        compactJpeg?1:0,compactFrames,compactSavedBytes,compactFallbacks);
    fprintf(f,"row_keys=%lu\nrow_key_repeats=%lu\nrow_key_acks=%lu\nrow_delta_panels=%lu\nrow_bytes_saved_queued=%llu\n",
        rowKeys,rowRepeats,rowAcks,rowDeltas,rowBytesSaved);
    fprintf(f,"audio_enabled=%u\naudio_rate_mhz=%u\naudio_samples_per_block=%u\naudio_blocks_queued=%lu\n"
        "audio_queue_full=%lu\naudio_expired_blocks=%lu\naudio_packets_sent=%lu\naudio_send_errors=%lu\n"
        "audio_wire_bytes=%llu\naudio_encoded_blocks=%lu\naudio_encode_mean_us=%llu\naudio_encode_max_us=%lu\n"
        "audio_reserved_bytes_per_second=%lu\n",
        audioEnabled?1u:0u,LIVE_AUDIO_RATE_MHZ,LIVE_AUDIO_SAMPLES,audioQueue.queued,audioQueue.dropped,audioQueue.expired,
        audioPacketsSent,audioSendErrors,audioWireBytes,audioEncodedBlocks,
        audioEncodedBlocks?audioEncodeSumUs/audioEncodedBlocks:0,audioEncodeMaxUs,
        audioEnabled?(u32)LIVE_AUDIO_RESERVE_BYTES:0u);
    for(unsigned i=0;i<2;i++)fprintf(f,
        "screen%u_attempts=%lu\nscreen%u_jpeg=%lu\nscreen%u_encode_mean_us=%llu\nscreen%u_encode_max_us=%lu\n"
        "screen%u_max_bytes=%lu\nscreen%u_encoder_core=%ld\nscreen%u_changed_mcus=%llu\nscreen%u_source_reused=%lu\n"
        "screen%u_dma_converted=%lu\nscreen%u_convert_mean_us=%llu\nscreen%u_convert_max_us=%lu\nscreen%u_converted_mcus=%llu\n",
        i,attempts[i],i,jpegFrames[i],i,attempts[i]?sumUs[i]/attempts[i]:0,i,maxUs[i],i,maxBytes[i],i,encoderCores[i],
        i,changedMcus[i],i,sourceReuse[i],i,convertCount[i],i,convertCount[i]?convertSumUs[i]/convertCount[i]:0,i,convertMaxUs[i],i,convertedMcus[i]);
    fclose(f);
}
bool live_submit(int sc,const u8 *frame,u32 fmt,u32 stride,u32 pts,bool wide) {
    if(sc<0||sc>1||!worker||stopped()||capture_paused()||wide)return false;
    u64 begin=svcGetSystemTick();u32 w=sc?320:400;
    /* Exact native-source comparison precedes color conversion. Only changed
     * MCUs enter the compact YCbCr420 snapshot; no RGB565 staging frame. */
    LightLock_Lock(&rawLocks[sc]);
    int converted=live_jpeg_source_update(rawSources[sc],frame,(size_t)w*stride,fmt,stride);
    if(converted>=0){rawPts[sc]=pts;rawValid[sc]=true;}
    LightLock_Unlock(&rawLocks[sc]);
    if(converted<0)return false;
    convertedMcus[sc]+=(unsigned)converted;
    LightEvent_Signal(&events[0]);u32 us=(u32)((svcGetSystemTick()-begin)*1000000/SYSCLOCK_ARM11);
    convertCount[sc]++;convertSumUs[sc]+=us;if(us>convertMaxUs[sc])convertMaxUs[sc]=us;return true;
}
void live_schedule(u32 stamp,u32 top,u32 bottom,u32 mask) {
    if(!worker||stopped())return;
    LightLock_Lock(&lock);planStamp=stamp;planPts[0]=top;planPts[1]=bottom;planMask=mask;LightLock_Unlock(&lock);
}
