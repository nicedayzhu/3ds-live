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

Both datagrams are exactly 12 bytes. The receiver sends the response from its bound media port to the observed source IP and source port. The handheld checks the source port, session and outstanding challenge. The first valid response selects the PC. A paired sender probes by unicast, repeating an outstanding challenge until acknowledged while control traffic continues. Media and status travel by unicast.

Version 0.1.10 drains queued controls before the four-second lease check. A valid discovery reply or recognized control from the selected endpoint and session renews the lease. Expiry clears peer-specific references and pending repairs. The selected capture rate, byte ceiling and JPEG target persist until a receiver requests a change. The handheld checks the route when searching or when control traffic stops. A detected local address or mask change clears the peer and causes a rebind. The PC reserves an endpoint after it replies and accepts media for that discovery session. A different device can take over once the previous reservation and media activity have expired. An explicitly configured IPv4 filter restricts which handheld the PC answers.

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

Combined mode XORs data chunk `i` into stripe `i % 4`. After the data, stripe order is `(data_chunk_count + n) % 4` for `n` from 0 through 3. Per-screen mode uses two XOR stripes independently on each screen. The sender skips stripes containing zero chunks, so a one-chunk combined pair uses one data packet and one full-size parity packet. One missing fragment in a stripe can be recovered from that stripe’s parity and its remaining fragments. Both formats support 15, 20, 24 and 30 FPS. Fields describing lengths count the transmitted image representation and allow up to 128000 bytes per screen.

Version 0.1.11 used `R20F` to advertise experimental 35 and 40 FPS modes. Current receivers accept its 35/40 FPS media and status records during playback. Current senders advertise the four standard rates, and configuration requests use those rates.

Each screen carries a full JPEG starting with `FF D8` or a compact payload. The compact layout starts with `R8J1`, an 8-bit layout version (1), an 8-bit quantization quality (1–95), and a uint16 width (400 or 320). The compressed scan follows at byte 8 and ends with `FF D9`. Layout 1 fixes height at 240, sampling at 4:2:0 and restart interval at width / 16. The receiver rebuilds the 629-byte JPEGENC header from quality and width, saving 621 bytes per compact screen on the wire. The sender can use either representation on any frame.

### Exact JPEG row reuse

The handheld advertises support with `R17S` (session and version 1 as uint32). The receiver replies with `R17A` (session, flags, reference sequence as uint32) from its media socket. Flags 0 requests a reference and flags 1 acknowledges a complete validated reference pair. Sequence zero is valid. Only negotiated combined streams use row references. A peer change or expired negotiation requires a fresh reference. Independent screens continue to work with earlier receivers.

A candidate reference uses `R17K` in place of the four-byte `R8J1` tag on both screens. Its outer pair sequence identifies it. The sender may repeat candidate packets with the same sequence. Each reference is independently decodable. Each differential screen refers directly to an acknowledged pair, so a dropped differential frame does not invalidate the next one.

The 20-byte `R17D` header contains the tag, uint8 layout, uint8 Q, uint16 width, uint32 reference sequence, uint16 changed-row mask (bits 0–14), uint16 reserved (zero), and uint32 reconstructed compact JPEG size. For each set bit in increasing order, a uint32 length precedes that row's exact entropy bytes including its restart marker. Reference rows must have matching Q and width. All 15 rows carry restart markers, including the last. The receiver appends the final EOI. It validates each changed row and the total size before publishing the screen. An unchanged screen requires 20 bytes. A screen that cannot be represented more compactly can use an independent JPEG in the same pair.

### Selective repair of combined video

An R19 sender advertises selective repair with its periodic status when combined FEC is enabled. Requests and replies use the paired media IP, UDP port and stream session. The existing `R10P` data and `R10F` parity formats also work with earlier peers.

| Tag | Direction | UDP payload length | Fields after tag, in order |
| --- | --- | --- | --- |
| `R19S` | Handheld → PC | 16 bytes | uint32 session, version (1), maximum data indices per request (1–16, current sender 16) |
| `R19A` | PC → handheld | 12 bytes | uint32 session, version (1) |
| `R19N` | PC → handheld | 20 + count bytes | uint32 session, pair sequence, capture PTS in µs, count, then one uint8 per requested data chunk index |
| `R19P` | Handheld → PC | 32 + chunk length bytes | Original `R10P` header fields and original data bytes for the requested index |

The receiver answers each valid offer. While an acknowledgement is less than four seconds old, the handheld sends each fresh `R17K` candidate once and services explicit fragment requests. Earlier receivers retain the scheduled candidate repeats. The receiver first accounts for recovery possible with its XOR parity. It requests at most one list of distinct indices for a partially received pair. If the whole pair exceeds the advertised limit, it tries the upper screen, then the lower screen. A completely missing pair has no header from which to construct a request.

