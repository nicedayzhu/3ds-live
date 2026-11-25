#include "receiver.h"
#include "live_jpeg_wire.h"
#include "live_pair.h"
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
// Word indexes here follow the protocol table, including the four-byte tag.
static QJsonObject packetTiming(QByteArrayView packet, qint64 receivedUs) {
    auto value = [&](int i) { return qint64(word(packet, i - 1)); };
    QJsonObject result{{"elapsed_ms", value(2)}, {"received_us", receivedUs}, {"mode", value(3)},
                       {"sleeps", value(4)}, {"sleep_requested_us", value(5)}, {"sleep_overshoot_us", value(6)},
                       {"max_sleep_overshoot_us", value(7)}, {"guard_activations", value(8)}, {"guard_added_us", value(9)}};
    auto fragment = [&](int i) {
        return QJsonObject{{"sequence", value(i)}, {"pts_us", value(i+1)}, {"index", value(i+2)},
                           {"submit_us", value(i+3)}, {"return_us", value(i+4)}};
    };
    const auto flags = value(10);
    if (flags & 1) result["original"] = fragment(11);
    if (flags & 2) result["nack"] = QJsonObject{{"sequence", value(16)}, {"pts_us", value(17)}, {"received_us", value(18)}};
    if (flags & 4) result["repair"] = fragment(19);
    const char *classes[] = {"video", "repair_packets", "audio", "status"};
    for (int group = 0; group < 4; ++group) {
        int i = 24 + group * 6;
        result[classes[group]] = QJsonObject{{"calls", value(i)}, {"call_us", value(i+1)}, {"max_call_us", value(i+2)},
                                             {"starts_under_300us", value(i+3)}, {"returns_under_300us", value(i+4)},
                                             {"start_after_return_under_150us", value(i+5)}};
    }
    return result;
}
struct SenderWireRate {
    struct Sample { quint32 elapsed, media, video; qint64 arrived; };
    std::optional<Sample> previous;
    std::optional<quint32> mediaBytesPerSecond, videoBytesPerSecond;
    static constexpr qint64 expiryUs = 2500000;

    void observe(Sample next) {
        if (previous) {
            const auto ms = next.elapsed - previous->elapsed;
            if (qint32(ms) <= 0) return;
            if (ms <= 2500 && next.arrived - previous->arrived <= expiryUs) {
                // Configuration acknowledgements may produce multiple reports in one second.
                if (ms < 750) return;
                const auto media = next.media - previous->media, video = next.video - previous->video;
                // Unsigned subtraction handles both counter and timestamp wrap. Counts
                // well above the maximum wire rate indicate a discontinuity in the sender.
                if (video <= media && media <= quint64(ms) * LIVE_MAX_RATE / 1000 + 65536) {
                    mediaBytesPerSecond = quint32(quint64(media) * 1000 / ms);
                    videoBytesPerSecond = quint32(quint64(video) * 1000 / ms);
                    previous = next;
                    return;
                }
            }
        }
        mediaBytesPerSecond.reset();
        videoBytesPerSecond.reset();
        previous = next;
    }
    void report(QJsonObject &out, qint64 now) const {
        if (!mediaBytesPerSecond || !previous || now - previous->arrived > expiryUs) return;
        out["device_media_bytes_per_s"] = qint64(*mediaBytesPerSecond);
        out["device_video_bytes_per_s"] = qint64(*videoBytesPerSecond);
    }
};

