#include "transport.h"
#include <QtEndian>
#include <algorithm>

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
static constexpr qint64 statusTimeoutUs = 2500000;
void RecoveryCadence::confirm(int fps, int rate, qint64 now) {
    if (fps != confirmedFps || rate != confirmedRate || now - lastStatus > statusTimeoutUs) {
        samples.clear();
        since = now;
    }
    confirmedFps = fps;
    confirmedRate = rate;
    lastStatus = now;
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
    bool split = magic == "R15P" || magic == "R15F";
    bool old = magic == "R3LP", shared = split || magic == "R10P" || magic == "R10F",
         fec = magic == "R10F" || magic == "R15F" || magic == "R3F2";
    if (!old && !shared && magic != "R3P2" && magic != "R3F2")
        return bad();
    auto sn = word(b, 0), seq = word(b, 1), pts = word(b, 2), a = word(b, 3), z = word(b, 4), off = word(b, 5),
         hz = word(b, 6);
    if ((split && b.size() < 36) || (hz != 15 && hz != 20 && hz != 24 && hz != 30) ||
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
        previous[sc] = f.join();
        Pair update{sn, seq, pts, {}, int(hz)};
        update.screens[sc] = previous[sc];
        publish(std::move(update));
        if (previous[1 - sc].isEmpty())
            return {};
        ++counts.complete;
        ++counts.fullPairs;
        return Pair{sn, seq, pts, previous, int(hz)};
    }
    if (completed && !newer(seq, *completed)) {
        if (seq != *completed)
            ++counts.late;
        return {};
    }
    auto it = std::find_if(pending.begin(), pending.end(), [seq](const Partial &p) { return p.seq == seq; });
    if (it == pending.end()) {
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
    }
    if (it->pts != pts || it->fps != int(hz) || it->combined != shared || it->bottomPts != bottomPts ||
        (shared && (it->top != int(a) || it->bottom != int(z))))
        return bad();
    if (it->accounted) return {};
    auto &s = it->screens;
    s[shared ? 0 : int(a)].add(size, int(index), stripes, fec, payload, counts);
    Pair update{sn, seq, pts, {}, int(hz), {}, bottomPts};
    for (int i = 0; i < 2; ++i) {
        if (!it->ready[i].isEmpty()) continue;
        if (shared)
            it->ready[i] = s[0].range(i ? int(a) : 0, i ? int(z) : int(a));
        else if (s[i].total && s[i].count == s[i].data.size())
            it->ready[i] = s[i].join();
        update.screens[i] = it->ready[i];
    }
    publish(std::move(update));
    if (it->ready[0].isEmpty() || ((!split || z) && it->ready[1].isEmpty())) return {};
    Pair slot{sn, seq, pts, it->ready, int(hz), {}, bottomPts};
    ++counts.complete;
    if (split && !z) {
        // A scheduled upper-only slot must not retire an older lower screen still in flight.
        it->accounted = true;
        return slot;
    }
    while (pending.front().seq != seq)
        discard();
    pending.pop_front();
    completed = seq;
    ++counts.fullPairs;
    return slot;
}
} // namespace live