Qt uses parity, the final data fragment or a newer pair as evidence that the original data have been sent. Mid-frame silence with bounded holes can trigger an upper-screen repair, and a prolonged pause with at most two trailing fragments missing can trigger a tail repair. At the default 100 ms playout delay, a requested fragment can complete its original frame for up to 65 ms after the request and 90 ms after the first packet. A newer complete frame waits at most one source period plus 6 ms, capped at 50 ms and the available playout budget. The receiver retires older frames after releasing a newer complete pair and counts expired, retired and duplicate repairs separately.

The handheld keeps up to eight recent pairs in the existing 256,000-byte transmit buffer for at most 250 ms from reservation. Large pairs can overwrite older bytes sooner. Each entry accepts one `R19N`. A valid request has up to 45 ms from receipt to send its requested fragments, bounded by the original entry's expiry. The sender services the earliest deadline among entries whose original transmission completed. Each fresh frame grants spare repair bytes from 96% of its video-rate budget after accounting for its data and parity. Repair shares the total IPv4 pacing clock with audio and video. A successful `R19P` adds its data length plus 60 bytes to both the repair-byte and video-byte counters. The handheld's `live-stats.txt` counts requests for absent or expired entries, duplicate and invalid requests, and unsent fragments that expired or were evicted.

### Lossless lower-screen compression

In combined row-reference mode, the handheld advertises `R20S` and a compatible receiver responds with `R20A`. Both are 12 bytes including a uint32 session and version 1. When compression saves bytes and the encoder has time, the handheld can transmit a lower-screen `R17D` as `R20D`. Its 20-byte header contains the tag, uint8 layout 1, uint8 JPEG Q, uint16 width 320, uint32 reference sequence, uint32 uncompressed R17D length and uint32 zero. A raw LZ4 block follows. The dictionary consists of the last 32 KiB of the matching compact lower-screen key after its first eight bytes, including EOI. A shorter key contributes all bytes after that header. The receiver validates the inner header and reconstructs exactly the original JPEG rows. Other screen representations and the upper screen retain their existing formats.


## Stereo audio

`R9A1` shares the video session and capture clock. Its 24-byte header contains the tag, uint32 session, uint32 datagram sequence, uint16 record count (1–6), uint16 flags (0), uint32 sample rate in millihertz (32728498), uint16 samples per channel (160) and uint16 channels (2).

Each 175-byte record contains a uint32 block sequence, uint32 capture PTS in µs and 167 bytes of stereo IMA audio. The left and right channel headers each contain an int16 initial sample, uint8 IMA index and uint8 zero. The following 159 bytes store one four-bit code per channel per sample, left in the low nibble.

Each datagram carries up to three fresh blocks. Datagram N can include up to three copies from datagram N−2 before its fresh records. At the nominal capture rate, a three-block batch spans 14.7 ms and its copy is sent about 29.3 ms later. The first two datagrams after startup or a peer reset contain fresh blocks only. The sender omits a redundant batch when its oldest capture exceeds 80 ms. The receiver uses each record’s original block sequence for deduplication and its capture PTS for alignment, including streams from earlier senders that repeat the immediately preceding batch. The default PC playout buffer remains 100 ms.

## Status and configuration

The handheld sends `R7ST` (76 bytes) for video-only streaming or `R9ST` (108 bytes) with audio, approximately once per second. Following the tag, all fields are uint32, in this order.

1. Session, elapsed milliseconds, total wire-rate cap, video budget and capture FPS.
2. Encoded upper/lower counts, sent upper/lower counts, send errors, expired frames and accepted feedback count.
3. Upper/lower encoder CPU core, last upper/lower encode time in µs, maximum supported wire rate and supported FPS bitmask.
4. `R9ST` adds audio enabled, sample rate in millihertz, channels, samples per block, cumulative blocks admitted to the capture queue, cumulative blocks dropped at a full queue plus blocks expired before encoding, successfully submitted audio datagrams, and current queue depth in blocks.

`R6CF` is a 20-byte configuration request containing the tag, session, request sequence, FPS, and total IPv4 bytes per second. `R10C` adds a uint32 combined-FEC flag (0 or 1) for a total of 24 bytes. The device validates the selected peer, stream session, sequence and supported settings, then applies changes between frame pairs. Status reports the settings in effect. The standard independent sender supports 400000–2000000 IPv4 bytes per second.

Senders with selectable source JPEG quality follow each periodic status with a 20-byte `R16S` datagram. Its four uint32 fields are session, layout version (1), applied JPEG target (0 for automatic, 40, 60, 80, or a separately advertised target), and the elapsed milliseconds from the accompanying status. The receiver ignores repeated or older quality reports and clears the capability on a session or endpoint change.

