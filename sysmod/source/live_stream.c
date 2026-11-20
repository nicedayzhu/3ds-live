#include "live_stream.h"
#include "live_timing.h"
#include "live_jpeg.h"
#include "live_jpeg_source.h"
#include "live_jpeg_wire.h"
#include "live_pacing.h"
#include "live_pair.h"
#include "live_audio_transport.h"
#include "live_discovery.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Encoder 0 coordinates pairs, encoder 1 handles bottom. The socket worker
 * owns rate changes and one latest complete pair. No temporal wire dependency. */
#define MAX_JPEG LIVE_JPEG_MAX_BYTES
static LiveJpegSource *rawSources[2];
static LiveJpegFrame *rawWorking[2];
static size_t rawBytes[2];
static u8 encoded[2][MAX_JPEG],pending[2][MAX_JPEG],sending[2][MAX_JPEG];
static bool rawValid[2],pendingReady,enabled,socReady,compactJpeg,audioEnabled,combinedFec;
static volatile bool stopping;
static u32 rawPts[2],planPts[2],planStamp,planMask;
static u32 pendingLengths[2],pendingSeq,pendingStamp,pendingEpoch,sequence,session;
static u64 pendingAt,started;
static LightLock lock,rawLocks[2];
static LightEvent events[3];
static Thread worker,encoders[2];
static s32 encoderCores[2];
static int fd=-1,lastErrno;
#define SOC_ARENA_BYTES 0x100000u
static struct sockaddr_in target;
static LiveDiscovery discovery;
static bool automaticHost,mediaReady;
static u64 lastProbeTick;
static u32 localAddress,localMask;
static struct {
    u32 polls,probeAttempts,probesSent,replies,accepted,hints,hintsAnswered,reportedBroadcast,lastDestination;
    int infoResult,infoErrno,socketErrno,bindErrno,sendErrno,receiveErrno;
    unsigned localPort;
    u64 savedAt,lastHintTick;
} netDiag;
static u32 fps=30,rate=800000,configEpoch,configSequence;
static unsigned quality=60,spare;
static LivePayloadControl payloadControl;
static u32 jobKind,jobQuality,jobDone,jobSize[2],jobChanged[2],jobUs[2];
static u32 attempts[2],jpegFrames[2],maxUs[2],maxBytes[2],convertCount[2],convertMaxUs[2];
static u64 sumUs[2],convertSumUs[2],changedMcus[2];
static u64 convertedMcus[2],snapshotSumUs;
static u32 snapshotCount,snapshotMaxUs;
static u32 sourceReuse[2],lastSource[2],frames[2],replaced,errors,expired,oversize,retries,pairs;
static u32 sendCalls,sendMaxUs,lastEncodeUs[2],feedbackCount,configChanges;
static u32 ratePendingKept,rateEncodingKept;
static u64 sendSumUs,lastStatusTick,lastControlTick;
static u64 compactSavedBytes;
static u32 compactFrames,compactFallbacks;
static LiveAudioQueue audioQueue;
static LiveAudioState audioCodec;
static u8 audioPrevious[LIVE_AUDIO_BATCH_BLOCKS*LIVE_AUDIO_RECORD_BYTES];
static unsigned audioPreviousCount;
static u64 audioPreviousTick,audioEncodeSumUs,audioWireBytes;
static u32 audioPacketSequence,audioPacketsSent,audioSendErrors,audioEncodedBlocks,audioEncodeMaxUs;
static u32 get32(const u8 *p){return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24;}
static void put32(u8 *p,u32 n){for(unsigned i=0;i<4;i++)p[i]=(u8)(n>>(8*i));}
static void put16(u8 *p,u16 n){p[0]=(u8)n;p[1]=(u8)(n>>8);}
static bool stopped(void){return __atomic_load_n(&stopping,__ATOMIC_ACQUIRE);}
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
        else if(!strcmp(key,"fps"))fps=(u32)strtoul(val,NULL,10);
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
u32 live_encoded_frames(unsigned sc){return sc<2?__atomic_load_n(&jpegFrames[sc],__ATOMIC_RELAXED):0;}
bool live_audio_active(void){return audioEnabled&&worker&&!stopped();}
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
static bool run_pair_job(unsigned kind,unsigned q) {
    jobKind=kind;jobQuality=q;
    __atomic_store_n(&jobDone,0,__ATOMIC_RELEASE);LightEvent_Signal(&events[1]);
    encode_job(0,kind,q);
    while(!__atomic_load_n(&jobDone,__ATOMIC_ACQUIRE)&&!stopped())LightEvent_WaitTimeout(&events[2],1000000);
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
static u32 pack_panel(unsigned sc,unsigned q) {
    size_t packed=compactJpeg?live_jpeg_wire_pack(pending[sc],MAX_JPEG,encoded[sc],
        jobSize[sc],sc?320:400,q):0;
    if(packed){compactFrames++;compactSavedBytes+=jobSize[sc]-packed;return (u32)packed;}
    memcpy(pending[sc],encoded[sc],jobSize[sc]);
    if(compactJpeg)compactFallbacks++;
    return jobSize[sc];
}

static u32 pair_payload_budget(u32 byteRate,u32 hz,bool combined) {
    u32 videoRate=live_av_video_rate(byteRate,audioEnabled);
    return combined?live_combined_budget(videoRate,hz):live_pair_budget(videoRate,hz);
}

/* Called by the network thread with lock held, between frame sends. A byte
 * ceiling changes the allowable JPEG size, not the meaning of its fragments. */
static void configure_stream(u32 hz,u32 bps,bool fec) {
    bool formatChanged=fps!=hz||combinedFec!=fec;
    if(!formatChanged&&rate==bps)return;
    if(formatChanged)configEpoch++;
    if(pendingReady&&(formatChanged||pendingLengths[0]+pendingLengths[1]>pair_payload_budget(bps,hz,fec))) {
        live_timing_drop_pending(TIMING_RECONFIGURED);
        pendingReady=false;
    } else if(pendingReady)ratePendingKept++;
    __atomic_store_n(&fps,hz,__ATOMIC_RELAXED);
    rate=bps;combinedFec=fec;configChanges++;
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
    u32 budgetEpoch=0,budgetRate=0;
    while(!stopped()) {
        if(!live_peer_ready()){svcSleepThread(10000000);continue;}
        u64 now=svcGetSystemTick();
        if(now<next){svcSleepThread((next-now)*1000000000ull/SYSCLOCK_ARM11);continue;}
        bool ready;u32 stamp=0,source[2]={0,0},wanted[2],jobRate,jobFps,epoch;
        LightLock_Lock(&lock);
        ready=planMask==3;wanted[0]=planPts[0];wanted[1]=planPts[1];stamp=planStamp;
        jobRate=rate;jobFps=fps;epoch=configEpoch;
        bool combined=combinedFec;
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
        u32 snapshotUs=(u32)((svcGetSystemTick()-snapshotBegin)*1000000/SYSCLOCK_ARM11);
        snapshotCount++;snapshotSumUs+=snapshotUs;if(snapshotUs>snapshotMaxUs)snapshotMaxUs=snapshotUs;
        u64 period=SYSCLOCK_ARM11/jobFps;next+=period;if(next<now)next=now+period;
        const u32 initialRate=jobRate;
        u32 budget=pair_payload_budget(jobRate,jobFps,combined);
        if(budgetEpoch!=epoch||budgetRate!=jobRate) {
            payloadControl=(LivePayloadControl){.percent=100};
            spare=0;
            budgetEpoch=epoch;
            budgetRate=jobRate;
        }
        u32 targetBytes=budget*payloadControl.percent/100;
        for(unsigned i=0;i<2;i++){sourceReuse[i]+=pairs&&source[i]==lastSource[i];lastSource[i]=source[i];jobUs[i]=0;}
        pairs++;u32 seq=++sequence;unsigned q=quality;
        live_timing_begin(seq,stamp);
        if(!run_pair_job(1,q))break;
        live_timing_encoded();
        /* Bounded requantization: reuse DCT, at most 3 entropy passes. */
        for(unsigned retry=0;retry<2;retry++) {
            /* A rate cut may arrive during the DCT. Reuse it for the normal
             * bounded entropy retries instead of abandoning the frame. */
            LightLock_Lock(&lock);
            u32 currentRate=rate;
            LightLock_Unlock(&lock);
            if(currentRate!=jobRate) {
                jobRate=currentRate;
                budget=pair_payload_budget(jobRate,jobFps,combined);
                targetBytes=budget*payloadControl.percent/100;
            }
            u32 total=payload_size(0,q,combined)+payload_size(1,q,combined);
            if(jobSize[0]&&jobSize[1]&&total<=budget)break;
            retries++;q=retry==1?1:live_lower_quality(q,total,budget);
            if(!run_pair_job(2,q))break;
            live_timing_encoded();
        }
        if(stopped())break;
        u32 payloadTotal=payload_size(0,q,combined)+payload_size(1,q,combined);
        bool fits=jobSize[0]&&jobSize[1]&&payloadTotal<=budget;
        for(unsigned i=0;i<2;i++) {
            attempts[i]++;sumUs[i]+=jobUs[i];changedMcus[i]+=jobChanged[i];
            if(jobUs[i]>maxUs[i])maxUs[i]=jobUs[i];
            __atomic_store_n(&lastEncodeUs[i],jobUs[i],__ATOMIC_RELAXED);
            if(fits){__atomic_add_fetch(&jpegFrames[i],1,__ATOMIC_RELAXED);if(jobSize[i]>maxBytes[i])maxBytes[i]=jobSize[i];}
        }
        quality=q;
        if(!fits){live_timing_drop_encoded(TIMING_OVERSIZE);oversize++;spare=0;continue;}
        /* A pair inside the wire cap is sent now. Adjust the next JPEG's
         * quality for queue pressure without an extra pass or dropping it. */
        if(payloadTotal>targetBytes){quality=live_lower_quality(q,payloadTotal,targetBytes);spare=0;}
        else if(payloadTotal<targetBytes*9/10){if(++spare>=4){quality=quality<83?quality+2:85;spare=0;}}
        else spare=0;
        LightLock_Lock(&lock);
        if(epoch==configEpoch&&(jobRate==rate||payloadTotal<=pair_payload_budget(rate,jobFps,combined))) {
            if(initialRate!=rate)rateEncodingKept++;
            live_payload_observe(&payloadControl,jobFps,pendingReady);
            if(pendingReady){live_timing_drop_pending(TIMING_REPLACED);replaced++;}
            for(unsigned i=0;i<2;i++) {
                /* Compact payloads are validated before the R10 budget check.
                 * Legacy mode retains its conservative full-JPEG budget. */
                pendingLengths[i]=pack_panel(i,q);
            }
            pendingSeq=seq;pendingStamp=stamp;pendingEpoch=epoch;pendingAt=svcGetSystemTick();pendingReady=true;
            live_timing_publish(pendingAt);
        } else live_timing_drop_encoded(TIMING_RECONFIGURED);
        LightLock_Unlock(&lock);
    }
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
    if(now-audioPreviousTick>SYSCLOCK_ARM11/4)audioPreviousCount=0;
    u8 packet[LIVE_AUDIO_MAX_PACKET_BYTES],fresh[LIVE_AUDIO_BATCH_BLOCKS*LIVE_AUDIO_RECORD_BYTES];
    LiveAudioCapture item;unsigned count=0;
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
        audioEncodedBlocks++;count++;
    }
    if(!count)return;
    memcpy(packet,"R9A1",4);put32(packet+4,session);put32(packet+8,++audioPacketSequence);
    put16(packet+12,(u16)(count+audioPreviousCount));put16(packet+14,0);
    put32(packet+16,LIVE_AUDIO_RATE_MHZ);put16(packet+20,LIVE_AUDIO_SAMPLES);put16(packet+22,LIVE_AUDIO_CHANNELS);
    memcpy(packet+LIVE_AUDIO_HEADER_BYTES,audioPrevious,audioPreviousCount*LIVE_AUDIO_RECORD_BYTES);
    memcpy(packet+LIVE_AUDIO_HEADER_BYTES+audioPreviousCount*LIVE_AUDIO_RECORD_BYTES,fresh,count*LIVE_AUDIO_RECORD_BYTES);
    const u32 bytes=LIVE_AUDIO_HEADER_BYTES+(count+audioPreviousCount)*LIVE_AUDIO_RECORD_BYTES;
    now=svcGetSystemTick();
    if(*due>now)svcSleepThread((*due-now)*1000000000ull/SYSCLOCK_ARM11);
    if(stopped())return;
    const u64 begin=svcGetSystemTick();
    const int sent=sendto(fd,packet,bytes,0,(struct sockaddr*)&target,sizeof target);
    *due=live_packet_due(*due,begin,bytes+28,rate,SYSCLOCK_ARM11);
    if(sent==(int)bytes){audioPacketsSent++;audioWireBytes+=bytes+28;}
    else audioSendErrors++;
    /* Retain even after a failed send: the next packet gets one more chance. */
    memcpy(audioPrevious,fresh,count*LIVE_AUDIO_RECORD_BYTES);audioPreviousCount=count;audioPreviousTick=begin;
}
static const char *format_address(u32 address,char *out) {
    struct in_addr ip={.s_addr=htonl(address)};
    return inet_ntop(AF_INET,&ip,out,INET_ADDRSTRLEN)?out:"?";
}

