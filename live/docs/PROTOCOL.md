# 3DS Live UDP protocol

The New 3DS sends dual-screen JPEGs, stereo game audio and device status to the Qt receiver over IPv4 UDP. The default PC port is **8001**. All integer fields in the application payload use little-endian byte order. All tags below are four ASCII bytes.

## LAN discovery

In automatic mode the handheld listens on UDP 8002 when available. If the port is occupied, it continues with an ephemeral source port. While searching, it sends a probe approximately once per second to the configured PC port using the subnet broadcast address when known and the limited broadcast address. The same socket receives replies and carries the subsequent unicast stream.

While waiting for a peer, the PC also sends an eight-byte invitation from its bound media port to UDP 8002. The invitation consists of `3DLP` followed by a little-endian uint32 version of 1. An unpaired handheld responds with its regular probe by unicast to the inviting PC. Both discovery directions then use the same exchange below.

| Offset | Length | Probe from handheld | Response from PC |
| --- | --- | --- | --- |
| 0 | 4 bytes | `3DLS` | `3DLA` |
| 4 | 4 bytes | Stream session (uint32) | Copy of probe session |
| 8 | 4 bytes | Current challenge (uint32) | Copy of probe challenge |

Both datagrams are exactly 12 bytes. The receiver sends the response from its bound media port to the observed source IP and source port. The handheld checks the source port, session and outstanding challenge. The first valid response selects the PC. While paired it sends the same probe by unicast with a fresh challenge and accepts the response only from the selected PC. Media and status travel by unicast.

After four seconds without a valid response the handheld expires the peer and resumes discovery. A change to its local IP or mask also clears the peer and causes a rebind. The PC reserves an endpoint after it replies and accepts media for that discovery session. A timely heartbeat renews the reservation. A different device can take over once the previous reservation and media activity have expired. An explicitly configured IPv4 filter restricts which handheld the PC answers.

Discovery and streaming work on a trusted IPv4 LAN that passes broadcast between devices. The challenge correlates a response to the current probe. It carries no credentials. With an explicit unicast PC address in the handheld configuration, the handheld can send directly to a receiver that predates discovery.

## Video

The default combined transport sends the upper and lower JPEGs consecutively as one byte stream. Each display can update as soon as its JPEG has arrived. A data fragment carries up to 1168 bytes. A parity fragment carries exactly 1168 bytes, including zero padding for XOR.

| Offset | Type | Combined data `R10P` / parity `R10F` | Per-screen data `R3P2` / parity `R3F2` |
| --- | --- | --- | --- |
| 0 | char[4] | Tag | Tag |
| 4 | uint32 | Session | Session |
| 8 | uint32 | Frame pair sequence | Frame pair sequence |
| 12 | uint32 | Capture PTS, µs | Capture PTS, µs |
| 16 | uint32 | Upper JPEG wire length | Screen, 0 upper or 1 lower |
| 20 | uint32 | Lower JPEG wire length | JPEG wire length |
| 24 | uint32 | Data chunk index or parity stripe index | Data byte offset or parity stripe index |
| 28 | uint32 | Configured FPS | Configured FPS |
| 32 | bytes | Fragment payload | Fragment payload |

Combined mode XORs data chunk `i` into stripe `i % 4`. After the data, stripe order is `(data_chunk_count + n) % 4` for `n` from 0 through 3. Per-screen mode uses two XOR stripes independently on each screen. One missing fragment in a stripe can be recovered from that stripe’s parity and its remaining fragments. Both formats support 15, 20, 24 and 30 FPS. Fields describing lengths count the transmitted image representation and allow up to 128000 bytes per screen.

Each screen carries a full JPEG starting with `FF D8` or a compact payload. The compact layout starts with `R8J1`, an 8-bit layout version (1), an 8-bit quantization quality (1–95), and a uint16 width (400 or 320). The compressed scan follows at byte 8 and ends with `FF D9`. Layout 1 fixes height at 240, sampling at 4:2:0 and restart interval at width / 16. The receiver rebuilds the 629-byte JPEGENC header from quality and width, saving 621 bytes per compact screen on the wire. The sender can use either representation on any frame.

## Stereo audio

`R9A1` shares the video session and capture clock. Its 24-byte header contains the tag, uint32 session, uint32 datagram sequence, uint16 record count (1–6), uint16 flags (0), uint32 sample rate in millihertz (32728498), uint16 samples per channel (160) and uint16 channels (2).

Each 175-byte record contains a uint32 block sequence, uint32 capture PTS in µs and 167 bytes of stereo IMA audio. The left and right channel headers each contain an int16 initial sample, uint8 IMA index and uint8 zero. The following 159 bytes store one four-bit code per channel per sample, left in the low nibble. A datagram normally includes up to three fresh blocks and up to three blocks repeated from the previous batch. The receiver deduplicates by block sequence and uses the timestamps to align audio and video.

## Status and configuration

The handheld sends `R7ST` (76 bytes) for video-only streaming or `R9ST` (108 bytes) with audio, approximately once per second. Following the tag, all fields are uint32, in this order.

1. Session, elapsed milliseconds, total wire-rate cap, video budget and capture FPS.
2. Encoded upper/lower counts, sent upper/lower counts, send errors, expired frames and accepted feedback count.
3. Upper/lower encoder CPU core, last upper/lower encode time in µs, maximum supported wire rate and supported FPS bitmask.
4. `R9ST` adds audio enabled, sample rate in millihertz, channels, samples per block, queued blocks, dropped blocks, packets sent and queue depth.

`R6CF` is a 20-byte configuration request containing the tag, session, request sequence, FPS, and total IPv4 bytes per second. `R10C` adds a uint32 combined-FEC flag (0 or 1) for a total of 24 bytes. The device validates the selected peer, stream session, sequence and supported settings, then applies changes between frame pairs. Status reports the settings in effect. The standard independent sender supports 400000–2000000 IPv4 bytes per second.

The receiver returns `R3FB` (28 bytes) with six uint32 fields after the tag: session, feedback sequence, received video packets, complete screen count, incomplete screen count and relative arrival-delay growth in milliseconds. The handheld counts feedback from the active receiver. Qt adjusts its requested rate using observed stream quality.

Earlier sender formats remain readable by Qt, including single-screen `R3LP` and `R3ST` status. Qt also understands optional `R13S` diagnostic counters and `R15S` / `R15P` / `R15F` upper-priority streams from those sender versions.