After observing `R16S`, the receiver uses `R16C` for configuration. This 28-byte request carries the same fields as `R10C`, followed by the requested JPEG target. The receiver requests combined FEC and includes its current JPEG selection on rate changes. It retries a lost request until the sender reports the selection. Senders that have not advertised `R16S` continue to receive the earlier configuration messages. The Q byte in each compact JPEG gives the actual quality of that screen.

Version 0.1.7 also sends a 16-byte `R16Q` datagram containing session, layout version 1 and additional supported JPEG target 70. Receivers associate this capability with the current session and endpoint before requesting Q70. The accompanying `R16S` continues to report the actual selected target.

The optional `R17T` report (20 bytes) contains four uint32 fields after the tag. They are session, elapsed milliseconds, cumulative audio-plus-video IPv4 bytes successfully submitted to the socket, and cumulative video IPv4 bytes. Both counters wrap modulo 2^32 and include UDP/IP headers, parity, transmitted key repeats and selective repairs. Status and discovery traffic are excluded. In a matched delivery window the PC compares the same media types on both ends. Its local instantaneous receive-rate counter also includes status packets.

The optional `R19T` diagnostic report is exactly 44 bytes. Its ten uint32 fields follow the tag in the order below. The elapsed time matches the companion `R7ST` or `R9ST`. Counters are cumulative for the stream and wrap modulo 2^32.

| Word after tag (zero-based) | Meaning |
| --- | --- |
| 0, 1 | Session, elapsed milliseconds |
| 2 | Encoded pairs replaced before ordinary transmission, including a pending pair displaced by a repeated key |
| 3 | Repeated reference-key transmissions selected |
| 4, 5 | Accepted selective repair requests, successfully submitted repair data packets |
| 6 | Successfully submitted repair IPv4 bytes, already included in `R17T` video bytes |
| 7, 8 | Audio blocks dropped at a full capture queue, audio blocks discarded for age before encoding |
| 9 | Microseconds spent in original frame-send loops, including packet pacing waits and interleaved audio or repair work |

Qt accepts `R19T` from the active endpoint and session with a recent device status. It compares elapsed milliseconds with wrap-aware arithmetic, discards duplicate or older reports, and allows up to 2.5 seconds of skew from the latest status. The exported counters expire after 2.5 seconds without fresh reports or status. Word 9 measures elapsed time in the send loop and can be compared across samples using unsigned 32-bit differences.

### Measuring handheld packet spacing

`R21S` announces the handheld pacing mode. A PC started with `--packet-pacing baseline` or `--packet-pacing spaced` can request a different mode through `R21C` on the paired media socket. Both messages contain the four-byte tag followed by the session ID, version 1 and mode as little-endian uint32 values. Mode 0 uses the original send schedule. Mode 1 also reserves a bounded gap after the previous socket call returns. FPS, JPEG quality and bandwidth are configured independently. A PC started without the comparison option follows the handheld's mode.

`R21T` is a 192-byte report. The table counts the tag as word 0. Device timestamps and cumulative counters use uint32 arithmetic. Use the first report after a switch as the starting sample, then compare subsequent reports in the same mode and session. The snapshot precedes the status datagrams for that round.

| Words | Contents |
| --- | --- |
| 1–3 | Session ID, companion status elapsed milliseconds, mode |
| 4–9 | Cumulative paced sleeps, requested sleep µs, sleep overshoot µs, interval maximum overshoot µs, cumulative guard activations and cumulative extra scheduled µs |
| 10 | Interval sample flags. Bits 0, 1 and 2 identify a fresh original, accepted repair request and fresh repair sample respectively |
| 11–15, 19–23 | Original or repair sample with sequence, capture PTS, fragment index, socket call start and return in µs from stream start |
| 16–18 | Accepted repair request sequence, capture PTS and receive time in µs from stream start |
| 24–47 | Six words per class in order of original video including parity, repair, audio and status. Each class reports cumulative socket calls, cumulative call µs, interval maximum call µs, starts less than 300 µs apart, returns less than 300 µs apart, and starts less than 150 µs after the preceding return |

For a timestamp-aligned comparison use the deltas of the sender's media IPv4 bytes and the actual IPv4 lengths seen at the PC over the same status boundaries. Compare complete dual-screen sequence counts, real canvas paints and audio coverage over the entire interval, including gaps between reports. The socket return timestamp measures local submission. Arrival at the Wi-Fi adapter and delivery to the player are separate observations.

The receiver returns `R3FB` (28 bytes) with six uint32 fields after the tag: session, feedback sequence, received video packets, complete screen count, incomplete screen count and relative arrival-delay growth in milliseconds. The handheld counts feedback from the active receiver. Qt adjusts its requested rate using observed stream quality when adaptive rate is selected.

Earlier sender formats remain readable by Qt, including single-screen `R3LP` and `R3ST` status. Qt also understands optional `R13S` diagnostic counters and `R15S` / `R15P` / `R15F` upper-priority streams from those sender versions.
