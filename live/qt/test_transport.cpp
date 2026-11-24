#include "transport.h"
#include "test_jpeg_rows.h"
#include <cstdlib>
#include <QDebug>
#include <QRandomGenerator>
using namespace live;
static void require(bool ok, const char *reason) {
    if (!ok) {
        qCritical("%s", reason);
        std::exit(1);
    }
}
static QByteArray packet(QByteArray magic, quint32 seq, quint32 index, const QByteArray &payload) {
    return message(magic, {42, seq, 1000, 2000, 1500, index, 30}) + payload;
}
static void lowRateRecovery() {
    constexpr int allFps = (1 << 30) | (1 << 24) | (1 << 20) | (1 << 15);
    for (int selected : {15, 20, 24, 30})
        require(supportedCaptureFps(selected, allFps, 30) == selected, "Ignored supported capture FPS");
    require(supportedCaptureFps(24, (1 << 20) | (1 << 30), 30) == 20, "Requested unsupported 24 FPS");
    require(supportedCaptureFps(15, (1 << 24) | (1 << 30), 24) == 24, "Lost legacy sender cadence");
    RecoveryCadence recovery;
    qint64 tick = 1000000;
    auto sample = [&](int frames, int fps = 15, int rate = 400000, int duration = 1001000) {
        const qint64 start = tick;
        tick += duration;
        return recovery.healthy(fps, rate, start, tick, frames);
    };
    require(!sample(15), "Missing sender status counted as a recovery window");
    // The sender delivered 45 pairs in three seconds, although a single
    // 14-pair tick would fall below 94% of 15 FPS.
    const int nearRouter[] = {16, 15, 14, 15, 16, 14, 15, 16, 14, 15, 16};
    for (int i = 0; i < 11; ++i) {
        recovery.confirm(15, 400000, tick);
        require(sample(nearRouter[i]) == (i >= 2), "Healthy 14/15/16 cadence did not recover");
    }
    for (int i = 0; i < 8; ++i) {
        recovery.confirm(15, 400000, tick);
        require(!sample(8), "Sustained loss falsely recovered");
    }
    recovery.confirm(24, 500000, tick + 100000);
    require(!sample(24, 24, 500000), "Mixed sender settings contributed to recovery");
    for (int i = 0; i < 3; ++i) {
        recovery.confirm(24, 500000, tick);
        require(sample(24, 24, 500000) == (i == 2), "New sender setting did not establish its own window");
    }
    tick += 1000000;
    recovery.confirm(24, 500000, tick);
    require(!sample(24, 24, 500000), "Non-consecutive low-rate windows were combined");
    require(!sample(30, 24, 500000, 3000000), "Stopped receiver kept stale recovery samples");
    require(!sample(24, 24, 500000), "Expired status counted as a recovery window");
    recovery.confirm(24, 500000, tick);
    require(!sample(0, 24, 500000), "No complete frames counted as a recovery window");
    for (int i = 0; i < 4; ++i) {
        recovery.confirm(24, 500000, tick);
        require(!sample(24, 24, 500000, 1350000), "Recovery ignored actual elapsed time");
    }
}
static void senderLimitedRecovery() {
    RecoveryCadence recovery;
    // Real device at 30 FPS / 600 kB/s. Its encoder produced 642 pairs, the sender
    // transmitted 585 in these 21 reporting intervals, and the receiver recovered all 585.
    const int transmitted[] = {30,29,27,26,30,29,27,28,30,29,30,29,29,29,24,22,25,27,27,28,30};
    const int durations[] = {1000,1010,1010,1010,1020,1040,1010,1030,1050,1010,1020,1030,1000,1020,1020,1020,
                             1020,1030,1020,1030,1010};
    RecoveryCadence::Delivery status{UINT32_MAX-1500, UINT32_MAX-50, 0, 1000};
    int fps = 30, rate = 600000;
    qint64 wall = 1000000;
    recovery.confirm(fps, rate, wall, status);
    auto report = [&](int sent, int received, int ms=1000) {
        status.elapsedMs += ms;
        status.sentPairs += sent;
        status.receivedPairs += received;
        wall += ms*1000;
        recovery.confirm(fps, rate, wall, status);
        return recovery.delivered(fps, rate, wall);
    };
    for (unsigned i=0; i<std::size(transmitted); ++i)
        require(report(transmitted[i],transmitted[i],durations[i]) == (i>=2),
                "Fully delivered sender-limited stream could not probe a higher rate");
    require(!recovery.delivered(30, 650000, wall), "Unconfirmed rate inherited delivery reports");
    require(!recovery.delivered(fps, rate, wall+3000000), "Expired sent counts allowed recovery");
    require(!report(29, 25), "Network loss was mistaken for a slow sender");
    for (int i=0; i<3; ++i) report(10,10);
    require(!recovery.delivered(fps, rate, wall), "A stalled sender counted as recovered");
    require(recovery.delivered(fps, rate, wall, true), "A quality-limited stream with full delivery could not recover");
    report(0,0);
    require(!recovery.delivered(fps, rate, wall, true), "A stationary send counter counted as delivery");
    for (int i=0; i<3; ++i) report(10,8);
    require(!recovery.delivered(fps, rate, wall, true), "Missing quality-limited frames counted as delivery");
    for (int i=0; i<3; ++i) report(27,27);
    ++status.sendFailures;
    require(!report(27,27), "Sender timeout counted as healthy delivery");
    rate=650000;
    require(!report(27,27), "New sender setting reused older delivery history");
    recovery.confirm(fps, rate, wall+1000);
    require(!recovery.delivered(fps, rate, wall+1000), "Absent paired-stream evidence retained delivery history");
}
static void delayedFragments(const QByteArray &source) {
    Assembler assembler;
    auto send = [&](quint32 seq, int index, qint64 now) {
        return assembler.feed(packet("R10P", seq, index, source.mid(index * Chunk, Chunk)), now);
    };
    // A real R12 frame took 780 ms to cross the WLAN while the screen was frozen.
    send(10, 0, 1000);
    send(11, 0, 40000);
    send(12, 0, 70000);
    assembler.expire(250000);
    send(10, 1, 380000);
    auto recovered = send(10, 2, 781000);
    require(recovered && recovered->seq == 10 && recovered->screens[0] + recovered->screens[1] == source,
            "Recoverable frame lost during wireless fade");

    send(13, 0, 782000);
    send(13, 1, 783000);
    auto newer = send(13, 2, 784000);
    require(newer && newer->seq == 13 && assembler.counts.incomplete == 2, "New frame waited for older fragments");
    require(!send(11, 1, 785000) && !send(11, 2, 786000), "Older frame overtook a complete frame");

    for (quint32 seq = 20; seq < 25; ++seq)
        send(seq, 0, 800000 + seq);
    require(assembler.counts.incomplete == 3, "Unbounded partial frame count");
    require(!send(20, 1, 810000) && !send(20, 2, 810001), "Evicted frame reappeared");
    assembler.expire(1900000);
    require(assembler.counts.incomplete == 7, "Stale partials remained after a stopped stream");
    for (int i = 0; i < 3; ++i)
        require(!send(24, i, 1910000 + i), "Expired frame reappeared");
}
static void independentPanels() {
    Assembler a;
    QList<Pair> updates;
    a.onPanels = [&](Pair update) { updates.push_back(std::move(update)); };
    QByteArray upper(Chunk + 203, 'U'), lower(Chunk + 27, 'L');
    const auto content = upper + lower;
    auto fragment = [&](quint32 seq, int index) {
        return message("R10P", {42, seq, seq * 33000, quint32(upper.size()), quint32(lower.size()),
                               quint32(index), 30}) + content.mid(index * Chunk, Chunk);
    };
    a.feed(fragment(1, 1), 1);
    a.feed(fragment(1, 0), 2);
    require(updates.size() == 1 && updates[0].screens[0] == upper && updates[0].screens[1].isEmpty() &&
            a.counts.complete == 0, "Upper did not arrive independently at a mid-fragment screen boundary");
    a.feed(fragment(1, 0), 3);
    auto whole = a.feed(fragment(1, 2), 4);
    require(whole && whole->screens == Screens{upper, lower} && updates.size() == 2 &&
            updates[1].screens[0].isEmpty() && updates[1].screens[1] == lower && a.counts.fullPairs == 1,
            "Late lower screen duplicated or lost upper frame");
    // Lose upper's first piece. The lower panel is independently decodable.
    a.feed(fragment(2, 2), 5);
    a.feed(fragment(2, 1), 6);
    require(updates.size() == 3 && updates.back().screens[1] == lower && updates.back().screens[0].isEmpty(),
            "Lower update incorrectly required a complete top");
    a.feed(fragment(3, 0), 7);
    a.feed(fragment(3, 1), 8);
    a.feed(fragment(2, 0), 9);
    require(updates.size() == 4 && updates.back().seq == 3 && updates.back().screens[0] == upper,
            "Older full pair regressed the upper screen");
    a.reset();
    const quint32 id = 51;
    auto r15 = [&](quint32 seq, int idx, bool bottom, quint32 lowerPts = 0) {
        return message("R15P", {id, seq, seq*33333, quint32(upper.size()), bottom ? quint32(lower.size()) : 0,
                               quint32(idx), 30, lowerPts}) +
               (bottom ? content : upper).mid(idx * Chunk, Chunk);
    };
    a.feed(r15(1, 0, true, 22111), 10);
    a.feed(r15(1, 1, true, 22111), 11);
    a.feed(r15(2, 0, false), 12);
    auto shortSlot = a.feed(r15(2, 1, false), 13);
    require(shortSlot && shortSlot->screens[0] == upper && shortSlot->screens[1].isEmpty() &&
            a.counts.fullPairs == 2 && a.counts.incomplete == 0, "Scheduled empty lower counted as loss or full pair");
    whole = a.feed(r15(1, 2, true, 22111), 14);
    require(whole && updates.back().screens[1] == lower && updates.back().screens[0].isEmpty() &&
            updates.back().bottomPts == 22111, "Newer top-only slot retired a legitimate delayed lower");
    const auto invalid = a.counts.invalid;
    a.feed(r15(3, 0, false, 3), 15);
    auto truncated = r15(3, 0, false);
    a.feed(truncated.left(35), 16);
    require(a.counts.invalid == invalid + 2, "Malformed R15 header accepted");
}
static void recoveredUpper() {
    Assembler a;
    QList<Pair> updates;
    a.onPanels = [&](Pair update) { updates.push_back(std::move(update)); };
    const int topSize = 100, bottomSize = 5 * Chunk + 20 - topSize;
    QByteArray contents(topSize + bottomSize, Qt::Uninitialized);
    for (int i = 0; i < contents.size(); ++i) contents[i] = char((i*13+i/Chunk)&255);
    auto feed = [&](const char *magic, int i, QByteArray bytes) {
        return a.feed(message(magic,{2, 1, 10, quint32(topSize), quint32(bottomSize), quint32(i), 30})+bytes, 50);
    };
    for (int i : {1,2,3,4})
        feed("R10P", i, contents.mid(i*Chunk, Chunk));
    // Upper data missing but its stripe parity and chunk 4 recover it, producing top independently.
    auto stripe = contents.left(Chunk);
    for (int i = 0; i < Chunk; ++i) stripe[i] ^= contents[4*Chunk+i];
    feed("R10F", 0, stripe);
    require(updates.size()==1 && updates[0].screens[0]==contents.left(topSize) && updates[0].screens[1].isEmpty(),
            "FEC failed to deliver upper while the lower tail was still missing");
}
static void restartRows() {
    Assembler assembler;
    assembler.rowReuse = true;
    const Screens original{row_fixture::compact(400), row_fixture::compact(320)};
    const Screens edited{row_fixture::compact(400, 0), row_fixture::compact(320, 14)};
    const quint32 base = UINT32_MAX;
    const Screens deltas{row_fixture::delta(original[0], edited[0], base),
                         row_fixture::delta(original[1], edited[1], base)};
    require(!deltas[0].isEmpty() && !deltas[1].isEmpty(), "Could not create valid restart deltas");
    QList<Pair> published;
    QList<std::optional<quint32>> ack;
    assembler.onPanels = [&](Pair pair) { published.append(std::move(pair)); };
    assembler.onReference = [&](std::optional<quint32> seq) { ack.append(seq); };
    qint64 now = 0;
    auto feed = [&](quint32 seq, const Screens &screens, int skip = -1) {
        const QByteArray packed = screens[0] + screens[1];
        std::optional<Pair> result;
        for (int off = 0; off < packed.size(); off += Chunk) {
            if (off / Chunk == skip) continue;
            auto header = message("R10P", {1, seq, 1234, quint32(screens[0].size()), quint32(screens[1].size()),
                                           quint32(off / Chunk), 30});
            result = assembler.feed(header + packed.mid(off, Chunk), ++now);
        }
        return result;
    };
    Screens keys{row_fixture::key(original[0]), row_fixture::key(original[1])};
    require(!feed(base, keys, 0) && ack.isEmpty(), "ACKed a key before both screens arrived");
    auto key = feed(base, keys);
    // Completion can occur on a retried early fragment, before its duplicate tail.
    require(ack.size() == 1 && ack.front() == base && assembler.counts.fullPairs == 1,
            "Could not ACK a key completed by a retry");
    const auto restored = feed(1, deltas); // Lost intervening frame 0, crossed a sequence wrap.
    require(restored && restored->screens == edited, "Reconstructed screens differ from original JPEG bytes");
    require(published.back().deltaBytes[0] == deltas[0].size() && published.back().reusedRows[1] == 14,
            "Reconstruction lost wire size or reused-row diagnostics");
    auto unknown = deltas;
    qToLittleEndian(quint32(0), unknown[0].data() + 8);
    qToLittleEndian(quint32(0), unknown[1].data() + 8);
    const auto good = published.size();
    require(!feed(2, unknown) && published.size() == good && ack.size() == 2 && !ack.back() &&
            !assembler.acknowledgedReference() && assembler.rowReferenceMisses == 2,
            "Delta with unavailable key escaped or failed to request a new key");
    auto broken = keys;
    broken[0].chop(2);
    require(!feed(3, broken) && ack.size() == 2 && assembler.rowInvalid == 1,
            "Incomplete JPEG key became a reference");
    key = feed(4, keys);
    require(key && key->screens == original && assembler.acknowledgedReference() == 4,
            "Failed to recover on the next intact key");
    const auto initialAcks = ack.size();
    const auto initialPairs = assembler.counts.fullPairs;
    const auto top = quint32(keys[0].size()), bottom = quint32(keys[1].size());
    auto retry = [&](const char *tag, quint32 session, quint32 stamp, quint32 topBytes) {
        return assembler.feed(message(tag, {session, 4, stamp, topBytes, bottom, 0, 30}) + keys[0].left(Chunk), ++now);
    };
    now += 30000;
    retry("R10P", 2, 1234, top);
    retry("R10P", 1, 1235, top);
    retry("R10F", 1, 1234, top + 1);
    require(ack.size() == initialAcks, "Key with mismatched session, timestamp or lengths was ACKed");
    require(!retry("R10P", 1, 1234, top) && !retry("R10F", 1, 1234, top) &&
            ack.size() == initialAcks + 1 && ack.back() == 4,
            "Duplicate fragments did not yield one throttled key ACK");
    now += 30000;
    require(!retry("R10F", 1, 1234, top) && assembler.rowRepeatAcks == 2 &&
            ack.size() == initialAcks + 2 && assembler.counts.fullPairs == initialPairs,
            "Parity-only retry lost its key ACK or published a duplicate frame");
    assembler.reset();
    require(!assembler.rowReuse && !assembler.acknowledgedReference(), "Reset retained the old peer's reference");
    require(!feed(1, deltas), "Accepted deltas after session reset");
    assembler.reset();
    assembler.rowReuse = true;
    const quint32 ref = 5;
    Screens longBase = original;
    QByteArray bytes(35000, Qt::Uninitialized);
    QRandomGenerator random(813);
    for (auto &v : bytes) v = char(random.bounded(1, 255)); // No embedded restart markers.
    longBase[1].insert(longBase[1].size() - 4, bytes);
    Screens changed = longBase;
    changed[1][changed[1].size() - 500] ^= 0x40;
    if (uchar(changed[1][changed[1].size() - 500]) == 0xff) changed[1][changed[1].size() - 500] = 1;
    require(bool(feed(ref, {row_fixture::key(longBase[0]), row_fixture::key(longBase[1])})), "Could not establish compression reference");
    const auto inner = row_fixture::delta(longBase[1], changed[1], ref);
    const auto envelope = row_fixture::dictionary(longBase[1], inner, ref);
    QByteArray independent(inner.size(), Qt::Uninitialized);
    require(LZ4_decompress_safe(envelope.constData() + 20, independent.data(), int(envelope.size()) - 20,
                                int(independent.size())) != inner.size() || independent != inner,
            "R20 test fixture did not actually reference the dictionary");
    quint32 seq = ref + 1;
    auto reject = [&](const QByteArray &bad) {
        auto before = assembler.counts.fullPairs;
        require(!feed(seq++, {edited[0], bad}) && assembler.counts.fullPairs == before,
                "Invalid wrapped bottom reached the complete-pair path");
    };
    reject(envelope); // old session without R20 capability
    assembler.dictionarySupported = true;
    auto wrapped = feed(seq++, {changed[0], envelope});
    require(wrapped && wrapped->screens == changed && published.back().dictionaryWireBytes == envelope.size() &&
            published.back().deltaBytes[1] == inner.size() && assembler.dictionaryDecoded == 1,
            "R20 lossless dictionary round trip or physical size accounting failed");
    for (const int at : {4, 5, 6, 16}) {
        auto bad = envelope;
        bad[at] = char(uchar(bad[at]) + 1);
        reject(bad);
    }
    for (quint32 length : {quint32(0), quint32(inner.size()+1), quint32(128001), UINT32_MAX}) {
        auto bad = envelope;
        qToLittleEndian(length, bad.data() + 12);
        reject(bad);
    }
    reject(envelope.left(20));
    reject(envelope.chopped(2));
    for (int at : {0, 4, 5, 6, 8, 15, 16}) {
        auto bad = inner;
        bad[at] = char(uchar(bad[at]) + 1);
        reject(row_fixture::dictionary(longBase[1], bad, ref));
    }
    require(!feed(seq++, {envelope, edited[1]}), "Accepted a dictionary on the upper screen");
    auto beforeLegacy = published.size();
    assembler.feed(message("R3LP", {1,seq++,1234,1,quint32(envelope.size()),0,30}) + envelope, ++now);
    require(published.size() == beforeLegacy, "Legacy panel path published an R20 dictionary block");
    auto unknownBase = envelope;
    qToLittleEndian(ref - 1, unknownBase.data() + 8);
    reject(unknownBase);
    require(assembler.dictionaryDecoded == 1 && !assembler.acknowledgedReference(),
            "Failed decode counted as a frame or an unavailable reference was retained");
    assembler.reset();
    require(!assembler.dictionarySupported && !assembler.dictionaryDecoded, "R20 state leaked between sessions");
}
static void alignedDeliveryWindow() {
    DeliveryWindow delivery;
    constexpr quint32 epoch = UINT32_MAX - 1500, bytes = UINT32_MAX - 123456, frames = UINT32_MAX - 32;
    constexpr qint64 start = 90000000;
    auto status = [&](quint32 ms, int cap = 2000000) {
        delivery.status({epoch + ms, frames + 30 * ms / 1000, 30, cap, start + ms * 1000,
                         1000000 + ms * 800, 200 + 20 * ms / 1000, 220 + 25 * ms / 1000});
    };
    auto telemetry = [&](quint32 ms, int delay = 0) {
        delivery.wire(epoch + ms, bytes + ms * 1000, start + ms * 1000 + delay);
    };
    telemetry(0, -10000);
    status(0);
    status(1000); // the middle R17T is lost
    status(3000);
    require(delivery.report(start+3010000).isEmpty(), "Unmatched newest status produced comparable rates");
    telemetry(3000, 20000);
    delivery.wire(epoch + 3000, 1, start + 3050000); // duplicate must not rewrite an endpoint
    auto window = delivery.report(start+3050000);
    require(window["seconds"] == 3. && window["start_us"].toInteger() == start &&
            window["end_us"].toInteger() == start + 3000000 && window["device_sent_pairs"].toInt() == 90 &&
            window["received_pairs"].toInt() == 60 && window["received_tops"].toInt() == 75 &&
            window["device_sent_fps"] == 30. && window["top_fps"] == 25. && window["pair_fps"] == 20. &&
            window["device_MBps"] == 1. && window["received_MBps"] == .8,
            "Different report boundaries or counter rollover corrupted aligned rates");
    require(delivery.report(start+4600000).isEmpty(), "Expired aligned window survived");
    telemetry(4100, -10000);
    status(4100, 800000);
    require(delivery.report(start+4150000).isEmpty(), "Combined two different rate caps");
    status(6200, 800000);
    telemetry(6200);
    window = delivery.report(start+6200000);
    require(window["device_cap"] == 800000 && window["start_us"].toInteger() == start+4100000,
            "First reordered sample of the new gear was not preserved");
}
static void selectiveRepair() {
    Assembler a;
    QByteArray top(14 * Chunk + 23, 'T'), bottom(6 * Chunk + 17, 'B');
    const auto jpeg = top + bottom;
    const int n = (jpeg.size() + Chunk - 1) / Chunk;
    std::array<QByteArray,4> parity;
    parity.fill(QByteArray(Chunk, 0));
    auto piece = [&](int index) { return jpeg.mid(index * Chunk, Chunk); };
    for (int i = 0; i < n; ++i) {
        const auto data = piece(i);
        for (int j = 0; j < data.size(); ++j) parity[i % 4][j] ^= data[j];
    }
    auto feed = [&](const char *tag, quint32 seq, int i, qint64 when, quint32 session = 42, quint32 stampDelta = 0) {
        a.feed(message(tag,{session,seq,seq*33333u+stampDelta,quint32(top.size()),quint32(bottom.size()),
                            quint32(i),30}) + (QByteArrayView(tag) == "R10F" ? parity[i] : piece(i)), when);
    };
    QList<quint32> completed, tops;
    a.onComplete = [&](const Pair &pair) {
        require(pair.screens == Screens{top,bottom}, "Repair produced incorrect panel bytes");
        completed.append(pair.seq);
    };
    a.onPanels = [&](Pair pair) {
        if (!pair.screens[0].isEmpty()) tops.append(pair.seq);
    };
    QByteArray requested;
    a.onRepair = [&](quint32 seq, quint32 pts, const QByteArray &indices) {
        require(pts == seq*33333u, "Repair request lost the original capture timestamp");
        requested = indices;
        return true;
    };
    auto burst = [&](quint32 seq, qint64 now) {
        for (int i = 0; i < n; ++i) if (i < 3 || i > 11) feed("R10P",seq,i,now);
        for (int i = 0; i < 4; ++i) feed("R10F",seq,i,now+1000);
        a.pollRepairs(now+1001);
    };
    auto intact = [&](quint32 seq, qint64 now) {
        for (int i = 0; i < n; ++i) feed("R10P",seq,i,now);
    };
    burst(1,1000);
    require(requested.isEmpty(), "Legacy sender received a repair request");
    intact(2,34000);
    a.repairLimit = 16;
    intact(3,67000);
    require(completed == QList<quint32>{2,3} && a.repairRequests == 0, "Healthy stream unexpectedly delayed");
    burst(4,100000);
    require(requested == QByteArray::fromHex("0304050607") && a.repairRequests == 1,
            "Nine consecutive holes should require only five retransmissions with four parity stripes");
    a.pollRepairs(105000);
    feed("R19P",4,3,106000,41);
    feed("R19P",4,3,106000,42,1);
    feed("R19P",4,8,106000);
    feed("R19P",5,3,106000);
    require(a.repairFragments == 0 && a.repairRequests == 1, "Accepted unsolicited or mismatched repairs");
    intact(5,120000);
    require(completed.back() == 3 && tops.back() == 3, "Newer frame overtook a pending repair");
    // The measured median request-to-reply time on the console is 37.4 ms.
    for (char i : requested) feed("R19P",4,uchar(i),138500);
    feed("R19P",4,3,138501);
    require(completed == QList<quint32>{2,3,4,5} && tops == completed && a.repairFragments == 5 &&
            a.repairedPairs == 1 && a.repairedTops == 1, "Did not deliver repaired and deferred originals in order");
    burst(6,160000);
    intact(7,186000);
    a.pollRepairs(201000);
    require(completed.back() == 7 && tops.back() == 7 && a.repairRequests == 2,
            "Failed repair stalled a later complete frame");
    for (char i : requested) feed("R19P",6,uchar(i),202000);
    require(!completed.contains(6) && a.repairedPairs == 1 && a.repairFragments == 5,
            "Expired frame came back after a newer one");
    a.reset();
    require(!a.repairLimit && !a.repairRequests, "Repair permission leaked across session reset");
    a.repairLimit = 2;
    // A long pause midway through an ordered train does not prove loss of its suffix.
    for (int i = 0; i < 7; ++i) feed("R10P",8,i,220000);
    a.pollRepairs(258000);
    require(!a.repairRequests, "Ordered data pause requested the unsent suffix");
    // Only evidenced upper-screen holes fit. The lower screen may still be on its way.
    for (int i = 9; i <= 14; ++i) feed("R10P",8,i,259000);
    a.pollRepairs(270000);
    require(requested == QByteArray::fromHex("0708"), "Early repair did not select only bracketed upper-screen holes");
    intact(9,271000);
    for (char i : requested) feed("R19P",8,uchar(i),272000);
    require(tops.mid(tops.size()-2) == QList<quint32>{8,9} && completed.back() == 9 &&
            !completed.contains(8) && a.repairedTops == 1 && a.repairedPairs == 0,
            "Panel-only recovery held its successor after all requested packets arrived");
    a.repairLimit = 16;
    burst(10,310000);
    // No newer complete pair can be shown. A 55ms response is still useful.
    for (char i : requested) feed("R19P",10,uchar(i),366000);
    require(completed.back() == 10 && a.repairedPairs == 1, "Useful 55 ms response expired at the successor hold deadline");
    burst(11,410000);
    feed("R19P",11,uchar(requested[0]),476002);
    require(completed.back() == 10 && a.expiredRepairs == 1, "Stale retransmission extended the repair deadline");
    // With a delayed frame tail the 90 ms absolute cap wins even when the request itself is recent.
    for (int i = 0; i < n; ++i) if (i < 3 || i > 11) feed("R10P",12,i,510000);
    for (int i = 0; i < 4; ++i) feed("R10F",12,i,557000);
    a.pollRepairs(557001);
    feed("R19P",12,uchar(requested[0]),600001);
    require(a.expiredRepairs == 2 && !completed.contains(12), "Accepted repairs older than the playout budget");
    a.reset();
    a.repairLimit = 16;
    for (int i = 0; i < n-2; ++i) feed("R10P",13,i,710000);
    a.pollRepairs(724000);
    require(!a.repairRequests, "A brief pause was mistaken for a missing end of frame");
    a.pollRepairs(745000);
    QByteArray tail;
    tail.append(char(n-2)).append(char(n-1));
    require(requested == tail && a.repairRequests == 1, "A lost final short tail could not recover on a quiet stream");
    for (char i : requested) feed("R19P",13,uchar(i),761000);
    require(completed.back() == 13, "Short-tail recovery failed");
}
int main() {
    QByteArray source(3500, Qt::Uninitialized);
    for (int i = 0; i < source.size(); ++i)
        source[i] = char(i * 13);
    Assembler assembler;
    auto last = std::numeric_limits<quint32>::max();
    auto first = source.left(Chunk), second = source.mid(Chunk, Chunk), third = source.mid(Chunk * 2);
    require(!assembler.feed(packet("R10P", last, 2, third), 1000), "Incomplete pair published");
    require(!assembler.feed(packet("R10F", last, 0, first), 1010), "Missing stripe silently accepted");
    auto complete = assembler.feed(packet("R10P", last, 1, second), 1020);
    require(complete && complete->screens[0] + complete->screens[1] == source && assembler.counts.repaired == 1,
            "FEC mismatch");
    require(!assembler.feed(packet("R10F", last, 1, second), 1030), "Duplicate produced a frame");
    for (int i = 0; i < 3; ++i)
        complete = assembler.feed(packet("R10P", 0, i, source.mid(i * Chunk, Chunk)), 1040);
    require(complete && complete->seq == 0, "Sequence wrap dropped frame");
    require(!assembler.feed(packet("R10P", 1, 0, first), 1050), "Unexpected frame");
    int reported = 0;
    assembler.onIncomplete = [&](quint32 seq, quint32 pts, qint64 firstPacket, int missing) {
        require(seq == 1 && pts == 1000 && firstPacket == 1050 && missing == 2, "Incomplete trace identity");
        ++reported;
    };
    assembler.expire(1100000);
    require(reported == 1, "Missing incomplete trace");
    require(assembler.counts.incomplete == 1 && assembler.counts.missing == 2, "Partial frame leak");
    require(!assembler.feed(packet("R10F", 2, 4, first), 1200000) && assembler.counts.invalid == 1,
            "Bad stripe accepted");
    assembler.reset();
    QByteArray input(12, 'A'), other(15, 'B');
    require(!assembler.feed(message("R3P2", {43, 0, 10, 1, 15, 0, 30}) + other, 300000),
            "Single screen published");
    auto pair = assembler.feed(message("R3P2", {43, 0, 10, 0, 12, 0, 30}) + input, 300001);
    require(pair && pair->screens[0] == input && pair->screens[1] == other, "Two-screen transport mismatch");
    delayedFragments(source);
    independentPanels();
    recoveredUpper();
    lowRateRecovery();
    senderLimitedRecovery();
    restartRows();
    alignedDeliveryWindow();
    selectiveRepair();
    return 0;
}
