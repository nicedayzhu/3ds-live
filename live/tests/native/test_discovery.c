#include <assert.h>
#include "live_discovery.h"

static void make_ack(LiveDiscovery *state, uint32_t session, uint8_t *packet) {
    live_discover_probe(state,session,packet);
    packet[3]='A';
}

int main(void) {
    const uint32_t first_pc=0xc0a80102u, next_pc=0xc0a80103u;
    const uint32_t session=0x12345678u;
    LiveDiscovery discovery={.challenge=0x87654320u};
    uint8_t probe[LIVE_DISCOVERY_BYTES];
    const uint8_t expected[]={'3','D','L','S',0x78,0x56,0x34,0x12,0x21,0x43,0x65,0x87};
    live_discover_probe(&discovery,session,probe);
    assert(memcmp(probe,expected,sizeof expected)==0);
    assert(!live_discover_ack(&discovery,first_pc,session,probe,sizeof probe,0));
    probe[3]='A';
    assert(!live_discover_ack(&discovery,first_pc,session+1,probe,sizeof probe,0));
    assert(!live_discover_ack(&discovery,first_pc,session,probe,sizeof probe-1,0));
    assert(!live_discover_ack(&discovery,first_pc,session,probe,sizeof probe+1,0));
    probe[8]^=1;
    assert(!live_discover_ack(&discovery,first_pc,session,probe,sizeof probe,0));
    probe[8]^=1;
    assert(live_discover_ack(&discovery,first_pc,session,probe,sizeof probe,1));
    assert(!live_discover_ack(&discovery,first_pc,session,probe,sizeof probe,2));
    assert(discovery.last_ack==1);

    make_ack(&discovery,session,probe);
    assert(!live_discover_ack(&discovery,next_pc,session,probe,sizeof probe,3));
    assert(live_discover_ack(&discovery,first_pc,session,probe,sizeof probe,4));
    make_ack(&discovery,session,probe);
    assert(!live_discover_expire(&discovery,43,10));
    assert(live_discover_expire(&discovery,44,10));
    assert(!live_discover_ack(&discovery,first_pc,session,probe,sizeof probe,45));
    assert(discovery.peer==0);
    make_ack(&discovery,session,probe);
    assert(live_discover_ack(&discovery,next_pc,session,probe,sizeof probe,46));
    assert(discovery.peer==next_pc);
    /* A valid same-session control waiting in the socket on a delayed poll
     * must renew the current peer before the deadline check. */
    live_discover_activity(&discovery,first_pc,90);
    assert(discovery.last_ack==46);
    live_discover_activity(&discovery,next_pc,90);
    assert(!live_discover_expire(&discovery,90,10));
    assert(!live_discover_expire(&discovery,129,10));
    assert(live_discover_expire(&discovery,130,10));
    live_discover_activity(&discovery,next_pc,131);
    assert(discovery.peer==0);

    discovery.challenge=UINT32_MAX;
    live_discover_probe(&discovery,session,probe);
    assert(discovery.challenge==1);

    const uint32_t local=0xc0a80101u, mask=0xffffff00u;
    assert(!live_peer_address(first_pc,0,mask));
    assert(!live_peer_address(first_pc,local,0));
    assert(live_reply_address(first_pc,local,0));
    assert(live_reply_address(first_pc,0,0));
    assert(!live_reply_address(0xffffffffu,0,0));
    assert(!live_reply_address(local,local,0));
    assert(!live_reply_address(0xc0a801ffu,local,mask));
    assert(!live_peer_address(local,local,mask));
    assert(!live_peer_address(0xc0a80100u,local,mask));
    assert(!live_peer_address(0xc0a801ffu,local,mask));
    assert(!live_peer_address(0xffffffffu,local,mask));
    assert(!live_peer_address(0xe0000001u,local,mask));
    assert(live_peer_address(first_pc,local,mask));
    /* The PC's .255 host is usable on /23, but is broadcast after DHCP changes to /24. */
    assert(live_peer_address(0xc0a800ffu,0xc0a80001u,0xfffffe00u));
    assert(!live_peer_address(0xc0a800ffu,0xc0a80001u,mask));
    assert(live_broadcast_address(local,mask)==0xc0a801ffu);
    assert(live_broadcast_address(local,0xfffffffeu)==UINT32_MAX);

    assert(live_config_port("8001")==8001);
    assert(live_config_port("65535")==65535);
    assert(live_config_port("65536")==0);
    assert(live_config_port("4294967297")==0);
    assert(live_config_port("9000x")==0);
    assert(live_config_port("-1")==0);
    assert(live_config_port("")==0);

    uint8_t invitation[]={'3','D','L','P',1,0,0,0};
    assert(live_discover_invitation(invitation,sizeof invitation));
    assert(!live_discover_invitation(invitation,sizeof invitation-1));
    invitation[4]=2;
    assert(!live_discover_invitation(invitation,sizeof invitation));
    uint8_t invited[LIVE_DISCOVERY_BYTES];
    discovery=(LiveDiscovery){0};
    live_discover_invited_probe(&discovery,session,probe);
    assert(discovery.outstanding&&discovery.challenge!=0);
    live_discover_invited_probe(&discovery,session,invited);
    assert(memcmp(probe,invited,sizeof invited)==0);
    live_discover_probe(&discovery,session,probe);
    assert(memcmp(probe,invited,sizeof invited)!=0);
    return 0;
}