bool isSourceJpegQuality(quint32 quality) {
    return live_valid_quality_target(quality);
}
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
Receiver::Receiver(Timeline &t, Pipeline &p, QHostAddress ip, quint16 udp, int hz, int cap, quint16 lookupPort,
                   int jpegQuality, bool adaptive, std::optional<int> packetPacing)
    : timeline(t), pipeline(p), sender(ip), port(udp), handheldDiscoveryPort(lookupPort), requestedPacing(packetPacing),
      requested{int(live_migrate_fps(hz)), cap, jpegQuality, adaptive} {
    worker = std::thread(&Receiver::run, this);
}
Receiver::~Receiver() {
    stop = true;
    worker.join();
}
void Receiver::configure(int hz, int cap, int jpegQuality, bool adaptive, int screenMask) {
    hz = int(live_migrate_fps(hz));
    std::lock_guard lock(mutex);
    if (hz != requested.fps || cap != requested.rate || jpegQuality != requested.quality ||
        adaptive != requested.adaptive || screenMask != requested.mask) {
        requested = {hz, cap, jpegQuality, screenMask, adaptive, requested.revision + 1};
        latestDelivery = {};
    }
}
QJsonObject Receiver::stats() {
    std::lock_guard lock(mutex);
    auto report = snapshot;
    if (!latestDelivery.isEmpty() && latestDelivery.value("session") == report.value("session") &&
        clockUs() - latestDelivery.value("end_us").toInteger() <= DeliveryWindow::freshUs)
        report["delivery_window"] = latestDelivery;
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
    Assembler assembler(timeline.delay);
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
    std::optional<quint32> senderCountersMs;
    QJsonObject senderCounters, deviceAudioQueue;
    std::optional<int> pacingMode;
    std::optional<quint32> lastTimingMs;
    quint32 lastScreenCounterMs = 0;
    qint64 lastEncodedTop = -1, lastEncodedBottom = -1, lastSentTop = -1, lastSentBottom = -1;
    QJsonObject timing;
    qint64 lastPacingStatusUs = 0, lastTimingUs = 0;
    qint64 senderCountersAt = 0;
    int peerChanges = 0;
    assembler.onIncomplete = [&](quint32 seq, quint32 pts, qint64 firstPacket, int missing) {
        TraceEvent event{"incomplete", {current.value_or(0), seq, pts, firstPacket}, clockUs()};
        event.missingFragments = missing;
        timeline.diagnostics.add(std::move(event));
    };
    QList<quint32> retired;
    qint64 bytes = 0, lastBytes = 0, tick = clockUs(), lastConfig = 0, changed = tick, lastVideo = 0,
           maxGap = 0, intervalPairGap = 0, intervalTopGap = 0, lastSeen = 0;
    constexpr int allFps = LIVE_FPS_MASK;
    std::optional<qint64> baseline;
    int lagMs = 0, severeIntervals = 0, pressureIntervals = 0;
    int cleanIntervals = 0, rateBeforeProbe = 0;
    qint64 lastProbe = 0, probeAfter = 0;
    std::array<int, 2> qualitySum{}, qualityCount{};
    std::array<qint64, 2> panelCount{}, beforePanelCount{};
    std::optional<quint32> lastBottomPts;
    bool splitSupported = false, splitActive = false;
    std::optional<int> deviceQuality;
    bool supportsQ70 = false;
    std::optional<quint32> qualityStatusMs;
    SenderWireRate senderWireRate;
    DeliveryWindow deliveryWindow;
    qint64 receivedMediaBytes = 0;
    auto readSettings = [&] {
        std::lock_guard lock(mutex);
        return requested;
    };
    auto config = readSettings();
    auto invalidateDelivery = [&] {
        deliveryWindow = {};
        std::lock_guard lock(mutex);
        latestDelivery = {};
    };
    auto refreshDelivery = [&](qint64 now) {
        auto window = deliveryWindow.report(now);
        if (!window.isEmpty()) {
            window["session"] = qint64(current.value_or(0));
            if (deviceQuality) window["device_jpeg_quality"] = *deviceQuality;
        }
        // Publish at the report boundary, independently of the 1 Hz stats/UI timers.
        std::lock_guard lock(mutex);
        if (config.revision == requested.revision) latestDelivery = std::move(window);
    };
    qint64 lastRowReply = 0, lastRepairReply = 0, lastDictionaryReply = 0;
    auto clearCapabilities = [&] {
        splitSupported = splitActive = false;
        deviceQuality.reset();
        supportsQ70 = false;
        qualityStatusMs.reset();
        senderCountersMs.reset();
        senderCounters = {};
        deviceAudioQueue = {};
        senderCountersAt = 0;
        pacingMode.reset();
        lastTimingMs.reset();
        timing = {};
        lastPacingStatusUs = lastTimingUs = 0;
        lastRowReply = lastRepairReply = lastDictionaryReply = 0;
        recovery = {};
        assembler.reset();
        senderWireRate = {};
        invalidateDelivery();
    };
    qint64 jpegWireBytes = 0, compactScreens = 0, fullJpegScreens = 0;
    qint64 rowSavedBytes = 0, rowDeltaScreens = 0, rowsReused = 0;
    qint64 dictionarySavedBytes = 0, dictionaryScreens = 0;
    assembler.onReference = [&](std::optional<quint32> reference) {
        if (current && remote && assembler.rowReuse) {
            lastRowReply = clockUs();
            socket.writeDatagram(message("R17A", {*current, reference ? 1u : 0u, reference.value_or(0)}), peer, remote);
        }
    };
    auto replyEnabled = [&](const char *tag, qint64 &sentAt) {
        sentAt = clockUs();
        socket.writeDatagram(message(tag, {*current, 1}), peer, remote);
    };
    assembler.onRepair = [&](quint32 seq, quint32 pts, const QByteArray &indices) {
        if (!current || !remote || !assembler.repairLimit) return false;
        auto nack = message("R19N", {*current, seq, pts, quint32(indices.size())}) + indices;
        return socket.writeDatagram(nack, peer, remote) == nack.size();
    };
    std::optional<Pair> ready;
    SourceFrame readySource;
    auto observeTransit = [&](qint64 transit) {
        baseline = std::min(baseline.value_or(transit), transit);
        lagMs = int(std::clamp<qint64>((transit - *baseline) / 1000, 0, 10000));
    };
    assembler.onComplete = [&](const Pair &pair) {
        if (pair.screens[1].isEmpty()) return;
        const auto now = clockUs();
        if (lastVideo) {
            maxGap = std::max(maxGap, now - lastVideo);
            intervalPairGap = std::max(intervalPairGap, now - lastVideo);
        }
        lastPairUs = lastVideo = now;
        auto source = timeline.arrival(pair.session, pair.seq, pair.pts, now, 0);
        if (!pair.bottomPts) observeTransit(now - source.pts);
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
                    if (update.deltaBytes[i]) {
                        const int outer = i == 1 ? update.dictionaryWireBytes : 0;
                        jpegWireBytes += outer ? outer : update.deltaBytes[i];
                        if (outer) {
                            dictionarySavedBytes += update.deltaBytes[i] - outer;
                            ++dictionaryScreens;
                        }
                        rowSavedBytes += wire.size() - update.deltaBytes[i];
                        ++rowDeltaScreens;
                        rowsReused += update.reusedRows[i];
                    } else jpegWireBytes += wire.size();
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
    int hz = 0, cap = 0, deviceMax = 2000000, mask = allFps, effective = config.rate, stable = 0;
    Counts before;
    QJsonObject device, captureStats;
    std::optional<quint32> captureSampleMs;
    std::array<quint32,4> previousCapture{};
    std::array<char, 2048> buffer;
    QHostAddress ip;
    quint16 senderPort = 0;
    while (!stop) {
        if (!socket.hasPendingDatagrams())
            socket.waitForReadyRead(assembler.socketWaitMs(clockUs()));
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
            if (magic == "R21S" || magic == "R21T") {
                if (!current || word(b,0) != *current || ip != peer || senderPort != remote ||
                    !latestStatusMs || now - device.value("device_status_us").toInteger() > SenderWireRate::expiryUs)
                    continue;
                if (magic == "R21S") {
                    if (b.size() != 16 || word(b,1) != 1 || word(b,2) > 1) continue;
                    pacingMode = int(word(b,2));
                    lastPacingStatusUs = now;
                    if (requestedPacing && *requestedPacing != *pacingMode)
                        socket.writeDatagram(message("R21C", {*current, 1, quint32(*requestedPacing)}), peer, remote);
                } else {
                    if (b.size() != 192 || word(b,2) > 1 || (word(b,9) & ~7u)) continue;
                    const auto elapsed = word(b,1);
                    const auto skew = qint32(elapsed - *latestStatusMs);
                    if (skew < -2500 || skew > 2500 || (lastTimingMs && qint32(elapsed - *lastTimingMs) <= 0))
                        continue;
                    lastTimingMs = elapsed;
                    lastTimingUs = now;
                    timing = packetTiming(b, now);
                }
                bytes += size + 28;
                continue;
            }
            if (magic == "R19T") {
                if (b.size() != 44 || !current || word(b, 0) != *current || ip != peer || senderPort != remote ||
                    !latestStatusMs || now - device.value("device_status_us").toInteger() > SenderWireRate::expiryUs)
                    continue;
                const auto elapsed = word(b, 1);
                const auto skew = qint32(elapsed - *latestStatusMs);
                if (skew < -2500 || skew > 2500 || (senderCountersMs && qint32(elapsed - *senderCountersMs) <= 0))
                    continue;
                senderCountersMs = elapsed;
                senderCountersAt = now;
                senderCounters = {{"device_counters_elapsed_ms", qint64(elapsed)}, {"device_counters_rx_us", now},
                                  {"device_replaced", qint64(word(b, 2))}, {"device_row_repeats", qint64(word(b, 3))},
                                  {"device_repair_requests", qint64(word(b, 4))}, {"device_repair_packets", qint64(word(b, 5))},
                                  {"device_repair_ipv4_bytes", qint64(word(b, 6))}, {"device_audio_queue_full", qint64(word(b, 7))},
                                  {"device_audio_expired", qint64(word(b, 8))}, {"device_send_active_us", qint64(word(b, 9))}};
                bytes += size + 28;
                continue;
            }
            if (magic == "R17T") {
                if (b.size() != 20 || !current || word(b, 0) != *current ||
                    ip != peer || senderPort != remote) continue;
                const auto elapsed = word(b, 1);
                // It may precede the matching R9ST; never use this packet's arrival
                // time as the boundary for the PC counts.
                deliveryWindow.wire(elapsed, word(b, 2), now);
                refreshDelivery(now);
                if (!latestStatusMs) continue;
                const auto skew = qint32(elapsed - *latestStatusMs);
                if (skew < -2500 || skew > 2500 || now - device["device_status_us"].toInteger() > SenderWireRate::expiryUs)
                    continue;
                senderWireRate.observe({elapsed, word(b, 2), word(b, 3), now});
                bytes += size + 28;
                continue;
            }
            if (magic == "R20S") {
                if (b.size() != 12 || !current || word(b, 0) != *current || ip != peer || senderPort != remote ||
                    now - lastSeen >= 2000000 || word(b, 1) != 1) continue;
                assembler.dictionarySupported = true;
                replyEnabled("R20A", lastDictionaryReply);
                bytes += size + 28;
                continue;
            }
            if (magic == "R19S") {
                if (b.size() != 16 || !current || word(b,0) != *current || ip != peer || senderPort != remote ||
                    now - lastSeen >= 2000000 || word(b,1) != 1 || word(b,2) == 0 || word(b,2) > MaxRepairFragments) continue;
                assembler.repairLimit = int(word(b,2));
                replyEnabled("R19A", lastRepairReply);
                bytes += size + 28;
                continue;
            }
            if (magic == "R19P" && (b.size() < 32 || !current || word(b,0) != *current ||
                ip != peer || senderPort != remote || !assembler.repairLimit)) continue;
            if (magic == "R17S") {
                if (b.size() != 12 || !current || word(b, 0) != *current || ip != peer ||
                    senderPort != remote || now - lastSeen >= 2000000 || word(b, 1) != 1) continue;
                assembler.rowReuse = true;
                assembler.onReference(assembler.acknowledgedReference());
                bytes += size + 28;
                continue;
            }
            if (magic == "R16Q") {
                if (b.size() != 16 || !current || word(b, 0) != *current || ip != peer || senderPort != remote ||
                    word(b, 1) != 1 || word(b, 2) != LIVE_JPEG_EXTRA_QUALITY) continue;
                if (!supportsQ70) lastConfig = 0;
                supportsQ70 = true;
                bytes += size + 28;
                continue;
            }
            if (magic == "R16S") {
                if (b.size() != 20 || !current || word(b, 0) != *current || ip != peer || senderPort != remote ||
                    word(b, 1) != 1 || !isSourceJpegQuality(word(b, 2)) ||
                    (qualityStatusMs && qint32(word(b, 3) - *qualityStatusMs) <= 0)) continue;
                qualityStatusMs = word(b, 3);
                const int quality = int(word(b, 2));
                if (deviceQuality != quality) {
                    recovery = {};
                    invalidateDelivery();
                }
                deviceQuality = quality;
                bytes += size + 28;
                continue;
            }
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
                (b.size() < 32 || !(magic == "R10P" || magic == "R10F" || magic == "R19P" || magic == "R3P2" || magic == "R3F2" ||
                                    magic == "R3LP" || magic == "R15P" || magic == "R15F")))
                continue;
            if (status) {
                const auto fps = word(b, 4), capability = b.size() >= 76 ? word(b, 16) : 1000000u;
                const auto supported = b.size() >= 76 ? word(b, 17) : quint32((1 << 24) | (1 << 30));
                if (!(supported & live_media_fps_flag(fps)) || (supported & ~quint32(allFps | LIVE_LEGACY_FPS_EXTRA)) ||
                    capability < 400000 || capability > 2000000 || word(b, 2) > capability ||
                    word(b, 2) < 400000 || word(b, 3) > word(b, 2))
                    continue;
            }
            if (audio && (b.size() < 24 || b.size() > 1074))
                continue;
            auto id = word(b, 0);
            if (current && (ip != peer || (remote && senderPort != remote))) {
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
                    clearCapabilities();
                    mask = allFps;
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
                clearCapabilities();
                lastTopUs = intervalTopGap = 0;
                lastBottomPts.reset();
                latestStatusMs.reset();
                peer = ip;
                remote = 0;
                pipeline.resetSession(id);
                before = assembler.counts;
                beforePanelCount = panelCount;
                lastBytes = bytes;
                tick = changed = now;
                hz = cap = 0;
                lastConfig = lastVideo = 0;
                lastPairUs = intervalPairGap = 0;
                baseline.reset();
                lagMs = severeIntervals = pressureIntervals = 0;
                cleanIntervals = rateBeforeProbe = 0;
                lastProbe = probeAfter = 0;
                sentFps = 0;
                qualitySum = qualityCount = {};
                jpegWireBytes = compactScreens = fullJpegScreens = 0;
                rowSavedBytes = rowDeltaScreens = rowsReused = 0;
                dictionarySavedBytes = dictionaryScreens = 0;
                lastSent.reset();
                config = readSettings();
                effective = config.rate;
                stable = 0;
                device = {};
                captureStats = {};
                captureSampleMs.reset();
                deviceMax = 2000000;
                mask = allFps;
            }
            if (status && latestStatusMs && qint32(word(b, 1) - *latestStatusMs) <= 0)
                continue;
            // A handheld can forget all negotiated references after four seconds
            // of silence even when reconnecting on the same socket and session.
            if (lastSeen && now - lastSeen >= 4000000 &&
                (assembler.rowReuse || assembler.dictionarySupported || assembler.repairLimit))
                clearCapabilities();
            lastPacketUs = lastSeen = now;
            bytes += b.size() + 28;
            if (!status) receivedMediaBytes += b.size() + 28;
            if (status) {
                remote = senderPort;
                const bool sameConfig = hz == int(word(b, 4)) && cap == int(word(b, 2));
                hz = int(word(b, 4));
                cap = int(word(b, 2));
                deviceMax = b.size() >= 76 ? int(word(b, 16)) : 1000000;
                mask = b.size() >= 76 ? int(word(b, 17) & allFps) : ((1 << 24) | (1 << 30));
                const auto sent = word(b, 7), elapsed = word(b, 1);
                if (b.size() >= 76 && sent == word(b, 8) && !splitActive) {
                    deliveryWindow.status({elapsed, sent, hz, cap, now, receivedMediaBytes,
                                           assembler.counts.fullPairs, panelCount[0]});
                    refreshDelivery(now);
                } else invalidateDelivery();
                std::optional<RecoveryCadence::Delivery> delivery;
                // Only combined, equal-screen streams have one sent pair for every full pair.
                if (assembler.combined && !splitActive && sent == word(b, 8))
                    delivery = RecoveryCadence::Delivery{elapsed, sent, word(b, 9) + word(b, 10),
                                                         assembler.counts.fullPairs};
                recovery.confirm(hz, cap, now, delivery);
                latestStatusMs = elapsed;
                if (!sameConfig)
                    lastSent.reset();
                if (!lastSent || quint32(elapsed - lastProgressMs) >= 900) {
                    const auto interval = elapsed - lastProgressMs;
                    if (lastSent)
                        sentFps = quint32(sent - *lastSent) * 1000. / interval;
                    lastProgressMs = elapsed;
                    lastSent = sent;
                }
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
                if (lastScreenCounterMs && elapsed > lastScreenCounterMs) {
                    const double seconds = (elapsed - lastScreenCounterMs) / 1000.0;
                    if (lastEncodedTop >= 0) {
                        device["device_encoded_top_fps"] = (word(b, 5) - lastEncodedTop) / seconds;
                        device["device_encoded_bottom_fps"] = (word(b, 6) - lastEncodedBottom) / seconds;
                        device["device_sent_top_fps"] = (word(b, 7) - lastSentTop) / seconds;
                        device["device_sent_bottom_fps"] = (word(b, 8) - lastSentBottom) / seconds;

                    }
                }
                lastScreenCounterMs = elapsed;
                lastEncodedTop = word(b, 5);
                lastEncodedBottom = word(b, 6);
                lastSentTop = word(b, 7);
                lastSentBottom = word(b, 8);
                if (word(b, 12) <= 3 && word(b, 13) <= 3) {
                    device["encoder_core_top"] = int(word(b, 12));
                    device["encoder_core_bottom"] = int(word(b, 13));
                }
                deviceAudioQueue = {};
                if (b.size() == 108) {
                    device["device_audio_enabled"] = bool(word(b, 18));
                    timeline.setAudioEnabled(bool(word(b, 18)));
                    deviceAudioQueue = {{"device_audio_blocks_queued", qint64(word(b, 22))},
                                        {"device_audio_blocks_dropped", qint64(word(b, 23))},
                                        {"device_audio_packets_sent", qint64(word(b, 24))},
                                        {"device_audio_queue_depth", qint64(word(b, 25))}};
                }
            } else if (audio)
                timeline.addAudio(b, now);
            else {
                remote = senderPort;
                assembler.feed(b, now);
            }
        }
        auto now = clockUs();
        assembler.pollRepairs(now);
        deliver();
        // Each capability has its own four-second lease on the handheld. An
        // uninterrupted media stream can lose several consecutive status tails.
        // Renew only a negotiated feature, using fresh inbound activity and
        // the assembler's current verified key (including sequence zero).
        if (current && remote && now - lastSeen < 2000000) {
            auto due = [&](qint64 sentAt) { return lastSeen > sentAt && now - sentAt >= 1000000; };
            if (assembler.rowReuse && due(lastRowReply))
                assembler.onReference(assembler.acknowledgedReference());
            if (assembler.repairLimit && due(lastRepairReply))
                replyEnabled("R19A", lastRepairReply);
            if (assembler.dictionarySupported && due(lastDictionaryReply))
                replyEnabled("R20A", lastDictionaryReply);
        }
        if (handheldDiscoveryPort && now >= nextLocate && now >= discoveryUntil &&
            (!lastSeen || now - lastSeen >= 4000000)) {
            locateSent += locateHandheld(socket, handheldDiscoveryPort, sender, hint);
            nextLocate = now + 1000000;
        }
        assembler.expire(now);
        const auto next = readSettings();
        if (next.revision != config.revision) {
            config = next;
            recovery = {};
            invalidateDelivery();
            effective = config.rate;
            lastSent.reset();
            lastConfig = 0;
            pressureIntervals = severeIntervals = 0;
            cleanIntervals = rateBeforeProbe = 0;
            lastProbe = probeAfter = 0;
            stable = 0;
            changed = now;
        }
        int target = std::min(config.rate, deviceMax);
        const bool adaptive = config.adaptive;
        effective = adaptive ? std::min(effective, target) : target;
        const int freq = supportedCaptureFps(config.fps, mask, hz);
        const int quality = config.quality;
        const bool canRequestQuality = quality != LIVE_JPEG_EXTRA_QUALITY || supportsQ70;
        // A legacy R16S peer can apply the other controls while keeping its current JPEG target.
        const int wireQuality = canRequestQuality ? quality : deviceQuality.value_or(0);
        if (remote && current && now - lastConfig > 500000 && now - lastSeen < 2000000 && freq > 0 &&
            (wireQuality != LIVE_JPEG_EXTRA_QUALITY || supportsQ70) &&
            (hz != freq || cap != effective || !assembler.combined ||
             (splitSupported && splitActive) || (deviceQuality && *deviceQuality != wireQuality))) {
            if (++request == 0)
                ++request;
            if (deviceQuality) {
                socket.writeDatagram(message("R23C", {*current, quint32(requested.mask)}), peer, remote);
        socket.writeDatagram(message("R16C", {*current, request, quint32(freq), quint32(effective),
                                                      1u, quint32(wireQuality)}), peer, remote);
            } else if (splitSupported) {
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
        const bool qualityLimited = deviceQuality.value_or(0) != 0;
        const bool rateConfirmed = cap == effective && hz == freq;
        const bool matchingDelivery = recovery.delivered(freq, effective, now, qualityLimited);
        const bool healthyCadence = freq > 0 && lagMs < 120 &&
                                    (receivedFps >= freq * .94 || (qualityLimited && matchingDelivery));
        const bool lowStep = effective < target && effective < 600000;
        const bool cadenceRecovery = lowStep ? recovery.healthy(freq, effective, tick, now, complete) : healthyCadence;
        const bool healthyRecovery = lagMs < 120 && (cadenceRecovery || (complete > 0 && matchingDelivery));
        bool severe = lost >= 5 || (lost && lost * 100 >= 12 * (complete + lost)) || missing >= 32 ||
                      late >= 24 || (complete && lagMs >= 250);
        const bool pressure = severe || (complete && lagMs >= 180) || lost >= 2 || missing >= 8 || late >= 8 ||
                              (repaired >= 8 && repaired * 100 >= (c.packets - before.packets) * 2);
        pressureIntervals = pressure ? std::min(pressureIntervals + 1, 2) : 0;
        // One repaired burst on an otherwise full-rate link need not reduce JPEG quality.
        bool congestion = severe || (pressure && (!healthyCadence || pressureIntervals >= 2));
        severeIntervals = severe && lost > 0 ? std::min(severeIntervals + 1, 2) : 0;
        if (adaptive && rateConfirmed && congestion && now - changed >= 2000000) {
            // R7+ senders support 0.4 MB/s. Reserve that range for measured, sustained loss.
            const int minimum = config.fps >= 30 && device.value("device_audio_enabled").toBool() ? 500000 : 400000;
            int deviceFloor = deviceMax > 1000000 ? (severeIntervals == 2 ? minimum : 600000) : 700000;
            // A short fade can erase many fragments while the sender is still
            // delivering nearly all 30 pairs. Reserve multi-step reductions
            // for a persisting fade or a real collapse in delivery.
            const bool hardCut = severe && (severeIntervals == 2 || !complete ||
                                            (!qualityLimited && receivedFps < freq * .75) || lagMs >= 250);
            // Measured receive bytes can be low simply because the selected JPEG Q
            // produces small frames. Reduce the confirmed ceiling in bounded steps.
            int floor = std::min(target, deviceFloor);
            int limit = hardCut ? effective * 4 / 5 : effective - 1;
            if (rateBeforeProbe && now - lastProbe < 8000000) {
                limit = std::min(limit, rateBeforeProbe);
                probeAfter = now + 16000000;
            }
            int next = floor;
            for (int rate : rates())
                if (rate >= floor && rate <= limit)
                    next = rate;
            if (next < effective) {
                effective = next;
                changed = now;
                rateBeforeProbe = 0;
            }
        }
        if (!adaptive || congestion || !healthyRecovery)
            stable = 0;
        else if (lost || pressure)
            stable = std::max(0, stable - 2);
        else
            stable = std::min(stable + 1, 20);
        cleanIntervals = adaptive && rateConfirmed && complete > 0 && !lost && !pressure && lagMs < 120
                         ? std::min(cleanIntervals + 1, 3) : 0;
        // Three clean intervals, confirmed by the handheld's sent-pair counters,
        // allow a larger probe even if a manual JPEG target costs some FPS.
        // Legacy senders without delivery counts retain the single-step cadence probe.
        const bool fastProbe = matchingDelivery && cleanIntervals >= 3 && now - changed >= 4000000;
        const bool cadenceProbe = stable >= 8 && now - changed >= 10000000;
        if (adaptive && rateConfirmed && effective < target && now >= probeAfter && (fastProbe || cadenceProbe)) {
            int next = effective;
            for (int rate : rates()) {
                if (rate <= effective) continue;
                if (rate > target || (next != effective && (!fastProbe || rate > effective * 5 / 4))) break;
                next = rate;
            }
            if (next > effective) {
                rateBeforeProbe = effective;
                effective = next;
                changed = lastProbe = now;
                stable = cleanIntervals = 0;
            }
        }
        if (current && remote && now - lastSeen < 2000000)
            socket.writeDatagram(
                message("R3FB", {*current, quint32(now / 1000), quint32(c.packets), quint32(c.complete * 2),
                                 quint32(c.incomplete * 2), quint32(lagMs)}),
                peer, remote);
        QJsonObject report = device;
        if (now - device.value("device_status_us").toInteger() <= SenderWireRate::expiryUs) {
            for (auto it = deviceAudioQueue.begin(); it != deviceAudioQueue.end(); ++it) report[it.key()] = it.value();
            if (now - senderCountersAt <= SenderWireRate::expiryUs)
                for (auto it = senderCounters.begin(); it != senderCounters.end(); ++it) report[it.key()] = it.value();
        }
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
            const qint64 fullBytes = jpegWireBytes + rowSavedBytes + dictionarySavedBytes + saved;
            report["jpeg_compact_screens"] = compactScreens;
            report["jpeg_full_screens"] = fullJpegScreens;
            report["jpeg_wire_bytes"] = jpegWireBytes;
            report["jpeg_header_saved_bytes"] = saved;
            report["jpeg_reconstructed_bytes"] = fullBytes;
            report["jpeg_header_saving_pct"] = 100. * saved / fullBytes;
        }
        report["jpeg_row_supported"] = assembler.rowReuse;
        report["jpeg_dictionary_supported"] = assembler.dictionarySupported;
        report["jpeg_dictionary_screens"] = dictionaryScreens;
        report["jpeg_dictionary_saved_bytes"] = dictionarySavedBytes;
        if (assembler.dictionaryDecoded) {
            report["jpeg_dictionary_decode_mean_us"] = assembler.dictionaryDecodeNs / (1000. * assembler.dictionaryDecoded);
            report["jpeg_dictionary_decode_max_us"] = assembler.dictionaryMaxDecodeNs / 1000.;
        }
        report["selective_repair_supported"] = assembler.repairLimit > 0;
        if (requestedPacing) report["requested_packet_pacing"] = *requestedPacing;
        if (pacingMode && now - lastPacingStatusUs <= SenderWireRate::expiryUs)
            report["device_packet_pacing"] = *pacingMode;
        if (lastTimingMs && now - lastTimingUs <= SenderWireRate::expiryUs)
            report["device_packet_timing"] = timing;
        report["repair_requests"] = assembler.repairRequests;
        report["repair_fragments_requested"] = assembler.requestedFragments;
        report["repair_fragments_received"] = assembler.repairFragments;
        report["repair_wire_fragments_received"] = assembler.repairWireFragments;
        report["repair_fragments_expired"] = assembler.expiredRepairs;
        report["repair_fragments_retired"] = assembler.retiredRepairs;
        report["repair_fragments_duplicate"] = assembler.duplicateRepairs;
        report["repair_completed_pairs"] = assembler.repairedPairs;
        report["repair_completed_tops"] = assembler.repairedTops;
        report["jpeg_row_key_pairs"] = assembler.rowKeyPairs;
        report["jpeg_row_repeat_acks"] = assembler.rowRepeatAcks;
        report["jpeg_row_delta_screens"] = rowDeltaScreens;
        report["jpeg_rows_reused"] = rowsReused;
        report["jpeg_row_saved_bytes"] = rowSavedBytes;
        report["jpeg_reference_misses"] = assembler.rowReferenceMisses;
        report["jpeg_row_invalid"] = assembler.rowInvalid;
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
        senderWireRate.report(report, now);
        report["arrival_lag_ms"] = lagMs;
        report["requested_rate"] = config.rate;
        report["requested_fps"] = config.fps;
        report["requested_jpeg_quality"] = quality;
        report["jpeg_quality_supported"] = deviceQuality.has_value();
        report["jpeg_q70_supported"] = supportsQ70;
        report["jpeg_quality_confirmed"] = canRequestQuality && deviceQuality == quality;
        if (deviceQuality) report["device_jpeg_quality"] = *deviceQuality;
        report["max_pair_gap_ms"] = maxGap / 1000.;
        report["rx_sample_us"] = now;
        report["interval_pair_gap_ms"] = intervalPairGap / 1000.;
        report["interval_top_gap_ms"] = intervalTopGap / 1000.;
        intervalPairGap = intervalTopGap = 0;
        report["effective_rate"] = effective;
        report["adaptive_rate"] = adaptive;
        report["effective_fps"] = freq;
        report["recovery_stable_seconds"] = stable;
        report["recovery_matches_sender"] = matchingDelivery;
        report["config_confirmed"] = cap == effective && hz == freq && canRequestQuality &&
                                     (deviceQuality ? *deviceQuality == quality : quality == 0);
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