static void network_report(FILE *f) {
    char local[INET_ADDRSTRLEN],mask[INET_ADDRSTRLEN],destination[INET_ADDRSTRLEN],peer[INET_ADDRSTRLEN];
    fprintf(f,"session=%08lx\nautomatic=%u\npeer_ready=%u\nnetwork_polls=%lu\n"
            "local=%s\nmask=%s\nlast_probe_destination=%s\npeer=%s\nlocal_port=%u\nmedia_port=%u\n",
            session,automaticHost,mediaReady,netDiag.polls,format_address(localAddress,local),
            format_address(localMask,mask),format_address(netDiag.lastDestination,destination),
            format_address(ntohl(target.sin_addr.s_addr),peer),netDiag.localPort,ntohs(target.sin_port));
    fprintf(f,"ip_info_result=%d\nip_info_errno=%d\nsocket_errno=%d\nbind_errno=%d\n"
            "probe_attempts=%lu\nprobes_sent=%lu\nprobe_errno=%d\nreceive_errno=%d\n"
            "received_datagrams=%lu\naccepted_replies=%lu\ninvites=%lu\ninvites_answered=%lu\n",
            netDiag.infoResult,netDiag.infoErrno,netDiag.socketErrno,netDiag.bindErrno,netDiag.probeAttempts,
            netDiag.probesSent,netDiag.sendErrno,netDiag.receiveErrno,netDiag.replies,netDiag.accepted,
            netDiag.hints,netDiag.hintsAnswered);
}

