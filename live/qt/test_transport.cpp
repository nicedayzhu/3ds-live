#include "transport.h"
#include <cstdlib>
#include <QDebug>
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
    require(captureFps(30, 800000, 800000, true, allFps, 30) == 30, "Healthy 0.8 MB/s stream lost capture FPS");
    require(captureFps(30, 800000, 600000, true, allFps, 30) == 24, "0.8 -> 0.6 fade kept 30 capture slots");
    require(captureFps(30, 600000, 600000, true, allFps, 30) == 30, "Manual 0.6 MB/s cap lost capture FPS");
    require(captureFps(30, 750000, 600000, true, allFps, 30) == 30, "Smaller rate dip changed capture FPS");
    require(captureFps(30, 800000, 650000, true, allFps, 24) == 30, "Restored budget did not recover 30 FPS");
    require(captureFps(30, 800000, 500000, true, allFps, 30) == 24, "Severe downgrade kept 30 capture slots");
    require(captureFps(30, 500000, 500000, true, allFps, 30) == 30, "Manual rate treated as network loss");
    require(captureFps(30, 800000, 500000, true, allFps ^ (1 << 24), 30) == 20, "Requested unsupported 24 FPS");
    require(captureFps(20, 800000, 500000, true, allFps, 20) == 20, "Raised user's 20 FPS limit");
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
    return 0;
}
