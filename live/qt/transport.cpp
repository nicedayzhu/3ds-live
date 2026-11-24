#include "transport.h"
#include "live_fps.h"
#include <QtEndian>
#include <algorithm>
#include <chrono>
#include "lz4.h"

namespace live {
quint32 word(QByteArrayView b, int i) {
    return qFromLittleEndian<quint32>(b.data() + 4 + i * 4);
}
QByteArray message(QByteArray magic, std::initializer_list<quint32> values) {
    for (quint32 value : values) {
        value = qToLittleEndian(value);
        magic.append(reinterpret_cast<const char *>(&value), 4);
    }
    return magic;
}
static bool newer(quint32 a, quint32 b) {
    return qint32(a - b) > 0;
}
int supportedCaptureFps(int requested, int supported, int current) {
    for (int fps : {30, 24, 20, 15})
        if (fps <= requested && (supported & live_fps_flag(fps))) return fps;
    return live_valid_fps(current) ? current : 0;
}
static constexpr qint64 statusTimeoutUs = 2500000;
void DeliveryWindow::status(Status next) {
    if (!statuses.empty()) {
        const auto &previous = statuses.back();
        if (!newer(next.elapsed, previous.elapsed) || next.wall <= previous.wall) return;
        if (next.fps != previous.fps || next.cap != previous.cap || next.wall - previous.wall > 4000000)
            statuses.clear();
    }
    // A configuration ACK can produce extra status reports between normal 1 Hz samples.
    // Keep the latest boundary in closely spaced bursts and bound both histories.
    if (statuses.size() >= 2 && next.wall - statuses[statuses.size()-2].wall < 250000) statuses.pop_back();
    statuses.push_back(next);
    while (statuses.size() > 32 || next.wall - statuses.front().wall > 4200000) statuses.pop_front();
}
void DeliveryWindow::wire(quint32 elapsed, quint32 mediaBytes, qint64 now) {
    while (!wires.empty() && (now - wires.front().wall > 5500000 || wires.size() >= 64)) wires.pop_front();
    for (const auto &sample : wires)
        if (sample.elapsed == elapsed) return;
    wires.push_back({elapsed, mediaBytes, now});
}
std::optional<quint32> DeliveryWindow::bytesAt(const Status &status) const {
    for (const auto &sample : wires)
        if (sample.elapsed == status.elapsed && std::abs(sample.wall - status.wall) <= 1000000) return sample.bytes;
    return {};
}
QJsonObject DeliveryWindow::report(qint64 now) const {
    if (statuses.size() < 2) return {};
    const auto &last = statuses.back();
    if (now < last.wall || now - last.wall > freshUs) return {};
    const auto endBytes = bytesAt(last);
    if (!endBytes) return {};
    // Select a matched boundary nearest three seconds ago. Every displayed rate
    // uses these exact two PC wall times, even if a middle R17T was lost.
    const Status *first = nullptr;
    quint32 startBytes = 0;
    for (const auto &candidate : statuses) {
        const auto span = last.wall - candidate.wall;
        if (span < 2000000 || span > 4000000 ||
            (first && std::abs(span - 3000000) >= std::abs(last.wall - first->wall - 3000000))) continue;
        const quint32 elapsedMs = last.elapsed - candidate.elapsed;
        // A delayed/replayed status must not bind several seconds of media to a short PC window.
        if (std::abs(qint64(elapsedMs) * 1000 - span) > 500000) continue;
        if (auto bytes = bytesAt(candidate)) {
            first = &candidate;
            startBytes = *bytes;
        }
    }
    if (!first) return {};
    const double seconds = (last.wall - first->wall) / 1e6;
    const auto ms = last.elapsed - first->elapsed, sent = last.sent - first->sent, bytes = *endBytes - startBytes;
    const auto pairs = last.pairs - first->pairs, tops = last.tops - first->tops, received = last.mediaBytes - first->mediaBytes;
    // Both device counters wrap at 32 bits. Reject a reboot/counter discontinuity.
    if (sent > ms * 30 / 1000 + 8 || bytes > quint64(ms) * 2000 + 256000 ||
        pairs < 0 || tops < 0 || received < 0) return {};
    return {{"seconds", seconds}, {"device_seconds", ms / 1000.}, {"start_us", first->wall}, {"end_us", last.wall},
            {"device_sent_pairs", qint64(sent)}, {"received_pairs", pairs}, {"received_tops", tops},
            {"device_media_bytes", qint64(bytes)}, {"received_media_bytes", received},
            {"device_sent_fps", sent / seconds}, {"pair_fps", pairs / seconds}, {"top_fps", tops / seconds},
            {"device_MBps", bytes / seconds / 1e6}, {"received_MBps", received / seconds / 1e6},
            {"device_cap", last.cap}, {"device_fps", last.fps}};
}

void RecoveryCadence::confirm(int fps, int rate, qint64 now, std::optional<Delivery> delivery) {
    if (fps != confirmedFps || rate != confirmedRate || now - lastStatus > statusTimeoutUs) {
        samples.clear();
        reports.clear();
        since = now;
    }
    if (!delivery) reports.clear();
    else {
        if (!reports.empty() && !newer(delivery->elapsedMs, reports.back().elapsedMs)) return;
        reports.push_back(*delivery);
        if (reports.size() > 4) reports.pop_front();
    }
    confirmedFps = fps;
    confirmedRate = rate;
    lastStatus = now;
}
bool RecoveryCadence::delivered(int fps, int rate, qint64 now, bool qualityLimited) const {
    if (reports.size() < 4 || fps <= 0 || fps != confirmedFps || rate != confirmedRate ||
        now - lastStatus > statusTimeoutUs) return false;
    const auto &first = reports.front(), &last = reports.back();
    const quint32 elapsed = last.elapsedMs - first.elapsedMs, sent = last.sentPairs - first.sentPairs;
    const qint64 received = last.receivedPairs - first.receivedPairs;
    // R10 status is emitted between pairs. Its datagram can cross one pair boundary in flight.
    // A fading link or stalled sender must not look healthy just because both counters are small.
    // With a JPEG quality target the source can deliberately send fewer pairs. Require ongoing delivery
    // in each status interval before probing a higher rate at that lower cadence.
    const bool producing = qualityLimited
        ? std::adjacent_find(reports.begin(), reports.end(), [](const Delivery &a, const Delivery &b) {
              return a.sentPairs == b.sentPairs || a.receivedPairs == b.receivedPairs;
          }) == reports.end()
        : sent * 100000LL >= elapsed * qint64(fps) * 75;
    return elapsed >= 2000 && elapsed <= 6000 && first.sendFailures == last.sendFailures && producing &&
           sent <= elapsed * qint64(fps) / 1000 + 2 &&
           received >= qint64(sent) - 1 && received <= qint64(sent) + 1;
}
bool RecoveryCadence::healthy(int fps, int rate, qint64 begin, qint64 end, int complete) {
    if (begin != lastSampleEnd) samples.clear();
    lastSampleEnd = end;
    if (fps <= 0 || fps != confirmedFps || rate != confirmedRate || begin < since ||
        end - lastStatus > statusTimeoutUs || end <= begin || end - begin > 2000000 || complete <= 0) {
        samples.clear();
        return false;
    }
    samples.emplace_back(complete, end - begin);
    if (samples.size() > 3) samples.pop_front();
    int frames = 0;
    qint64 elapsed = 0;
    for (const auto &[count, duration] : samples) {
        frames += count;
        elapsed += duration;
    }
    return samples.size() == 3 && frames * 100000000LL >= elapsed * fps * 94;
}
void Assembler::reset() {
    pending.clear();
    completed.reset();
    newest.reset();
    legacy = {};
    legacySeq = {};
    previous = {};
    published = {};
    references.clear();
    activeReference.reset();
    lastKeyAckUs = 0;
    awaitingKey = true;
    rowReuse = false;
    dictionarySupported = false;
    dictionaryDecodeNs = dictionaryMaxDecodeNs = dictionaryDecoded = 0;
    rowKeyPairs = rowReferenceMisses = rowInvalid = rowRepeatAcks = 0;
    repairLimit = 0;
    repairRequests = requestedFragments = repairFragments = repairedPairs = repairedTops = 0;
    repairWireFragments = expiredRepairs = retiredRepairs = duplicateRepairs = 0;
    combined = false;
}
void Assembler::discard() {
    const auto &p = pending.front();
    if (p.accounted) {
        pending.pop_front();
        return;
    }
    ++counts.incomplete;
    int missing = 0;
    for (const auto &f : p.screens)
        missing += int(f.data.size()) - f.count;
    counts.missing += missing;
    if (onIncomplete)
        onIncomplete(p.seq, p.pts, p.born, missing);
    pending.pop_front();
}
void Assembler::expire(qint64 now) {
    // A wireless fade can spread one frame's fragments over several hundred ms.
    // A newer complete frame already retires every older partial immediately.
    constexpr qint64 maxPartialAgeUs = 1000000;
    while (!pending.empty() && now - pending.front().born > maxPartialAgeUs)
        discard();
}
void Assembler::requestRepair(Partial &p, qint64 now) {
    const int playoutBudget = std::clamp(playoutDelayUs - 10000, 0, 90000);
    const qint64 lastUseful = p.born + std::max(playoutBudget, 33333);
    if (!onRepair || repairLimit <= 0 || !p.combined || p.bottomPts || p.accounted ||
        p.repairUntil || p.unusable[0] || p.unusable[1] || now + 12000 >= lastUseful) return;
    const auto &f = p.screens[0];
    if (f.data.isEmpty() || f.count == f.data.size()) return;
    const auto parity = std::count_if(f.parity.begin(), f.parity.end(), [](const auto &part) { return !part.isEmpty(); });
    const auto idle = now - p.lastProgress;
    if (p.seq == newest && parity != 4 && idle < (parity || !f.data.back().isEmpty() ? 3000 : 10000)) return;
    const int last = int(f.data.size()) - 1;
    const auto nonempty = [](const QByteArray &data) { return !data.isEmpty(); };
    const int highest = last - int(std::find_if(f.data.crbegin(), f.data.crend(), nonempty) - f.data.crbegin());
    // The sender sends R10F only after the original data. Without parity or
    // a successor, a pause midway through its ordered packet train is common.
    const bool ended = p.seq != newest || parity || highest == last ||
                       (highest >= last-2 && idle >= 24000 && now-p.born >= 1000000/p.fps);
    const int topEnd = (p.top - 1) / Chunk, bottomBegin = p.top / Chunk;
    // Keep one hole per usable XOR stripe for local recovery. For a single
    // panel, leave a hole outside that panel first, so its other holes can
    // never prevent reconstruction of the chosen panel.
    auto holes = [&](int begin, int end) {
        std::array<int,4> reserve{-1,-1,-1,-1};
        for (int i = 0; i < f.data.size(); ++i) {
            if (!f.data[i].isEmpty() || f.parity[i % 4].isEmpty()) continue;
            auto &slot = reserve[i % 4];
            if (slot < 0 || (slot >= begin && slot <= end)) slot = i;
        }
        QByteArray needed;
        for (int i = begin; i <= end; ++i)
            if (f.data[i].isEmpty() && reserve[i % 4] != i) needed.append(char(i));
        return needed;
    };
    const int limit = std::min(repairLimit, MaxRepairFragments);
    QByteArray needed;
    if (!ended) {
        // Only spend the one request early on a complete upper screen whose
        // missing indices are bracketed by original data already received.
        if (highest < topEnd || f.data.front().isEmpty() || !p.ready[0].isEmpty()) return;
        needed = holes(0, topEnd);
    } else {
        needed = holes(0, last);
    }
    if (ended && needed.size() > limit) {
        // Request only when the available repair budget can complete a screen.
        needed = holes(0, topEnd);
        if (needed.isEmpty() || needed.size() > limit) needed = holes(bottomBegin, last);
    }
    if (needed.isEmpty() || needed.size() > limit) return;
    if (onRepair(p.seq, p.pts, needed)) {
        ++repairRequests;
        requestedFragments += needed.size();
        p.requestedFragments = std::move(needed);
        // Real Q80 feedback often returns after 33 ms. Accept it while the
        // frame is useful, but give a ready successor a shorter hold budget.
        p.repairUntil = std::min(now + 65000, lastUseful);
        p.holdUntil = std::min({p.repairUntil, p.born + playoutBudget,
                                now + std::min(1000000 / p.fps + 6000, 50000)});
    } else p.repairUntil = now; // A closed or failing socket cannot retry at packet rate.
}
bool Assembler::heldBefore(quint32 seq, qint64 now) const {
    for (const auto &p : pending) {
        if (p.seq == seq) break;
        if (p.holdUntil > now) return true;
    }
    return false;
}
void Assembler::releaseDeferred(qint64 now) {
    // finish() may retire multiple old entries. Keep just sequence IDs across calls.
    std::array<quint32,4> queued{};
    int count = 0;
    for (const auto &p : pending) {
        if (p.deferred) queued[count++] = p.seq;
        if (p.holdUntil > now) break;
    }
    for (int i = 0; i < count; ++i) finish(queued[i], now);
}
void Assembler::pollRepairs(qint64 now) {
    if (!repairLimit) return;
    releaseDeferred(now);
    for (auto &p : pending) {
        requestRepair(p, now);
        if (p.holdUntil > now) break;
    }
}
int Assembler::socketWaitMs(qint64 now) const {
    qint64 wake = now + 10000;
    if (repairLimit) for (const auto &p : pending) {
        if (!p.combined || p.bottomPts || p.accounted || now - p.born >= 90000) continue;
        const auto &parity = p.screens[0].parity;
        const bool tail = std::any_of(parity.begin(), parity.end(), [](const auto &b) { return !b.isEmpty(); });
        const bool lastData = !p.screens[0].data.isEmpty() && !p.screens[0].data.back().isEmpty();
        const auto due = p.repairUntil ? p.holdUntil : p.lastProgress + (tail || lastData ? 3000 : 10000);
        if (due > now) wake = std::min(wake, due);
    }
    return int((wake - now + 999) / 1000);
}
Pair Assembler::complete(Pair pair) {
    if (onComplete) onComplete(pair);
    return pair;
}
void Assembler::Fragments::add(int size, int i, int stripes, bool fec, QByteArrayView payload, Counts &c) {
    if (!total) {
        total = size;
        data.resize((size + Chunk - 1) / Chunk);
    }
    if (total != size) {
        ++c.invalid;
        return;
    }
    QByteArray &slot = fec ? parity[i] : data[i];
    if (!slot.isEmpty())
        return;
    slot = payload.toByteArray();
    if (!fec)
        ++count;
    int group = fec ? i : i % stripes;
    if (parity[group].isEmpty() || count == data.size())
        return;
    int missing = -1;
    for (int j = group; j < data.size(); j += stripes)
        if (data[j].isEmpty()) {
            if (missing >= 0)
                return;
            missing = j;
        }
    if (missing < 0)
        return;
    QByteArray recovered = parity[group];
    char *dst = recovered.data();
    for (int j = group; j < data.size(); j += stripes) {
        const char *src = data[j].constData();
        for (int k = 0; k < data[j].size(); ++k)
            dst[k] ^= src[k];
    }
    recovered.truncate(std::min(Chunk, total - missing * Chunk));
    data[missing] = std::move(recovered);
    ++count;
    ++c.repaired;
}
QByteArray Assembler::Fragments::join() const {
    QByteArray out;
    out.reserve(total);
    for (const auto &part : data)
        out += part;
    return out;
}
QByteArray Assembler::Fragments::range(int begin, int size) const {
    if (size <= 0 || begin + size > total) return {};
    const int first = begin / Chunk, last = (begin + size - 1) / Chunk;
    for (int i = first; i <= last; ++i)
        if (data[i].isEmpty()) return {};
    QByteArray result(size, Qt::Uninitialized);
    for (int i = first; i <= last; ++i) {
        const int lo = std::max(0, begin - i*Chunk), hi = std::min(Chunk, begin+size-i*Chunk);
        auto *dst = result.data() + (i*Chunk + lo - begin);
        memcpy(dst, data[i].constData() + lo, hi - lo);
    }
    return result;
}
void Assembler::publish(Pair update) {
    if (!onPanels) return;
    for (int i = 0; i < 2; ++i) {
        if (update.screens[i].isEmpty()) continue;
        if (!published[i] || newer(update.seq, *published[i])) published[i] = update.seq;
        else update.screens[i].clear();
    }
    if (!update.screens[0].isEmpty() || !update.screens[1].isEmpty())
        onPanels(std::move(update));
}
std::optional<quint32> Assembler::acknowledgedReference() const {
    if (awaitingKey || references.empty()) return {};
    return references.back().seq;
}
bool Assembler::restorePanel(Partial &part, int screen, QByteArray &panel, Pair &update) {
    const bool key = panel.startsWith("R17K");
    const bool dictionary = panel.startsWith("R20D");
    if (!key && !dictionary && !panel.startsWith("R17D")) return true;
    const int width = screen ? 320 : 400;
    if (!rowReuse || !part.combined || part.bottomPts || panel.size() < 8 ||
        qFromLittleEndian<quint16>(panel.constData() + 6) != width) {
        ++rowInvalid;
        return false;
    }
    const auto *bytes = reinterpret_cast<const uint8_t *>(panel.constData());
    if (key) {
        LiveJpegRows rows{};
        if (!live_jpeg_rows_parse(&rows, bytes, panel.size())) {
            ++rowInvalid;
            return false;
        }
        part.keyRows[screen] = rows;
        memcpy(panel.data(), "R8J1", 4);
        return true;
    }
    if (panel.size() < LIVE_JPEG_DELTA_HEADER) {
        ++rowInvalid;
        return false;
    }
    const quint32 base = word(panel, 1);
    if (!newer(part.seq, base) || (!dictionary && (word(panel, 3) < 12 || word(panel, 3) > LIVE_JPEG_WIRE_MAX_PAYLOAD)) ||
        (dictionary &&
        (!dictionarySupported || screen != 1 || panel.size() <= 20 || !live_rows_format_valid(bytes) ||
         word(panel, 3) != 0 || word(panel, 2) < LIVE_JPEG_DELTA_HEADER || word(panel, 2) > 128000))) {
        ++rowInvalid;
        return false;
    }
    const auto ref = std::find_if(references.begin(), references.end(), [&](const Reference &candidate) {
        return candidate.session == part.session && candidate.seq == base;
    });
    if (ref == references.end()) {
        ++rowReferenceMisses;
        if (!awaitingKey && onReference) onReference({});
        awaitingKey = true;
        return false;
    }
    const auto &original = ref->screens[screen];
    const int outerBytes = int(panel.size());
    qint64 decodeNs = 0;
    if (dictionary) {
        if (memcmp(bytes + 4, original.constData() + 4, 4)) { ++rowInvalid; return false; }
        QByteArray inner(word(panel, 2), Qt::Uninitialized);
        const int dictBytes = std::min(32768, int(original.size()) - 8);
        const auto start = std::chrono::steady_clock::now();
        const int decoded = LZ4_decompress_safe_usingDict(panel.constData() + 20, inner.data(), outerBytes - 20,
            int(inner.size()), original.constData() + original.size() - dictBytes, dictBytes);
        decodeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        if (decoded != inner.size() || !inner.startsWith("R17D") ||
            memcmp(inner.constData() + 4, panel.constData() + 4, 8)) { ++rowInvalid; return false; }
        panel = std::move(inner);
        bytes = reinterpret_cast<const uint8_t *>(panel.constData());
    }
    const quint32 length = word(panel, 3);
    if (length < 12 || length > LIVE_JPEG_WIRE_MAX_PAYLOAD) { ++rowInvalid; return false; }
    QByteArray restored(length, Qt::Uninitialized);
    if (live_jpeg_rows_restore(reinterpret_cast<uint8_t *>(restored.data()), length, bytes, panel.size(),
                               reinterpret_cast<const uint8_t *>(original.constData()), original.size(),
                               &ref->rows[screen], base) != length) {
        ++rowInvalid;
        return false;
    }
    update.deltaBytes[screen] = int(panel.size());
    if (dictionary) {
        update.dictionaryWireBytes = outerBytes;
        ++dictionaryDecoded;
        dictionaryDecodeNs += decodeNs;
        dictionaryMaxDecodeNs = std::max(dictionaryMaxDecodeNs, decodeNs);
    }
    update.reusedRows[screen] = LIVE_JPEG_ROWS_COUNT - qPopulationCount(qFromLittleEndian<quint16>(bytes + 12));
    panel = std::move(restored);
    activeReference = base;
    return true;
}
std::optional<Pair> Assembler::feed(QByteArrayView b, qint64 now) {
    ++counts.packets;
    expire(now);
    auto bad = [this]() -> std::optional<Pair> {
        ++counts.invalid;
        return {};
    };
    if (b.size() < 32)
        return bad();
    const auto magic = b.first(4);
    const bool retry = magic == "R19P";
    bool split = magic == "R15P" || magic == "R15F";
    bool old = magic == "R3LP", shared = split || retry || magic == "R10P" || magic == "R10F",
         fec = magic == "R10F" || magic == "R15F" || magic == "R3F2";
    if (!old && !shared && magic != "R3P2" && magic != "R3F2")
        return bad();
    auto sn = word(b, 0), seq = word(b, 1), pts = word(b, 2), a = word(b, 3), z = word(b, 4), off = word(b, 5),
         hz = word(b, 6);
    if ((split && b.size() < 36) || !live_media_fps_flag(hz) ||
        ((z < 12) && !(split && z == 0)) || z > 128000 ||
        (shared ? (a < 12 || a > 128000) : a > 1))
        return bad();
    const auto bottomPts = split ? std::optional<quint32>(word(b, 7)) : std::nullopt;
    if (split && !z && *bottomPts) return bad();
    int stripes = shared ? 4 : 2, size = int(shared ? a + z : z), n = (size + Chunk - 1) / Chunk;
    quint32 index = (!fec && !shared) ? off / Chunk : off;
    auto payload = b.sliced(split ? 36 : 32);
    if (fec ? (index >= quint32(stripes) || payload.size() != Chunk)
            : (index >= quint32(n) || (!shared && off % Chunk) ||
               payload.size() != std::min(Chunk, size - int(index) * Chunk)))
        return bad();
    if (old) {
        int sc = int(a);
        if (legacySeq[sc] != seq) {
            if (legacySeq[sc] && !newer(seq, *legacySeq[sc])) {
                ++counts.late;
                return {};
            }
            if (legacy[sc].total && legacy[sc].count < legacy[sc].data.size())
                ++counts.incomplete;
            legacy[sc] = {};
            legacySeq[sc] = seq;
        }
        auto &f = legacy[sc];
        if (f.total && f.count == f.data.size())
            return {};
        f.add(size, int(index), stripes, false, payload, counts);
        if (f.total != size || f.count != n)
            return {};
        auto panel = f.join();
        if (panel.startsWith("R20D")) return bad();
        previous[sc] = std::move(panel);
        Pair update{sn, seq, pts, {}, int(hz)};
        update.screens[sc] = previous[sc];
        publish(std::move(update));
        if (previous[1 - sc].isEmpty())
            return {};
        ++counts.complete;
        ++counts.fullPairs;
        return complete({sn, seq, pts, previous, int(hz)});
    }
    if (repairLimit) releaseDeferred(now);
    if (retry) {
        if (!repairLimit) return {};
        ++repairWireFragments;
        if (completed && !newer(seq, *completed)) { ++retiredRepairs; return {}; }
    }
    if (completed && !newer(seq, *completed)) {
        if (rowReuse && shared && !split && acknowledgedReference() == seq) {
            const auto &key = references.back();
            if (sn == key.session && pts == key.pts && int(hz) == key.fps &&
                a == key.screens[0].size() && z == key.screens[1].size()) {
                // The sender repeats an unacknowledged key in the next capture
                // slots. Send at most one ACK per burst window, including FEC.
                if (onReference && now - lastKeyAckUs >= 25000) {
                    lastKeyAckUs = now;
                    ++rowRepeatAcks;
                    onReference(seq);
                }
                return {};
            }
        }
        if (seq != *completed)
            ++counts.late;
        return {};
    }
    auto it = std::find_if(pending.begin(), pending.end(), [seq](const Partial &p) { return p.seq == seq; });
    if (it == pending.end()) {
        if (retry) return {}; // A retransmission can only fill an existing, explicitly requested hole.
        if (newest && !newer(seq, *newest)) {
            ++counts.late;
            return {};
        }
        // At most four screen pairs (256 KB of JPEG data each) can be in flight.
        if (pending.size() == 4)
            discard();
        newest = seq;
        combined = shared;
        pending.push_back({seq, pts, int(hz), shared ? int(a) : 0, shared ? int(z) : 0, shared, now, {}, {}, bottomPts});
        it = std::prev(pending.end());
        it->session = sn;
        if (repairLimit) {
            for (auto older = pending.begin(); older != it; ++older) {
                requestRepair(*older, now);
                if (older->holdUntil > now) break;
            }
        }
    }
    if (it->session != sn || it->pts != pts || it->fps != int(hz) || it->combined != shared || it->bottomPts != bottomPts ||
        (shared && (it->top != int(a) || it->bottom != int(z))))
        return bad();
    if (it->accounted) return {};
    auto &s = it->screens;
    if (retry && !it->requestedFragments.contains(char(index))) return {};
    const auto &fragments = s[shared ? 0 : int(a)];
    if (fragments.total && fragments.total != size) return bad();
    const bool isNew = fec ? fragments.parity[index].isEmpty() :
                             (fragments.data.isEmpty() || fragments.data[index].isEmpty());
    if (retry && !isNew) { ++duplicateRepairs; return {}; }
    if (retry && now >= it->repairUntil) { ++expiredRepairs; return {}; }
    if (isNew) it->lastProgress = now;
    int topStripeHole = -1;
    if (retry && isNew) {
        it->usedRepair = true;
        ++repairFragments;
        if (int(index) * Chunk < it->top) it->repairedTopData = true;
        else if (!fragments.parity[index % 4].isEmpty()) {
            for (int i = int(index % 4); i * Chunk < it->top; i += 4)
                if (fragments.data[i].isEmpty()) { topStripeHole = i; break; }
        }
    }
    s[shared ? 0 : int(a)].add(size, int(index), stripes, fec, payload, counts);
    if (topStripeHole >= 0 && !fragments.data[topStripeHole].isEmpty()) it->repairedTopData = true;
    if (it->repairUntil > now &&
        std::all_of(it->requestedFragments.cbegin(), it->requestedFragments.cend(),
                    [&](char i) { return !fragments.data[uchar(i)].isEmpty(); }))
        it->holdUntil = now; // A panel-only request has no further replies to wait for.
    if (repairLimit && heldBefore(seq, now)) {
        it->deferred = true;
        return {};
    }
    auto result = finish(seq, now);
    if (repairLimit) releaseDeferred(now);
    return result;
}
std::optional<Pair> Assembler::finish(quint32 seq, qint64 now) {
    auto it = std::find_if(pending.begin(), pending.end(), [seq](const Partial &p) { return p.seq == seq; });
    if (it == pending.end() || it->accounted) return {};
    it->deferred = false;
    const auto sn = it->session, pts = it->pts;
    const int hz = it->fps, a = it->top, z = it->bottom;
    const auto bottomPts = it->bottomPts;
    const bool shared = it->combined, split = bottomPts.has_value();
    auto &s = it->screens;
    Pair update{sn, seq, pts, {}, int(hz), {}, bottomPts};
    for (int i = 0; i < 2; ++i) {
        if (it->unusable[i] || !it->ready[i].isEmpty()) continue;
        QByteArray panel;
        if (shared)
            panel = s[0].range(i ? int(a) : 0, i ? int(z) : int(a));
        else if (s[i].total && s[i].count == s[i].data.size())
            panel = s[i].join();
        if (panel.isEmpty()) continue;
        if (!restorePanel(*it, i, panel, update)) {
            it->unusable[i] = true;
            continue;
        }
        it->ready[i] = std::move(panel);
        update.screens[i] = it->ready[i];
        if (!i && it->repairedTopData && (!published[0] || newer(seq, *published[0]))) ++repairedTops;
    }
    publish(std::move(update));
    if ((it->ready[0].isEmpty() && !it->unusable[0]) ||
        ((!split || z) && it->ready[1].isEmpty() && !it->unusable[1])) return {};
    if (it->unusable[0] || it->unusable[1]) {
        // All wire data arrived. A missing reference is a codec failure, not a lost UDP fragment.
        while (pending.front().seq != seq) discard();
        pending.pop_front();
        completed = seq;
        return {};
    }
    if (it->keyRows[0] && it->keyRows[1]) {
        // Keep the base still in use if one or more candidate ACKs are lost or reordered.
        if (references.size() == 4) {
            const auto unused = std::find_if(references.begin(), references.end(), [&](const Reference &ref) {
                return ref.seq != activeReference;
            });
            references.erase(unused);
        }
        references.push_back({sn, seq, pts, int(hz), it->ready, {*it->keyRows[0], *it->keyRows[1]}});
        ++rowKeyPairs;
        awaitingKey = false;
        lastKeyAckUs = now;
        if (onReference) onReference(seq);
    }
    Pair slot{sn, seq, pts, it->ready, int(hz), {}, bottomPts};
    ++counts.complete;
    if (split && !z) {
        // A scheduled upper-only slot must not retire an older lower screen still in flight.
        it->accounted = true;
        return complete(std::move(slot));
    }
    if (it->usedRepair) ++repairedPairs;
    while (pending.front().seq != seq)
        discard();
    pending.pop_front();
    completed = seq;
    ++counts.fullPairs;
    return complete(std::move(slot));
}
} // namespace live
