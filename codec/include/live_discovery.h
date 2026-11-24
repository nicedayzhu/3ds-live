#ifndef LIVE_DISCOVERY_H
#define LIVE_DISCOVERY_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LIVE_DISCOVERY_BYTES 12
#define LIVE_DISCOVERY_PORT 8002
#define LIVE_PEER_LEASE_SECONDS 4

/* peer is a host-order IPv4 address. Zero means we are looking for a PC. */
typedef struct {
    uint32_t peer;
    uint32_t challenge;
    uint64_t last_ack;
    bool outstanding;
} LiveDiscovery;

/* An invalid configured port must not wrap into a valid service port. */
static inline uint16_t live_config_port(const char *text) {
    unsigned long value=0;
    for (;*text;++text) {
        if (*text<'0'||*text>'9') return 0;
        value=value*10+(unsigned)(*text-'0');
        if (value>65535) return 0;
    }
    return (uint16_t)value;
}

static inline bool live_unicast_address(uint32_t address) {
    unsigned first_octet = address >> 24;
    return first_octet != 0 && first_octet != 127 && first_octet < 224;
}

/* Addresses here use host byte order. /31 and /32 have no directed broadcast. */
static inline uint32_t live_broadcast_address(uint32_t local, uint32_t mask) {
    return ~mask > 1 ? (local | ~mask) : UINT32_MAX;
}

static inline bool live_peer_address(uint32_t address, uint32_t local, uint32_t mask) {
    if (!local || !mask || !live_unicast_address(address) || address == local)
        return false;
    return ~mask <= 1 || (address != (local & mask) && address != (local | ~mask));
}

/* A reply observed by recvfrom can identify a peer even when SOC hasn't
 * supplied the mask. A user-entered destination still needs full validation. */
static inline bool live_reply_address(uint32_t address, uint32_t local, uint32_t mask) {
    return local && mask ? live_peer_address(address,local,mask) :
           live_unicast_address(address) && address!=local;
}

static inline void live_discovery_fields(uint8_t *packet, uint32_t session, uint32_t challenge) {
    for (unsigned i = 0; i < 4; ++i) {
        packet[4+i] = (uint8_t)(session >> (8*i));
        packet[8+i] = (uint8_t)(challenge >> (8*i));
    }
}

static inline void live_discover_probe(LiveDiscovery *state, uint32_t session,
                                       uint8_t packet[LIVE_DISCOVERY_BYTES]) {
    if (++state->challenge == 0) ++state->challenge;
    state->outstanding = true;
    memcpy(packet, "3DLS", 4);
    live_discovery_fields(packet,session,state->challenge);
}

static inline bool live_discover_invitation(const uint8_t *packet,size_t length) {
    static const uint8_t expected[]={'3','D','L','P',1,0,0,0};
    return length==sizeof expected&&!memcmp(packet,expected,sizeof expected);
}

/* Multiple receivers can invite the same sender. Return the outstanding
 * broadcast challenge until the next scheduled probe rotates it. */
static inline void live_discover_invited_probe(LiveDiscovery *state,uint32_t session,
                                              uint8_t packet[LIVE_DISCOVERY_BYTES]) {
    if(!state->outstanding)live_discover_probe(state,session,packet);
    else {
        memcpy(packet,"3DLS",4);
        live_discovery_fields(packet,session,state->challenge);
    }
}

static inline bool live_discover_ack(LiveDiscovery *state, uint32_t address,
                                     uint32_t session, const uint8_t *packet,
                                     size_t length, uint64_t now) {
    if (!state->outstanding || length != LIVE_DISCOVERY_BYTES ||
        (state->peer && state->peer != address))
        return false;
    uint8_t expected[LIVE_DISCOVERY_BYTES] = {'3','D','L','A'};
    live_discovery_fields(expected,session,state->challenge);
    if (memcmp(packet,expected,sizeof expected)) return false;
    state->peer = address;
    state->last_ack = now;
    state->outstanding = false;
    return true;
}

static inline bool live_discover_expire(LiveDiscovery *state, uint64_t now,
                                        uint64_t ticks_per_second) {
    if (!state->peer || now-state->last_ack < LIVE_PEER_LEASE_SECONDS*ticks_per_second)
        return false;
    state->peer = 0;
    state->outstanding = false;
    return true;
}

/* A valid control from the paired media endpoint proves the PC is still
 * there even if a unicast discovery probe or its specific reply was lost. */
static inline void live_discover_activity(LiveDiscovery *state, uint32_t address, uint64_t now) {
    if (state->peer && state->peer == address) state->last_ack = now;
}

#endif