static void save_network_report(void) {
    FILE *f=fopen("sdmc:/3ds/3ds-live/network-status.txt","w");
    if(f){network_report(f);fclose(f);}
    netDiag.savedAt=svcGetSystemTick();
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

static bool send_discovery_probe(const u8 *probe,u32 address) {
    struct sockaddr_in destination=target;
    destination.sin_addr.s_addr=htonl(address);
    netDiag.lastDestination=address;
    netDiag.probeAttempts++;
    int sent=sendto(fd,probe,LIVE_DISCOVERY_BYTES,0,(struct sockaddr*)&destination,sizeof destination);
    if(sent!=LIVE_DISCOVERY_BYTES){netDiag.sendErrno=errno;return false;}
    netDiag.probesSent++;
    netDiag.sendErrno=0;
    if(!netDiag.localPort) {
        struct sockaddr_in local={0};socklen_t size=sizeof local;
        if(getsockname(fd,(struct sockaddr*)&local,&size)==0)netDiag.localPort=ntohs(local.sin_port);
    }
    return true;
}

static void poll_discovery(u64 now) {
    bool expired=live_discover_expire(&discovery,now,SYSCLOCK_ARM11);
    if(!expired&&lastProbeTick&&now-lastProbeTick<SYSCLOCK_ARM11)return;
    lastProbeTick=now;
    netDiag.polls++;
    if(fd<0)fd=open_stream_socket();
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
        discovery.peer=0;
        discovery.outstanding=false;
        if(automaticHost)target.sin_addr.s_addr=0;
        configSequence=0;
        audioPreviousCount=0;
        if(fd>=0){close(fd);fd=-1;}
        netDiag.localPort=0;
        fd=open_stream_socket();
    }
    if(address) {
        if(address!=localAddress||netmask)localMask=netmask;
        localAddress=address;
    }
    netDiag.reportedBroadcast=ntohl(broadcast.s_addr);
    if(!mediaReady&&(netDiag.polls==2||now-netDiag.savedAt>=5ull*SYSCLOCK_ARM11))save_network_report();
    if(fd<0)return;
    if(!automaticHost) {
        bool usable=live_peer_address(ntohl(target.sin_addr.s_addr),localAddress,localMask);
        __atomic_store_n(&mediaReady,usable,__ATOMIC_RELEASE);
        return;
    }
    u8 probe[LIVE_DISCOVERY_BYTES];
    live_discover_probe(&discovery,session,probe);
    bool sent;
    if(discovery.peer)sent=send_discovery_probe(probe,discovery.peer);
    else {
        /* Routers and SOC implementations differ in handling subnet vs limited
         * broadcast. Both probes share one challenge and expose no media. */
        u32 subnet=netDiag.reportedBroadcast;
        if(!subnet&&localAddress&&localMask)subnet=live_broadcast_address(localAddress,localMask);
        sent=subnet&&subnet!=UINT32_MAX&&send_discovery_probe(probe,subnet);
        if(send_discovery_probe(probe,UINT32_MAX))sent=true;
    }
    if(!sent) {
        discovery.outstanding=false;
        /* Even if SOC refuses outbound broadcasts, the fixed listener must
         * stay open so a PC invitation can initiate the unicast handshake. */
        if(!discovery.peer&&netDiag.bindErrno){close(fd);fd=-1;netDiag.localPort=0;}
    }
}

