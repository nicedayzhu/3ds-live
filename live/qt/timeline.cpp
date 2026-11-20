#include "timeline.h"
#include "imaging.h"
#include "transport.h"
#include "live_audio.h"
#include <QtEndian>
#include <QPainter>
#include <chrono>
#include <cmath>
#include <windows.h>
#include <mmsystem.h>
extern "C" int live_audio_output_48k(const uint8_t *, size_t, double, int64_t, uint8_t *, size_t);
namespace live {
static constexpr qint64 frameHistoryUs = 1000000;
static int newerPanels(const SourceFrame &source, const std::array<std::optional<PanelStamp>,2> &shown) {
    int mask = 0;
    for (int i = 0; i < 2; ++i)
        if (source.panels[i] && (!shown[i] || source.panels[i]->pts > shown[i]->pts))
            mask |= 1 << i;
    return mask;
}
static bool matchesQuality(const QString &quality, const QString &mode) {
    return quality == mode || quality.startsWith(mode + '+');
}
void Timeline::reset(quint32 id) {
    std::lock_guard lock(mutex);
    session = id;
    newSession = true;
    active = true;
    mapped = false;
    audioAnchor = false;
    frames.clear();
    lastPanels = {};
    lastChoiceKey.reset();
    lastRealCanvas = {};
    holdingGeneratedTop = false;
    lastEnhancedPts.reset();
    lastRawPts.reset();
    audio.clear();
    seen.clear();
    order.clear();
}
qint64 Timeline::observe(quint32 raw, qint64 wall) {
    qint64 pts = mapped ? newest + qint32(raw - quint32(newest)) : qint64(raw);
    if (!mapped || pts > newest)
        newest = pts;
    offset = mapped ? std::min(offset, wall - pts) : wall - pts;
    mapped = true;
    return pts;
}
SourceFrame Timeline::arrival(quint32 id, quint32 seq, quint32 raw, qint64 wall, int panelMask,
                              std::optional<quint32> bottomPts) {
    std::lock_guard lock(mutex);
    SourceFrame source{id, seq, observe(raw, wall), wall};
    if (!panelMask) {
        diagnostics.add({"arrive", source, wall, source.pts + offset + delay});
        return source;
    }
    for (int i = 0; i < 2; ++i)
        if (panelMask & (1 << i)) {
            const qint64 pts = source.pts + (i && bottomPts ? qint32(*bottomPts - raw) : 0);
            source.panels[i] = PanelStamp{seq, pts};
            diagnostics.add({i ? "arrive_bottom" : "arrive_top", source, wall, pts + offset + delay});
        }
    return source;
}
void Timeline::paceSource(Frame &frame, int fps) {
    const qint64 pts = frame.source.pts;
    if (lastRawPts && pts <= *lastRawPts)
        return;
    if (lastRawPts && pts - *lastRawPts < 150000) {
        const qint64 expected = rawDisplayPts + 1000000 / fps;
        // Spread a missing capture interval over the following real images.
        // Bound the phase change against the unmodified shared audio clock.
        const qint64 correction = std::clamp<qint64>((expected - pts) * 2 / 3, -12000, 12000);
        frame.displayPts = std::max(rawDisplayPts + 1, pts + correction);
    }
    lastRawPts = pts;
    rawDisplayPts = frame.displayPts;
}
void Timeline::notifyEarlierFrame(qint64 displayPts) {
    // The GUI may be waiting for a different frame, or just polling its controls.
    // Coalesce split-screen updates until present() schedules its next wake.
    if (nextWakeAt && displayPts + offset + delay < nextWakeAt) {
        nextWakeAt = 0;
        emit earlierFrameReady();
    }
}
void Timeline::put(SourceFrame source, const QImage &image, const QString &quality, bool synthetic, int fps,
                   int generatedPanels) {
    std::lock_guard lock(mutex);
    const bool requireFbcnn = !requiredQuality.isEmpty();
    if (!active || source.session != session || (!requireFbcnn && source.pts < newest - frameHistoryUs))
        return;
    if (!synthetic && !source.panels[0] && !source.panels[1])
        source.panels.fill(PanelStamp{source.sequence, source.pts});
    const qint64 key = !synthetic && source.panels[1] ? std::max(source.pts, source.panels[1]->pts) : source.pts;
    auto now = clockUs(), due = offset + key + delay;
    if (requireFbcnn && quality.isEmpty()) {
        diagnostics.add({"raw", source, now, due, 0, image.cacheKey()});
        return;
    }
    if (image.isNull() || (requireFbcnn && !matchesQuality(quality, requiredQuality)))
        return;
    if (synthetic && !(generatedPanels & 3))
        return;
    const bool alreadyShown = !newSession && lastChoiceKey &&
        (key < *lastChoiceKey || (key == *lastChoiceKey && (synthetic || !newerPanels(source, lastPanels))));
    if (alreadyShown || ((!quality.isEmpty() || synthetic) && (!requireFbcnn || synthetic) && now > due)) {
        diagnostics.add({"late", source, now, due, 0, 0, quality});
        return;
    }
    if (requireFbcnn && now > due)
        diagnostics.add({"enhance_after_deadline", source, now, due, 0, image.cacheKey(), quality});
    auto [pos, inserted] = frames.try_emplace(key);
    auto &frame = pos->second;
    if (!inserted && synthetic != frame.synthetic) {
        if (synthetic) return;
        // A real screen can land exactly on a queued interpolation timestamp.
        frame = {};
        inserted = true;
    }
    if (!inserted && !synthetic && !frame.synthetic) {
        if (newerPanels(frame.source, source.panels))
            return;
        if (newerPanels(source, frame.source.panels)) {
            // The old enhanced canvas must not be associated with a newer lower screen.
            frame.enhanced = {};
            frame.quality.clear();
        }
    }
    frame.source = source;
    frame.synthetic = synthetic;
    frame.generatedPanels = synthetic ? generatedPanels : 0;
    if (inserted) {
        frame.displayPts = key;
        if (synthetic && requireFbcnn && pos != frames.begin() && std::next(pos) != frames.end()) {
            const auto &before = std::prev(pos)->second, &after = std::next(pos)->second;
            if (!before.synthetic && !after.synthetic)
                frame.displayPts = (before.displayPts + after.displayPts) / 2;
        }
        if (key == source.pts && !requireFbcnn && !motionEnabled && fps > 0 && quality.isEmpty() && !synthetic)
            paceSource(frame, fps);
    }
    if (requireFbcnn && !synthetic && key == source.pts && (!lastEnhancedPts || source.pts > *lastEnhancedPts)) {
        const qint64 delta = lastEnhancedPts ? source.pts - *lastEnhancedPts : 0;
        if (delta > 0 && delta < frameHistoryUs) {
            // Preserve the cadence across missing pictures so the next restored
            // frame can shorten the hold within the existing A/V tolerance.
            if (delta < 100000)
                enhancedPeriodUs = .94 * enhancedPeriodUs + .06 * delta;
            const auto predicted = lastDisplayPts + enhancedPeriodUs;
            // Keep the picture within half a 30 FPS frame of the game's audio clock.
            frame.displayPts = std::clamp<qint64>(qRound64(.86 * predicted + .14 * source.pts),
                                                  source.pts - 15500, source.pts + 15500);
        } else
            enhancedPeriodUs = 33333.;
        lastEnhancedPts = source.pts;
        lastDisplayPts = frame.displayPts;
    }
    if (quality.isEmpty() && !synthetic)
        frame.original = image;
    else {
        frame.enhanced = image;
        frame.quality = quality;
    }
    diagnostics.add(
        {quality.isEmpty() && !synthetic ? "raw" : "enhance", source, now, due, 0, image.cacheKey(), quality});
    notifyEarlierFrame(frame.displayPts);
    while (frames.size() > 64 || frames.begin()->first < frames.rbegin()->first - frameHistoryUs)
        frames.erase(frames.begin());
}
qint64 Timeline::deadline(SourceFrame source) const {
    std::lock_guard lock(mutex);
    return source.session == session ? source.pts + offset + delay : 0;
}
void Timeline::setPictureMode(const QString &mode, bool interpolate) {
    std::lock_guard lock(mutex);
    const bool strict = usesFbcnn(mode);
    lastEnhancedPts.reset();
    lastRawPts.reset();
    lastRealCanvas = {};
    holdingGeneratedTop = false;
    motionEnabled = interpolate;
    const auto now = clockUs();
    diagnostics.add({strict ? "mode_fbcnn" : "mode_other", {}, now, 0, 0, 0, mode});
    const QString required = strict ? mode : QString{};
    if (requiredQuality != required) {
        requiredQuality = required;
        frames.clear();
        newSession = true;
        if (strict && (choices.empty() || !matchesQuality(choices.back().quality, mode))) {
            PresentedFrame waiting{{}, {}, "waiting_fbcnn", false, now, ++nextChoice};
            choices.push_back(waiting);
            diagnostics.add({"clear", {}, now, 0, waiting.id, 0, waiting.quality});
        }
        return;
    }
    for (auto it = frames.begin(); it != frames.end();) {
        if (it->second.synthetic) {
            it = frames.erase(it);
            continue;
        }
        it->second.enhanced = {};
        it->second.displayPts = it->first;
        it->second.quality.clear();
        ++it;
    }
}
void Timeline::clearSynthetic() {
    std::lock_guard lock(mutex);
    for (auto it = frames.begin(); it != frames.end();) {
        if (it->second.synthetic)
            it = frames.erase(it);
        else
            ++it;
    }
}
PresentedFrame Timeline::present(int *nextWakeMs) {
    std::lock_guard lock(mutex);
    const auto wall = clockUs();
    const auto playablePts = wall - offset - delay;
    auto it = frames.upper_bound(playablePts + 15500);
    while (it != frames.begin() && std::prev(it)->second.displayPts > playablePts)
        --it;
    if (nextWakeMs) {
        if (it != frames.end()) {
            const qint64 remaining = it->second.displayPts - playablePts;
            *nextWakeMs = int(std::clamp<qint64>((remaining + 999) / 1000, 1, motionEnabled ? 4 : 16));
        } else {
            // A worker wakes us when an image is ready. Keep servicing UI controls while idle.
            *nextWakeMs = 16;
        }
    }
    PresentedFrame next;
    bool hasChoice = false;
    qint64 choiceTarget = 0;
    if (it != frames.begin()) {
        auto choice = std::prev(it);
        if (choice->second.synthetic && choice != frames.begin()) {
            auto real = std::prev(choice);
            // If a late GUI wake makes both frames due, spend this refresh on
            // the unshown source. Showing the overdue midpoint would skip it.
            if (!real->second.synthetic && real->second.displayPts <= playablePts &&
                (newSession || !lastChoiceKey || real->first > *lastChoiceKey) &&
                (!real->second.enhanced.isNull() || !real->second.original.isNull())) {
                frames.erase(choice);
                choice = real;
            }
        }
        const auto &f = choice->second;
        const int updates = f.synthetic ? 0 : newerPanels(f.source, lastPanels);
        if (newSession || !lastChoiceKey || choice->first > *lastChoiceKey ||
            (choice->first == *lastChoiceKey && updates)) {
            auto im = f.enhanced.isNull() ? f.original : f.enhanced;
            if (!im.isNull()) {
                next = PresentedFrame{
                    f.source,    im,   f.quality.isEmpty() ? QStringLiteral("original") : f.quality,
                    f.synthetic, wall, 0, updates, f.generatedPanels & 3};
                if (!newSession && !choices.empty() && !lastRealCanvas.isNull()) {
                    for (int i = 0; i < 2; ++i) {
                        const bool stale = !f.synthetic && lastPanels[i] &&
                            (!f.source.panels[i] || f.source.panels[i]->pts < lastPanels[i]->pts);
                        const bool holdTop = i == 0 && holdingGeneratedTop && !((updates | next.generatedPanels) & 1);
                        if (!stale && !holdTop) continue;
                        const QImage &previous = i ? lastRealCanvas : choices.back().image;
                        const QRect dest(0, i * next.image.height()/2, next.image.width(), next.image.height()/2);
                        const QRect src(0, i * previous.height()/2, previous.width(), previous.height()/2);
                        QPainter painter(&next.image);
                        painter.drawImage(dest, previous, src);
                        if (stale) {
                            next.source.panels[i] = lastPanels[i];
                            if (!i) {
                                next.source.sequence = lastPanels[0]->sequence;
                                next.source.pts = lastPanels[0]->pts;
                            }
                        }
                    }
                }
                if (updates & 1) holdingGeneratedTop = false;
                if (next.generatedPanels & 1) holdingGeneratedTop = true;
                if (!f.synthetic) {
                    for (int i = 0; i < 2; ++i)
                        if (updates & (1 << i))
                            lastPanels[i] = f.source.panels[i];
                    lastRealCanvas = next.image;
                }
                hasChoice = true;
                lastChoiceKey = choice->first;
                newSession = false;
                choiceTarget = f.displayPts + offset + delay;
            }
        }
    }
    if (hasChoice) {
        next.id = ++nextChoice;
        choices.push_back(next);
        const char *kind = next.synthetic ? (next.generatedPanels & 1 ? "synthetic" : "synthetic_bottom") :
                           next.updatedPanels & 1 ? "select" :
                           next.updatedPanels & 2 ? "select_bottom" : "select_revision";
        TraceEvent event{kind, next.source, wall, choiceTarget, next.id, next.image.cacheKey(), next.quality};
        event.updatedPanels = next.updatedPanels;
        event.generatedPanels = next.generatedPanels;
        diagnostics.add(std::move(event));
        // Keep the exact composite selected for each MP4/RTSP sampling time.
        while (choices.size() > 1 && choices[1].time < wall - 1250000)
            choices.pop_front();
    }
    if (nextWakeMs)
        nextWakeAt = wall + *nextWakeMs * 1000;
    return choices.empty() ? PresentedFrame{} : choices.back();
}
PresentedFrame Timeline::recorded(qint64 wall) const {
    std::lock_guard lock(mutex);
    auto it = std::upper_bound(choices.begin(), choices.end(), wall,
                               [](qint64 t, const PresentedFrame &p) { return t < p.time; });
    return it == choices.begin() ? PresentedFrame{} : *std::prev(it);
}
void Timeline::addAudio(QByteArrayView b, qint64 wall) {
    if (b.size() < 24) {
        ++invalidAudio;
        return;
    }
    auto u16 = [&](int i) { return qFromLittleEndian<quint16>(b.constData() + i); };
    int count = u16(12);
    if (count < 1 || count > 6 || u16(14) || b.size() != 24 + count * 175 ||
        word(b, 3) != LIVE_AUDIO_RATE_MHZ || u16(20) != 160 || u16(22) != 2) {
        ++invalidAudio;
        return;
    }
    std::lock_guard lock(mutex);
    constexpr double duration = 160e9 / LIVE_AUDIO_RATE_MHZ;
    for (int i = 0; i < count; ++i) {
        const char *p = b.constData() + 24 + i * 175;
        auto seq = qFromLittleEndian<quint32>(p);
        if (seen.contains(seq)) {
            ++duplicates;
            continue;
        }
        double pts = double(observe(qFromLittleEndian<quint32>(p + 4), wall));
        double expected = anchorPts + qint32(seq - anchorSeq) * duration;
        if (!audioAnchor || std::abs(expected - pts) > 12000) {
            anchorSeq = seq;
            anchorPts = pts;
            audioAnchor = true;
        } else
            pts = expected;
        auto first = qRound64(pts * .048), end = qRound64((pts + duration) * .048);
        QByteArray data((end - first) * 4, Qt::Uninitialized);
        if (!live_audio_output_48k(reinterpret_cast<const uint8_t *>(p + 8), 167, pts, first,
                                   reinterpret_cast<uint8_t *>(data.data()), end - first)) {
            ++invalidAudio;
            continue;
        }
        seen.insert(seq);
        order.push_back(seq);
        if (order.size() > 320) {
            seen.remove(order.front());
            order.pop_front();
        }
        audio[first] = std::move(data);
        ++audioBlocks;
    }
    while (audio.size() > 300)
        audio.erase(audio.begin());
}
QByteArray Timeline::pcm(qint64 wall, int samples) const {
    QByteArray out(samples * 4, 0);
    std::lock_guard lock(mutex);
    if (!mapped || audio.empty())
        return out;
    qint64 start = qRound64(double(wall - offset - delay) * .048);
    auto it = audio.upper_bound(start);
    if (it != audio.begin())
        --it;
    for (; it != audio.end() && it->first < start + samples; ++it) {
        auto lo = std::max(it->first, start);
        auto hi = std::min<qint64>(it->first + it->second.size() / 4, start + samples);
        if (hi > lo)
            memcpy(out.data() + (lo - start) * 4, it->second.constData() + (lo - it->first) * 4, (hi - lo) * 4);
    }
    return out;
}
Speaker::Speaker(Timeline &timeline) {
    worker.reset(QThread::create([this, &timeline] {
        WAVEFORMATEX format{WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
        auto wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake)
            return;
        HWAVEOUT device{};
        if (waveOutOpen(&device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(wake), 0, CALLBACK_EVENT) !=
            MMSYSERR_NOERROR) {
            CloseHandle(wake);
            return;
        }
        opened = true;
        std::array<std::array<char, 1920>, 4> buffers{};
        std::array<WAVEHDR, 4> headers{};
        for (size_t i = 0; i < headers.size(); ++i) {
            headers[i].lpData = buffers[i].data();
            headers[i].dwBufferLength = 1920;
            if (waveOutPrepareHeader(device, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                stop = true;
                break;
            }
        }
        qint64 next = clockUs();
        size_t i = 0;
        while (!stop) {
            auto &h = headers[i];
            if (h.dwFlags & WHDR_INQUEUE) {
                WaitForSingleObject(wake, 10);
                continue;
            }
            next = std::max(next, clockUs());
            auto data = timeline.pcm(next, 480);
            if (mute)
                memset(h.lpData, 0, 1920);
            else
                memcpy(h.lpData, data.constData(), 1920);
            if (waveOutWrite(device, &h, sizeof(h)) != MMSYSERR_NOERROR)
                break;
            next += 10000;
            i = (i + 1) % headers.size();
        }
        waveOutReset(device);
        for (auto &h : headers)
            if (h.dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(device, &h, sizeof(h));
        waveOutClose(device);
        CloseHandle(wake);
        opened = false;
    }));
    worker->start();
}
Speaker::~Speaker() {
    stop = true;
    worker->wait();
}
} // namespace live
