#include "receiver.h"
#include "live_jpeg_wire.h"
#include "wlan_stream.h"
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSettings>
#include <QtEndian>
#include <QUdpSocket>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <mswsock.h>
#endif
namespace live {
const QList<int> &rates() {
    static const QList<int> steps = {400000,  500000,  600000, 650000, 700000, 750000, 800000, 900000,
                                     1000000, 1200000, 1400000, 1600000, 1800000, 2000000};
    return steps;
}
static int locateHandheld(QUdpSocket &socket, quint16 discoveryPort, const QHostAddress &pinned,
                          const QHostAddress &hint) {
    static const auto probe = message("3DLP", {1});
    if (!pinned.isNull())
        return socket.writeDatagram(probe, pinned, discoveryPort) == probe.size();

    int sent = 0;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning) ||
            !flags.testFlag(QNetworkInterface::CanBroadcast) || flags.testFlag(QNetworkInterface::IsLoopBack) ||
            flags.testFlag(QNetworkInterface::IsPointToPoint))
            continue;
        QSet<QHostAddress> destinations;
        for (const auto &entry : interface.addressEntries()) {
            const auto local = entry.ip();
            const int prefix = entry.prefixLength();
            if (local.protocol() != QAbstractSocket::IPv4Protocol || prefix < 1 || prefix > 30)
                continue;
            const auto broadcast = entry.broadcast();
            if (broadcast.isNull() || broadcast == QHostAddress::Broadcast) continue;
            auto send = [&](const QHostAddress &address) {
                if (destinations.contains(address)) return;
                destinations.insert(address);
                QNetworkDatagram packet(probe, address, discoveryPort);
                packet.setInterfaceIndex(interface.index());
                packet.setSender(local, socket.localPort());
                if (socket.writeDatagram(packet) == probe.size()) ++sent;
            };
            send(broadcast);
            // A previous session can expedite recovery on a LAN that suppresses broadcasts.
            // Never direct a remembered hint through the gateway after switching networks.
            if (!hint.isNull() && hint != local && hint != broadcast && hint.isInSubnet(local, prefix))
                send(hint);
        }
    }
    return sent;
}
Receiver::Receiver(Timeline &t, Pipeline &p, QHostAddress ip, quint16 udp, int hz, int cap, quint16 lookupPort)
    : timeline(t), pipeline(p), sender(ip), port(udp), handheldDiscoveryPort(lookupPort),
      requestedFps(hz), requestedRate(cap) {
    worker = std::thread(&Receiver::run, this);
}
Receiver::~Receiver() {
    stop = true;
    worker.join();
}
void Receiver::configure(int hz, int cap) {
    const auto oldFps = requestedFps.exchange(hz);
    const auto oldRate = requestedRate.exchange(cap);
    if (hz != oldFps || cap != oldRate) ++revision;
}
QJsonObject Receiver::stats() {
    std::lock_guard lock(mutex);
    auto report = snapshot;
    report["last_pair_us"] = lastPairUs.load();
    report["last_packet_us"] = lastPacketUs.load();
    report["last_top_us"] = lastTopUs.load();
    return report;
}
void Receiver::run() {
    QUdpSocket socket;
    if (!socket.bind(QHostAddress::AnyIPv4, port, QAbstractSocket::DontShareAddress)) {
        std::lock_guard lock(mutex);
        snapshot = {{"socket_error", socket.errorString()}};
        return;
    }
#ifdef Q_OS_WIN
    // A peer can change IP or close its port before its last control reply arrives.
    // Winsock otherwise reports that ICMP error on a later read of this shared UDP socket.
    BOOL reportUnreachable = FALSE;
    DWORD returned = 0;
    if (WSAIoctl(SOCKET(socket.socketDescriptor()), SIO_UDP_CONNRESET, &reportUnreachable,
                 sizeof(reportUnreachable), nullptr, 0, &returned, nullptr, nullptr) == SOCKET_ERROR)
        qWarning() << "Could not disable UDP connection reset reports:" << WSAGetLastError();
#endif
    socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4 * 1024 * 1024);
    {
        std::lock_guard lock(mutex);
        snapshot["udp_port"] = socket.localPort();
    }
    WlanStream wlan;
    Assembler assembler;
    RecoveryCadence recovery;
    QHostAddress peer;
    quint16 remote = 0;
    QHostAddress discovered;
    QSettings prefs;
    QHostAddress hint;
    if (handheldDiscoveryPort && sender.isNull())
        hint.setAddress(prefs.value("lastDiscoveredHandheld").toString());
    qint64 nextLocate = 0;
    int locateSent = 0;
    quint16 discoveryPort = 0;
    quint32 discoverySession = 0;
    qint64 discoveryUntil = 0;
    std::optional<quint32> current;
    std::optional<quint32> latestStatusMs;
    int peerChanges = 0;
    assembler.onIncomplete = [&](quint32 seq, quint32 pts, qint64 firstPacket, int missing) {
        TraceEvent event{"incomplete", {current.value_or(0), seq, pts, firstPacket}, clockUs()};
        event.missingFragments = missing;
        timeline.diagnostics.add(std::move(event));
    };
    QList<quint32> retired;
    qint64 bytes = 0, lastBytes = 0, tick = clockUs(), lastConfig = 0, changed = tick, lastVideo = 0,
           maxGap = 0, intervalPairGap = 0, intervalTopGap = 0, lastSeen = 0;
    constexpr int allFps = (1 << 15) | (1 << 20) | (1 << 24) | (1 << 30);
    std::optional<qint64> baseline;
    int lagMs = 0, severeIntervals = 0, pressureIntervals = 0;
    std::array<int, 2> qualitySum{}, qualityCount{};
    std::array<qint64, 2> panelCount{}, beforePanelCount{};
    std::optional<quint32> lastBottomPts;
    bool splitSupported = false, splitActive = false;
    qint64 jpegWireBytes = 0, compactScreens = 0, fullJpegScreens = 0;
    std::optional<Pair> ready;
    SourceFrame readySource;
    auto observeTransit = [&](qint64 transit) {
        baseline = std::min(baseline.value_or(transit), transit);
        lagMs = int(std::clamp<qint64>((transit - *baseline) / 1000, 0, 10000));
    };
    auto deliver = [&] {
        if (ready) pipeline.submit(std::move(*ready), readySource);
        ready.reset();
    };
    assembler.onPanels = [&](Pair update) {
        const auto now = clockUs();
        int panelMask = 0;
        bool verifiedJpeg = false;
        for (int i = 0; i < 2; ++i) {
            const auto &wire = update.screens[i];
            if (wire.isEmpty()) continue;
            if (wire.startsWith("\xff\xd8")) {
                verifiedJpeg |= wire.endsWith("\xff\xd9");
                jpegWireBytes += wire.size();
                ++fullJpegScreens;
            } else if (wire.size() >= LIVE_JPEG_WIRE_HEADER_SIZE + 3 &&
                       wire.startsWith("R8J1") && wire[4] == LIVE_JPEG_WIRE_LAYOUT &&
                       qFromLittleEndian<quint16>(wire.constData() + 6) == (i ? 320 : 400) &&
                       wire.endsWith("\xff\xd9")) {
                const int quality = uchar(wire[5]);
                if (quality >= 1 && quality <= 95) {
                    verifiedJpeg = true;
                    update.jpegQuality[i] = quality;
                    qualitySum[i] += quality;
                    ++qualityCount[i];
                    jpegWireBytes += wire.size();
                    ++compactScreens;
                }
            }
            if (i == 1 && update.bottomPts) {
                // R15 may periodically refresh a static lower screen for reconnecting clients.
                // Its JPEG counts toward bandwidth, its unchanged capture PTS is not a new image.
                if (lastBottomPts && qint32(*update.bottomPts - *lastBottomPts) <= 0) {
                    update.screens[i].clear();
                    continue;
                }
                lastBottomPts = update.bottomPts;
            }
            ++panelCount[i];
            panelMask |= 1 << i;
        }
        if (!panelMask) return;
        if (verifiedJpeg && handheldDiscoveryPort && sender.isNull() && !peer.isLoopback() && peer != hint) {
            hint = peer;
            prefs.setValue("lastDiscoveredHandheld", hint.toString());
        }
        auto source = timeline.arrival(update.session, update.seq, update.pts, now, panelMask, update.bottomPts);
        if (panelMask & 1) {
            const auto previous = lastTopUs.exchange(now);
            if (previous) intervalTopGap = std::max(intervalTopGap, now - previous);
            // The lower capture is optional in R15. Track congestion even when it stays static.
            if (update.bottomPts) observeTransit(now - source.pts);
        }
        // Coalesce screens from an already queued UDP burst, without waiting for another datagram.
        if (ready && ready->seq == update.seq && ready->session == update.session) {
            if (panelMask & 1) readySource.arrival = source.arrival;
            for (int i = 0; i < 2; ++i) if (panelMask & (1 << i)) {
                ready->screens[i] = std::move(update.screens[i]);
                ready->jpegQuality[i] = update.jpegQuality[i];
                readySource.panels[i] = source.panels[i];
            }
        } else {
            deliver();
            ready = std::move(update);
            readySource = source;
        }
    };
    double sentFps = 0;
    std::optional<quint32> lastSent;
    quint32 lastProgressMs = 0;
    // The sender keeps its last command sequence across PC restarts. The older
    // receiver also seeds this counter from the Windows monotonic clock.
    quint32 request = quint32(clockUs() / 1000);
    int hz = 0, cap = 0, deviceMax = 2000000, mask = allFps, effective = requestedRate.load(), stable = 0,
        seenRevision = 0;
    Counts before;
    QJsonObject device, captureStats;
    std::optional<quint32> captureSampleMs;
    std::array<quint32,4> previousCapture{};
    std::array<char, 2048> buffer;
    QHostAddress ip;
    quint16 senderPort = 0;
    while (!stop) {
        if (!socket.hasPendingDatagrams())
            socket.waitForReadyRead(10);
        for (int n = 0; n < 256 && socket.hasPendingDatagrams(); ++n) {
            const auto size = socket.readDatagram(buffer.data(), buffer.size(), &ip, &senderPort);
            if (size < 8 || (!sender.isNull() && ip != sender))
                continue;
            const QByteArrayView b(buffer.data(), size);
            const auto magic = b.first(4);
            const auto now = clockUs();
            const bool reserved = now < discoveryUntil;
            if (reserved && (ip != discovered || senderPort != discoveryPort))
                continue;
            if (magic == "3DLS") {
                if (size != 12 || retired.contains(word(b, 0)) ||
                    (current && now - lastSeen < 4000000 && (ip != peer || (remote && senderPort != remote))))
                    continue;
                const auto ack = message("3DLA", {word(b, 0), word(b, 1)});
                if (socket.writeDatagram(ack, ip, senderPort) == ack.size()) {
                    discovered = ip;
                    discoveryPort = senderPort;
                    discoverySession = word(b, 0);
                    discoveryUntil = now + 4000000;
                }
                continue;
            }
            if (reserved && word(b, 0) != discoverySession)
                continue;
            if (magic == "R15S") {
                if (b.size() != 20 || !current || word(b, 0) != *current || ip != peer || senderPort != remote ||
                    word(b, 1) != 1 || !(word(b, 2) & 1u) || word(b, 3) > 1) continue;
                splitSupported = true;
                splitActive = word(b, 3) != 0;
                bytes += size + 28;
                continue;
            }
            if (magic == "R13S") {
                if (b.size() != 68 || !current || word(b, 0) != *current || ip != peer || senderPort != remote)
                    continue;
                static constexpr const char *fields[] = {"captured_top","captured_bottom","changed_top","changed_bottom",
                    "dma_misses_top","dma_misses_bottom","queue_replaced","backlog_served","backlog_skipped",
                    "jpeg_oversize","send_retries","send_recovered","snapshot_copies_top","snapshot_copies_bottom"};
                captureStats["device_protocol"] = "R13";
                for (int i = 0; i < 14; ++i)
                    captureStats[fields[i]] = qint64(word(b, i + 2));
                const auto elapsed = word(b, 1);
                const quint32 delta = elapsed - captureSampleMs.value_or(elapsed);
                if (!captureSampleMs || (delta >= 900 && delta < 0x80000000u)) {
                    for (int i = 0; i < 4; ++i) {
                        const auto count = word(b, i + 2);
                        if (captureSampleMs)
                            captureStats[QString::fromLatin1(fields[i]) + "_fps"] =
                                quint32(count - previousCapture[i]) * 1000. / delta;
                        previousCapture[i] = count;
                    }
                    captureSampleMs = elapsed;
                }
                bytes += size + 28;
                continue;
            }
            bool audio = magic == "R9A1", status = magic == "R3ST" || magic == "R9ST" || magic == "R7ST";
            if (status && b.size() != (magic == "R9ST" ? 108 : magic == "R7ST" ? 76 : 68))
                continue;
            if (!status && !audio &&
                (b.size() < 32 || !(magic == "R10P" || magic == "R10F" || magic == "R3P2" || magic == "R3F2" ||
                                    magic == "R3LP" || magic == "R15P" || magic == "R15F")))
                continue;
            if (status) {
                const auto fps = word(b, 4), capability = b.size() >= 76 ? word(b, 16) : 1000000u;
                const auto supported = b.size() >= 76 ? word(b, 17) : quint32((1 << 24) | (1 << 30));
                if (fps > 30 || !(supported & (1u << fps)) || (supported & ~quint32(allFps)) ||
                    capability < 400000 || capability > 2000000 || word(b, 2) > capability ||
                    word(b, 2) < 400000 || word(b, 3) > word(b, 2))
                    continue;
            }
            if (audio && (b.size() < 24 || b.size() > 1074))
                continue;
            auto id = word(b, 0);
            if (current && ip != peer) {
                const bool sameSession = id == *current;
                const bool freshStatus = sameSession && status && latestStatusMs &&
                                         qint32(word(b, 1) - *latestStatusMs) > 0;
                const auto quietUs = now - lastSeen;
                // A periodic sender status can confirm a DHCP move before the normal
                // discovery timeout. A live peer, stale status or audio alone cannot take over.
                if (audio || (sameSession && status && latestStatusMs && !freshStatus) ||
                    (quietUs < 3000000 && !(freshStatus && quietUs >= 250000)))
                    continue;
                if (sameSession) {
                    peer = ip;
                    remote = 0;
                    lastConfig = 0;
                    ++peerChanges;
                }
            }
            if (!current || id != *current) {
                if (retired.contains(id))
                    continue;
                if (current) {
                    retired.append(*current);
                    if (retired.size() > 8)
                        retired.removeFirst();
                }
                current = id;
                ready.reset();
                splitSupported = splitActive = false;
                lastTopUs = intervalTopGap = 0;
                lastBottomPts.reset();
                latestStatusMs.reset();
                peer = ip;
                remote = 0;
                pipeline.resetSession(id);
                assembler.reset();
                recovery = {};
                hz = cap = 0;
                lastConfig = lastVideo = 0;
                lastPairUs = intervalPairGap = 0;
                baseline.reset();
                lagMs = severeIntervals = pressureIntervals = 0;
                sentFps = 0;
                qualitySum = qualityCount = {};
                jpegWireBytes = compactScreens = fullJpegScreens = 0;
                lastSent.reset();

                effective = requestedRate.load();
                stable = 0;
                device = {};
                captureStats = {};
                captureSampleMs.reset();
                deviceMax = 2000000;
                mask = allFps;
            }
            lastPacketUs = lastSeen = now;
            bytes += b.size() + 28;
            if (status) {
                remote = senderPort;
                hz = int(word(b, 4));
                cap = int(word(b, 2));
                recovery.confirm(hz, cap, now);
                const auto sent = word(b, 7), elapsed = word(b, 1);
                if (!latestStatusMs || qint32(elapsed - *latestStatusMs) > 0)
                    latestStatusMs = elapsed;
                if (!lastSent || quint32(elapsed - lastProgressMs) >= 900) {
                    if (lastSent)
                        sentFps = quint32(sent - *lastSent) * 1000. / quint32(elapsed - lastProgressMs);
                    lastProgressMs = elapsed;
                    lastSent = sent;
                }
                deviceMax = b.size() >= 76 ? int(word(b, 16)) : 1000000;
                mask = b.size() >= 76 ? int(word(b, 17)) : ((1 << 24) | (1 << 30));
                device = {{"capture_fps", hz},
                          {"device_status_us", now},
                          {"device_elapsed_ms", qint64(elapsed)},
                          {"device_budget", int(word(b, 3))},
                          {"device_cap", cap},
                          {"device_encoded_top", qint64(word(b, 5))},
                          {"device_encoded_bottom", qint64(word(b, 6))},
                          {"device_sent_top", qint64(word(b, 7))},
                          {"device_sent_bottom", qint64(word(b, 8))},
                          {"device_send_errors", qint64(word(b, 9))},
                          {"device_expired", qint64(word(b, 10))},
                          {"top_encode_us", int(word(b, 14))},
                          {"bottom_encode_us", int(word(b, 15))}};
                if (word(b, 12) <= 3 && word(b, 13) <= 3) {
                    device["encoder_core_top"] = int(word(b, 12));
                    device["encoder_core_bottom"] = int(word(b, 13));
                }
                if (b.size() == 108)
                    device["device_audio_enabled"] = bool(word(b, 18));
            } else if (audio)
                timeline.addAudio(b, now);
            else {
                remote = senderPort;
                if (auto pair = assembler.feed(b, now)) {
                    if (pair->screens[1].isEmpty()) continue;
                    if (lastVideo) {
                        maxGap = std::max(maxGap, now - lastVideo);
                        intervalPairGap = std::max(intervalPairGap, now - lastVideo);
                    }
                    lastPairUs = lastVideo = now;
                    auto source = timeline.arrival(id, pair->seq, pair->pts, now, 0);
                    if (!pair->bottomPts) observeTransit(now - source.pts);
                }
            }
        }
        deliver();
        auto now = clockUs();
        if (handheldDiscoveryPort && now >= nextLocate && now >= discoveryUntil &&
            (!lastSeen || now - lastSeen >= 4000000)) {
            locateSent += locateHandheld(socket, handheldDiscoveryPort, sender, hint);
            nextLocate = now + 1000000;
        }
        assembler.expire(now);
        if (revision != seenRevision) {
            seenRevision = revision;
            recovery = {};
            effective = requestedRate.load();

            lastConfig = 0;
            pressureIntervals = severeIntervals = 0;
            stable = 0;
            changed = now;
        }
        int target = std::min(requestedRate.load(), deviceMax);
        effective = std::min(effective, target);
        const int limit = requestedFps.load();
        int freq = hz;
        for (int candidate : {15, 20, 24, 30})
            if (candidate <= limit && (mask & (1 << candidate)))
                freq = candidate;
        if (remote && current && now - lastConfig > 500000 && now - lastSeen < 2000000 && freq > 0 &&
            (hz != freq || cap != effective || !assembler.combined ||
             (splitSupported && splitActive))) {
            if (++request == 0)
                ++request;
            if (splitSupported) {
                socket.writeDatagram(message("R15C", {*current, request, quint32(freq), quint32(effective),
                                                     0u}), peer, remote);
            } else {
                socket.writeDatagram(message("R6CF", {*current, request, quint32(freq), quint32(effective)}), peer, remote);
                socket.writeDatagram(message("R10C", {*current, request, quint32(freq), quint32(effective), 1}), peer, remote);
            }
            lastConfig = now;
        }
        if (now - tick < 1000000)
            continue;
        const auto &c = assembler.counts;
        auto goodput = (bytes - lastBytes) * 1000000 / (now - tick);
        int lost = int(c.incomplete - before.incomplete), complete = int(c.complete - before.complete);
        auto repaired = c.repaired - before.repaired, missing = c.missing - before.missing,
             late = c.late - before.late;
        const double receivedFps = complete * 1000000. / (now - tick);
        const bool healthyCadence = freq > 0 && receivedFps >= freq * .94 && lagMs < 120;
        const bool lowStep = effective < target && effective < 600000;
        const bool healthyRecovery = lowStep ? recovery.healthy(freq, effective, tick, now, complete) && lagMs < 120
                                             : healthyCadence;
        bool severe = lost >= 5 || (lost && lost * 100 >= 12 * (complete + lost)) || missing >= 32 ||
                      late >= 24 || (complete && lagMs >= 250);
        const bool pressure = severe || (complete && lagMs >= 180) || lost >= 2 || missing >= 8 || late >= 8 ||
                              (repaired >= 8 && repaired * 100 >= (c.packets - before.packets) * 2);
        pressureIntervals = pressure ? std::min(pressureIntervals + 1, 2) : 0;
        // One repaired burst on an otherwise full-rate link need not reduce JPEG quality.
        bool congestion = severe || (pressure && (!healthyCadence || pressureIntervals >= 2));
        severeIntervals = severe && lost > 0 ? std::min(severeIntervals + 1, 2) : 0;
        if (congestion && now - changed >= 2000000) {
            // R7+ senders support 0.4 MB/s. Reserve that range for measured, sustained loss.
            const int minimum = freq == 30 && device.value("device_audio_enabled").toBool() ? 500000 : 400000;
            int deviceFloor = deviceMax > 1000000 ? (severeIntervals == 2 ? minimum : 600000) : 700000;
            // A short fade can erase many fragments while the sender is still
            // delivering nearly all 30 pairs. Reserve multi-step reductions
            // for a persisting fade or a real collapse in delivery.
            const bool hardCut = severe && (severeIntervals == 2 || receivedFps < freq * .75 || lagMs >= 250);
            int floor = std::min(target, deviceFloor),
                limit = hardCut ? std::max(floor, int(std::min(effective * .8, goodput * .9))) : effective - 1;
            int next = floor;
            for (int rate : rates())
                if (rate >= floor && rate <= limit)
                    next = rate;
            if (next < effective) {
                effective = next;
                changed = now;
            }
        }
        if (congestion || !healthyRecovery)
            stable = 0;
        else if (lost || pressure)
            stable = std::max(0, stable - 2);
        else
            stable = std::min(stable + 1, 20);
        if (stable >= 8 && now - changed >= 10000000 && effective < target)
            for (int rate : rates())
                if (rate > effective && rate <= target) {
                    effective = rate;
                    changed = now;
                    stable = 0;
                    break;
                }
        if (current && remote && now - lastSeen < 2000000)
            socket.writeDatagram(
                message("R3FB", {*current, quint32(now / 1000), quint32(c.packets), quint32(c.complete * 2),
                                 quint32(c.incomplete * 2), quint32(lagMs)}),
                peer, remote);
        QJsonObject report = device;
        report["udp_port"] = socket.localPort();
        for (auto it = captureStats.begin(); it != captureStats.end(); ++it)
            report[it.key()] = it.value();
        wlan.setPeer(lastSeen && now - lastSeen < 3000000 ? peer.toIPv4Address() : 0);
        report["wlan_streaming"] = wlan.enabled.load();
        report["wlan_streaming_error"] = qint64(wlan.error.load());
        report["peer"] = peer.toString();
        report["discovery_queries_sent"] = locateSent;
        report["peer_changes"] = peerChanges;
        report["session"] = qint64(current.value_or(0));
        report["pairs"] = c.fullPairs;
        report["fps_received"] = (c.fullPairs - before.fullPairs) * 1000000. / (now - tick);
        report["top_received"] = panelCount[0];
        report["bottom_received"] = panelCount[1];
        report["fps_top_received"] = (panelCount[0] - beforePanelCount[0]) * 1000000. / (now - tick);
        report["fps_bottom_received"] = (panelCount[1] - beforePanelCount[1]) * 1000000. / (now - tick);
        report["split_supported"] = splitSupported;
        report["split_active"] = splitActive;
        report["device_sent_fps"] = sentFps;
        if (jpegWireBytes) {
            const qint64 saved = compactScreens * LIVE_JPEG_WIRE_SAVED_BYTES;
            report["jpeg_compact_screens"] = compactScreens;
            report["jpeg_full_screens"] = fullJpegScreens;
            report["jpeg_wire_bytes"] = jpegWireBytes;
            report["jpeg_header_saved_bytes"] = saved;
            report["jpeg_header_saving_pct"] = 100. * saved / (jpegWireBytes + saved);
        }
        for (int i = 0; i < 2; ++i)
            if (qualityCount[i])
                report[i == 0 ? "jpeg_top_q" : "jpeg_bottom_q"] = double(qualitySum[i]) / qualityCount[i];
        qualitySum = qualityCount = {};
        report["incomplete"] = c.incomplete;
        report["missing"] = c.missing;
        report["repaired"] = c.repaired;
        report["invalid"] = c.invalid;
        report["late"] = c.late;
        report["bytes_per_s"] = goodput;
        report["arrival_lag_ms"] = lagMs;
        report["requested_rate"] = requestedRate.load();
        report["requested_fps"] = requestedFps.load();
        report["max_pair_gap_ms"] = maxGap / 1000.;
        report["rx_sample_us"] = now;
        report["interval_pair_gap_ms"] = intervalPairGap / 1000.;
        report["interval_top_gap_ms"] = intervalTopGap / 1000.;
        intervalPairGap = intervalTopGap = 0;
        report["effective_rate"] = effective;
        report["effective_fps"] = freq;
        report["recovery_stable_seconds"] = stable;
        report["config_confirmed"] = cap == effective && hz == freq;
        report["max_rate"] = deviceMax;
        report["fps_mask"] = mask;
        report["combined_fec"] = assembler.combined;
        report["audio_blocks"] = timeline.audioBlocks.load();
        report["audio_duplicates"] = timeline.duplicates.load();
        report["audio_errors"] = timeline.invalidAudio.load();
        {
            std::lock_guard lock(mutex);
            snapshot = report;
        }
        tick = now;
        lastBytes = bytes;
        before = c;
        beforePanelCount = panelCount;
    }
}
} // namespace live