static void control(u64 *due) {
    u64 now=svcGetSystemTick();
    poll_discovery(now);
    if(fd<0)return;
    if(now-lastControlTick>=SYSCLOCK_ARM11/100) {
        lastControlTick=now;
        for(unsigned i=0;i<4;i++) {
            u8 msg[64];struct sockaddr_in from;socklen_t size=sizeof from;
            int n=recvfrom(fd,msg,sizeof msg,0,(struct sockaddr*)&from,&size);
            if(n<0) {
                if(errno!=EAGAIN&&errno!=EWOULDBLOCK)netDiag.receiveErrno=errno;
                break;
            }
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
                if(send_discovery_probe(probe,address))netDiag.hintsAnswered++;
                continue;
            }
            if(n<8||get32(msg+4)!=session)continue;
            if(automaticHost&&n>=4&&!memcmp(msg,"3DLA",4)) {
                bool first=!discovery.peer;
                u32 address=ntohl(from.sin_addr.s_addr);
                if(live_reply_address(address,localAddress,localMask)&&address!=netDiag.reportedBroadcast&&
                   live_discover_ack(&discovery,address,session,msg,(size_t)n,now)) {
                    netDiag.accepted++;
                    if(first) {
                        target.sin_addr=from.sin_addr;
                        lastStatusTick=0;
                        __atomic_store_n(&mediaReady,true,__ATOMIC_RELEASE);
                        save_network_report();
                    }
                }
                continue;
            }
            if(!mediaReady||from.sin_addr.s_addr!=target.sin_addr.s_addr)continue;
            if(n==28&&!memcmp(msg,"R3FB",4))feedbackCount++;
            if((n==20&&!memcmp(msg,"R6CF",4))||(n==24&&!memcmp(msg,"R10C",4))) {
                u32 seq=get32(msg+8),hz=get32(msg+12),bps=get32(msg+16);
                bool nextCombined=n==24?get32(msg+20)!=0:combinedFec;
                if(!live_valid_fps(hz)||bps<LIVE_MIN_RATE||bps>LIVE_MAX_RATE||!seq)continue;
                if(n==24&&get32(msg+20)>1)continue;
                if(configSequence&&(u32)(seq-configSequence)>=0x80000000u)continue;
                configSequence=seq;
                LightLock_Lock(&lock);
                configure_stream(hz,bps,nextCombined);
                LightLock_Unlock(&lock);lastStatusTick=0;
            }
        }
    }
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
    if(*due>now)svcSleepThread((*due-now)*1000000000ull/SYSCLOCK_ARM11);
    u64 begin=svcGetSystemTick();sendto(fd,msg,msgBytes,0,(struct sockaddr*)&target,sizeof target);
    *due=live_packet_due(*due,begin,msgBytes+28,rate,SYSCLOCK_ARM11);lastStatusTick=begin;
#ifdef LIVE_FRAME_TIMING
    u32 capacity;
    if(live_timing_marker(begin,&capacity)) {
        u8 marker[24];memcpy(marker,"R12D",4);put32(marker+4,session);put32(marker+8,1);put32(marker+12,capacity);
        u64 nowTick=svcGetSystemTick();
        if(*due>nowTick)svcSleepThread((*due-nowTick)*1000000000ull/SYSCLOCK_ARM11);
        u64 call=svcGetSystemTick();
        put32(marker+16,(u32)call);put32(marker+20,(u32)(call>>32));
        sendto(fd,marker,sizeof marker,0,(struct sockaddr*)&target,sizeof target);
        *due=live_packet_due(*due,call,sizeof marker+28,rate,SYSCLOCK_ARM11);
    }
#endif
}
static bool send_packet(u64 *due,u64 deadline,const char *magic,u32 seq,u32 stamp,
                        u32 sc,u32 total,u32 offset,const u8 *data,u32 len) {
    send_audio(due);
    u64 now=svcGetSystemTick();
    if(*due>now)svcSleepThread((*due-now)*1000000000ull/SYSCLOCK_ARM11);
    if(stopped()||svcGetSystemTick()>deadline){
        live_timing_finish(stopped()?TIMING_STOPPED:TIMING_DEADLINE);
        return false;
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
    *due=live_packet_due(*due,begin,len+header+28,rate,SYSCLOCK_ARM11);
    if(sent!=(int)(len+header)){lastErrno=errno;errors++;live_timing_finish(TIMING_SOCKET_ERROR);return false;}return true;
}
static void sender(void *unused) {
    (void)unused;u64 due=svcGetSystemTick();
    while(!stopped()) {
        control(&due);
        if(!mediaReady){svcSleepThread(10000000);continue;}
        send_audio(&due);
        u32 sizes[2]={0,0},seq=0,stamp=0;u64 queued=0;bool ready;
        LightLock_Lock(&lock);ready=pendingReady&&pendingEpoch==configEpoch;
        if(ready){live_timing_claim();for(unsigned i=0;i<2;i++){sizes[i]=pendingLengths[i];memcpy(sending[i],pending[i],sizes[i]);}
            seq=pendingSeq;stamp=pendingStamp;queued=pendingAt;pendingReady=false;}
        LightLock_Unlock(&lock);
        if(!ready){svcSleepThread(500000);continue;}
#ifdef LIVE_FRAME_TIMING
        u32 dataCount=combinedFec?(sizes[0]+sizes[1]+LIVE_CHUNK-1)/LIVE_CHUNK:
            (sizes[0]+LIVE_CHUNK-1)/LIVE_CHUNK+(sizes[1]+LIVE_CHUNK-1)/LIVE_CHUNK;
        live_timing_plan(dataCount,dataCount+(combinedFec?LIVE_PAIR_PARITY:2*LIVE_PARITY));
#endif
        u64 begin=svcGetSystemTick(),deadline=live_pair_deadline(begin,SYSCLOCK_ARM11);
        if(begin-queued>SYSCLOCK_ARM11/fps*2){expired++;live_timing_finish(TIMING_TOO_OLD);continue;}
        u8 parity[LIVE_PAIR_PARITY][LIVE_CHUNK]={0};bool ok=true;
        if(combinedFec) {
            u8 fragment[LIVE_CHUNK];u32 count=(sizes[0]+sizes[1]+LIVE_CHUNK-1)/LIVE_CHUNK;
            for(u32 index=0;ok&&index<count;index++) {
                u32 len=live_pair_fragment(fragment,sending[0],sizes[0],sending[1],sizes[1],index);
                u8 *stripe=parity[index%LIVE_PAIR_PARITY];
                for(u32 j=0;j<len;j++)stripe[j]^=fragment[j];
                ok=send_packet(&due,deadline,"R10P",seq,stamp,sizes[0],sizes[1],index,fragment,len);
            }
            for(unsigned order=0;order<LIVE_PAIR_PARITY&&ok;order++) {
                unsigned stripe=(count+order)%LIVE_PAIR_PARITY;
                ok=send_packet(&due,deadline,"R10F",seq,stamp,sizes[0],sizes[1],stripe,parity[stripe],LIVE_CHUNK);
            }
        } else {
            for(u32 off=0;ok&&(off<sizes[0]||off<sizes[1]);off+=LIVE_CHUNK)for(unsigned sc=0;sc<2&&ok;sc++) {
                if(off>=sizes[sc])continue;
                u32 len=sizes[sc]-off>LIVE_CHUNK?LIVE_CHUNK:sizes[sc]-off;
                u8 *stripe=parity[sc*LIVE_PARITY+off/LIVE_CHUNK%LIVE_PARITY];
                for(u32 j=0;j<len;j++)stripe[j]^=sending[sc][off+j];
                ok=send_packet(&due,deadline,"R3P2",seq,stamp,sc,sizes[sc],off,sending[sc]+off,len);
            }
            for(unsigned stripe=0;stripe<LIVE_PARITY&&ok;stripe++)for(unsigned sc=0;sc<2&&ok;sc++)
                ok=send_packet(&due,deadline,"R3F2",seq,stamp,sc,sizes[sc],stripe,parity[sc*LIVE_PARITY+stripe],LIVE_CHUNK);
        }
        if(ok){frames[0]++;if(sizes[1])frames[1]++;live_timing_finish(TIMING_SENT);}else expired++;
    }
}
Result live_start(void *(*allocate)(u32 bytes)) {
    if(!allocate||worker||encoders[0]||encoders[1])return -1;
    LightLock_Init(&lock);for(unsigned i=0;i<2;i++)LightLock_Init(&rawLocks[i]);
    for(unsigned i=0;i<3;i++)LightEvent_Init(&events[i],RESET_ONESHOT);
    started=svcGetSystemTick();stopping=false;jobDone=1;pendingReady=false;planMask=0;
    memset(rawValid,0,sizeof rawValid);memset(attempts,0,sizeof attempts);memset(jpegFrames,0,sizeof jpegFrames);
    memset(maxUs,0,sizeof maxUs);memset(maxBytes,0,sizeof maxBytes);memset(sumUs,0,sizeof sumUs);
    memset(convertCount,0,sizeof convertCount);memset(convertMaxUs,0,sizeof convertMaxUs);memset(convertSumUs,0,sizeof convertSumUs);
    memset(convertedMcus,0,sizeof convertedMcus);snapshotCount=snapshotMaxUs=0;snapshotSumUs=0;
    memset(changedMcus,0,sizeof changedMcus);memset(sourceReuse,0,sizeof sourceReuse);memset(lastSource,0,sizeof lastSource);
    memset(frames,0,sizeof frames);memset(lastEncodeUs,0,sizeof lastEncodeUs);
    sequence=replaced=errors=expired=oversize=retries=pairs=sendCalls=sendMaxUs=feedbackCount=0;
    configEpoch=configSequence=configChanges=ratePendingKept=rateEncodingKept=0;sendSumUs=0;lastErrno=0;quality=60;spare=0;
    payloadControl=(LivePayloadControl){.percent=100};
    compactSavedBytes=0;compactFrames=compactFallbacks=0;
    live_audio_queue_init(&audioQueue);live_audio_init(&audioCodec);audioPreviousCount=0;audioPreviousTick=0;
    audioPacketSequence=audioPacketsSent=audioSendErrors=audioEncodedBlocks=audioEncodeMaxUs=0;
    audioEncodeSumUs=audioWireBytes=0;
    session=(u32)started;if(!session)session=1;
    lastStatusTick=lastControlTick=started;
    discovery=(LiveDiscovery){.challenge=(u32)(started>>16)};
    lastProbeTick=0;
    localAddress=localMask=0;
    memset(&netDiag,0,sizeof netDiag);
    __atomic_store_n(&mediaReady,false,__ATOMIC_RELEASE);
    save_network_report();
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
#ifdef LIVE_FRAME_TIMING
    bool timingSaved=live_timing_export(session,started);
#endif
    if(fd>=0){close(fd);fd=-1;}if(socReady){socExit();socReady=false;}
    for(unsigned i=0;i<2;i++) {
        live_jpeg_reset(i);live_jpeg_source_destroy(rawSources[i]);rawSources[i]=NULL;
        rawWorking[i]=NULL; /* Caller releases the complete SYSTEM arena. */
    }
    FILE *f=fopen("sdmc:/3ds/3ds-live/live-stats.txt","w");if(!f)return;
    network_report(f);
#ifdef LIVE_FRAME_TIMING
    fprintf(f,"timing_session=%08lx\ntiming_saved=%u\n",(unsigned long)session,timingSaved?1u:0u);
#endif
    fprintf(f,"version=%s\nsent_top=%lu\nsent_bottom=%lu\nreplaced=%lu\nsend_errors=%lu\nrate=%lu\nfps=%lu\n",
        combinedFec?"live-r10-fec4-candidate":"live-r9-audio",frames[0],frames[1],replaced,errors,rate,fps);
    fprintf(f,"duration_ms=%llu\nexpired_pairs=%lu\nlast_send_errno=%d\nadaptive=0\npair_jpeg_budget=%lu\n",
        (svcGetSystemTick()-started)*1000/SYSCLOCK_ARM11,expired,lastErrno,(u32)live_pair_budget(live_av_video_rate(rate,audioEnabled),fps));
    fprintf(f,"payload_target_percent=%u\n",payloadControl.percent);
    fprintf(f,"combined_fec=%u\npair_payload_budget=%lu\n",combinedFec?1u:0u,
        (u32)(combinedFec?live_combined_budget(live_av_video_rate(rate,audioEnabled),fps):
                          live_pair_budget(live_av_video_rate(rate,audioEnabled),fps)));
    fprintf(f,"rate_pending_kept=%lu\nrate_encoding_kept=%lu\n",ratePendingKept,rateEncodingKept);
    fprintf(f,"pairs_attempted=%lu\npairs_oversize=%lu\nrequantize_passes=%lu\nquality=%u\nfec_stripes=%u\nconfig_changes=%lu\n",
        pairs,oversize,retries,quality,combinedFec?LIVE_PAIR_PARITY:LIVE_PARITY,configChanges);
    fprintf(f,"fec_scope=%s\nbudget_basis=%s\n",combinedFec?"pair":"screen",combinedFec?"wire_payload":"full_jpeg");
    fprintf(f,"send_calls=%lu\nsend_mean_us=%llu\nsend_max_us=%lu\nfeedback_count=%lu\n",
        sendCalls,sendCalls?sendSumUs/sendCalls:0,sendMaxUs,feedbackCount);
    fprintf(f,"send_timeout_ms=%u\n",LIVE_SEND_TIMEOUT_MS);
    fprintf(f,"source_layout=ycbcr420_mcu\nsnapshot_mean_us=%llu\nsnapshot_max_us=%lu\n",
        snapshotCount?snapshotSumUs/snapshotCount:0,snapshotMaxUs);
    fprintf(f,"compact_jpeg=%u\ncompact_frames_queued=%lu\ncompact_bytes_saved_queued=%llu\ncompact_fallbacks=%lu\n",
        compactJpeg?1:0,compactFrames,compactSavedBytes,compactFallbacks);
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
        "screen%u_convert_mean_us=%llu\nscreen%u_convert_max_us=%lu\nscreen%u_converted_mcus=%llu\n",
        i,attempts[i],i,jpegFrames[i],i,attempts[i]?sumUs[i]/attempts[i]:0,i,maxUs[i],i,maxBytes[i],i,encoderCores[i],
        i,changedMcus[i],i,sourceReuse[i],i,convertCount[i]?convertSumUs[i]/convertCount[i]:0,i,convertMaxUs[i],i,convertedMcus[i]);
    fclose(f);
}
bool live_submit(int sc,const u8 *frame,u32 fmt,u32 stride,u32 pts,bool wide) {
    if(sc<0||sc>1||!worker||stopped()||wide)return false;
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
