#include "recording.h"
#include "effect_load.h"
#include <QtTest>
#include <QtEndian>
#include <windows.h>
extern "C" {
#include <libavformat/avformat.h>
}

static std::atomic<bool> simulateFullDisk{false};
static int openTestIO(AVIOContext **out, const char *url, int flags, const AVIOInterruptCB *cb,
                      AVDictionary **options) {
    int result = avio_open2(out, url, flags, cb, options);
    if (result >= 0 && simulateFullDisk) {
        (*out)->write_packet = [](void *, const uint8_t *, int) { return AVERROR(ENOSPC); };
    }
    return result;
}
// Build the production recorder with one replaceable I/O boundary in this test binary only.
#define avio_open2 openTestIO
#include "recording.cpp"
#undef avio_open2

using namespace live;
static bool waitState(Recording &rec, const QString &value) {
    QElapsedTimer limit;
    limit.start();
    while (limit.elapsed() < 5000) {
        const auto state = rec.stats()["state"].toString();
        if (state == value)
            return true;
        if (state == "failed" || state == "finished")
            return false;
        QThread::msleep(5);
    }
    return false;
}
static void verifyOutput(const QString &path, bool expectSuccess) {
    Timeline timeline(100);
    Recording rec(timeline, path, 1, 30, true);
    if (!waitState(rec, "recording")) {
        QVERIFY(!expectSuccess);
    } else {
        QThread::msleep(120);
        rec.stop();
    }
    QVERIFY2(waitState(rec, expectSuccess ? "finished" : "failed"),
             qPrintable(rec.stats()["error"].toString()));
    if (!expectSuccess)
        QVERIFY(!rec.stats()["error"].toString().isEmpty());
}

class MediaTests : public QObject {
    Q_OBJECT
  private slots:
    void onlySustainedLocalInferenceOverloadRecommendsReducingEffects() {
        EffectLoad load;
        qint64 t = 1000000, calls = 0, us = 0, skips = 0;
        auto sample = [&](int callCount, int costPerCall, int skipped, double receivedFps = 30.) {
            t += 1000000;
            calls += callCount;
            us += callCount * costPerCall;
            skips += skipped;
            return load.observe(t, calls, us, skips, receivedFps);
        };
        QVERIFY(!sample(0, 0, 0));
        for (int i = 0; i < 4; ++i) QVERIFY(!sample(5, 45000, 5, 8.)); // sparse arrivals
        for (int i = 0; i < 4; ++i) QVERIFY(!sample(20, 10000, 8)); // backlog elsewhere
        for (int i = 0; i < 4; ++i) QVERIFY(!sample(20, 40000, 0)); // still meeting deadlines
        QVERIFY(!sample(20, 40000, 8));
        QVERIFY(!sample(20, 40000, 8));
        QVERIFY(sample(20, 40000, 8));
        QVERIFY(sample(20, 10000, 0));
        QVERIFY(sample(20, 10000, 0));
        QVERIFY(!sample(20, 10000, 0));
        QVERIFY(!sample(0, 0, 20));
        QVERIFY(!sample(0, 0, 20));
        QVERIFY(sample(1, 2700000, 20)); // one real completion across three windows
        QVERIFY(!sample(0, 0, 0, 0.));  // disconnect
    }
    void missingAudioFadesOnlyInsideTheGap() {
        Timeline timeline(0);
        timeline.reset(1);
        constexpr qint64 origin = 1000000;
        auto block = [&](quint32 seq, quint32 pts, qint16 value) {
            QByteArray b(24 + 175, 0);
            memcpy(b.data(), "R9A1", 4);
            qToLittleEndian<quint16>(1, b.data() + 12);
            qToLittleEndian<quint32>(32728498, b.data() + 16);
            qToLittleEndian<quint16>(160, b.data() + 20);
            qToLittleEndian<quint16>(2, b.data() + 22);
            qToLittleEndian(seq, b.data() + 24);
            qToLittleEndian(pts, b.data() + 28);
            // Zero IMA codes at index zero preserve the initial sample on each channel.
            qToLittleEndian(value, b.data() + 32);
            qToLittleEndian<qint16>(-value, b.data() + 36);
            timeline.addAudio(b, origin + pts);
        };
        block(1, 0, 12000);
        block(2, 4889, 12000);
        block(3, 30000, -9000);
        const auto pcm = timeline.pcm(origin, 1600);
        auto sample = [&](int frame, int channel) {
            return qFromLittleEndian<qint16>(pcm.constData() + 4 * frame + 2 * channel);
        };
        QCOMPARE(sample(234, 0), 12000);
        QCOMPARE(sample(235, 0), 12000);
        QCOMPARE(sample(468, 0), 12000);
        QVERIFY(sample(469, 0) > 11900 && sample(469, 0) < 12000);
        QCOMPARE(sample(469, 1), -sample(469, 0));
        QCOMPARE(sample(900, 0), 0);
        QVERIFY(sample(1439, 0) < -8900 && sample(1439, 0) > -9000);
        QCOMPARE(sample(1440, 0), -9000);
        QCOMPARE(sample(1440, 1), 9000);
        // The speaker and recorder ask for differently sized windows; the fill is sample-aligned.
        QCOMPARE(timeline.pcm(origin + 10000, 960), pcm.mid(480 * 4, 960 * 4));
    }
    void restoredUpperWakesIdlePresentation() {
        Timeline t(160);
        t.setPictureMode("fbcnn");
        t.reset(1);
        QSignalSpy wake(&t, &Timeline::earlierFrameReady);
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::green);
        int interval = 0;
        t.present(&interval);
        QCOMPARE(interval, 16);

        const auto origin = clockUs() - 500000;
        auto future = t.arrival(1, 3, 1000000, origin + 1000000);
        t.put(future, image, "fbcnn");
        QCOMPARE(wake.count(), 0);
        auto due = t.arrival(1, 1, 0, origin, 1);
        t.put(due, image); // Strict mode waits for the restored upper image.
        QCOMPARE(wake.count(), 0);
        t.put(due, image, "fbcnn");
        auto both = t.arrival(1, 1, 0, origin, 3);
        t.put(both, image, "fbcnn");
        QCOMPARE(wake.count(), 1);
        QCOMPARE(t.present(&interval).source.sequence, 1u);
        auto next = t.arrival(1, 2, 33333, origin + 33333);
        t.put(next, image, "fbcnn");
        QCOMPARE(wake.count(), 2);
        QCOMPARE(t.present(&interval).source.sequence, 2u);
    }
    void frameDropDiagnosis() {
        auto sample = [](int second) {
            qint64 t = second * 1000000LL;
            return QJsonObject{{"connection","live"},{"session",23},{"wall_us",t},{"rx_sample_us",t},
                {"device_status_us",t},{"device_elapsed_ms",second*1000},{"device_sent_top",30*second},
                {"pairs",30*second},{"presented_frames",60*second},{"synthetic_at_presentation",30*second},
                {"capture_fps",30},{"requested_fps",30},{"mode","fbcnn"},{"motion","rife"},
                {"incomplete",10},{"enhancement_overwrites",0},{"decode_overwrites",0},
                {"canvas_coalesced_updates",3},{"canvas_visible",true}};
        };
        QJsonArray history{sample(10), sample(11), sample(12)};
        const auto healthy = sample(13);
        auto stages = [&](QJsonObject row) { return diagnoseFrameDrops(row, history).value("stages").toArray(); };
        QVERIFY(stages(healthy).isEmpty());
        auto r = healthy;
        r["pairs"] = 360;
        r["presented_frames"] = 750;
        r["incomplete"] = 17;
        QCOMPARE(stages(r), QJsonArray{"input"});
        r["device_status_us"] = 0;
        QCOMPARE(stages(r), QJsonArray{"input"});
        r["presented_frames"] = 730;
        r["enhancement_overwrites"] = 8;
        const QJsonArray mixed{"input","processing"};
        QCOMPARE(stages(r), mixed);
        r = healthy;
        r["presented_frames"] = 750;
        r["enhancement_overwrites"] = 9;
        QCOMPARE(stages(r), QJsonArray{"processing"});
        r["enhancement_overwrites"] = 0;
        QCOMPARE(stages(r), QJsonArray{"playout"});
        r = healthy;
        r["canvas_coalesced_updates"] = 9;
        QCOMPARE(stages(r), QJsonArray{"paint"});
        r["canvas_visible"] = false;
        QVERIFY(stages(r).isEmpty());
        r = healthy;
        r["device_sent_top"] = 360;
        r["pairs"] = 360;
        r["presented_frames"] = 750;
        QCOMPARE(stages(r), QJsonArray{"sender"});
        r["capture_fps"] = 24;
        QCOMPARE(stages(r), QJsonArray{"rate"});
        r["device_status_us"] = 0; // Old device telemetry must never blame the sender.
        QCOMPARE(stages(r), QJsonArray{"input"});
        r["session"] = 24;
        QVERIFY(stages(r).isEmpty());
        QVERIFY(diagnoseFrameDrops(healthy, {}).isEmpty());
        r = healthy;
        r["source_interval_max_gap_ms"] = 151;
        QCOMPARE(stages(r), QJsonArray{"unknown"});
        r["interval_pair_gap_ms"] = 160;
        QCOMPARE(stages(r), QJsonArray{"input"});
        r["connection"] = "interrupted";
        r["last_pair_us"] = 9000000;
        r["last_packet_us"] = 12900000;
        QCOMPARE(stages(r), QJsonArray{"input"});
        r["last_packet_us"] = 9000000;
        QCOMPARE(stages(r), QJsonArray{"silence"});
        r["connection"] = "waiting";
        QVERIFY(stages(r).isEmpty());
    }
    void sourcePacingUsesRealFramesAndResetsAcrossSessions() {
        Timeline t(100);
        const qint64 origin = clockUs() - 1000000;
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::blue);
        auto put = [&](quint32 session, quint32 sequence, quint32 pts) {
            auto source = t.arrival(session, sequence, pts, origin + pts);
            t.put(source, image, {}, false, 30);
            auto choice = t.present();
            QCOMPARE(choice.source.sequence, sequence);
            QCOMPARE(choice.source.pts, qint64(pts));
            QVERIFY(!choice.synthetic);
        };
        t.reset(1);
        put(1, 0, 0);
        put(1, 2, 66666);
        put(1, 3, 99999);
        t.reset(2);
        put(2, 0, 0);
        t.setPictureMode("clean", true);
        put(2, 2, 66666);
        quint64 cursor = 0;
        QList<qint64> phase;
        for (const auto &event : t.diagnostics.since(cursor, clockUs()))
            if (QLatin1String(event.kind) == QLatin1String("select"))
                phase.append(event.target - origin - t.delay - event.source.pts);
        const QList<qint64> expected{0, -12000, -8000, 0, 0};
        QCOMPARE(phase, expected);
    }
    void rawPlayoutAcrossRepeatedCaptureStamps() {
        Timeline timeline(100);
        timeline.reset(7);
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::green);
        // The playhead is 95 ms past the first capture. The next real top
        // is already decoded, but its nominal timestamp is still 39 ms away.
        const qint64 origin = clockUs() - timeline.delay - 95000;
        auto put = [&](quint32 sequence, quint32 pts) {
            auto source = timeline.arrival(7, sequence, pts, origin + pts, 1);
            timeline.put(source, image, {}, false, 30);
            return source;
        };
        put(134, 0);
        const auto first = timeline.present();
        QCOMPARE(first.source.sequence, 134u);
        for (quint32 seq = 135; seq < 138; ++seq) {
            put(seq, 0);
            QCOMPARE(timeline.present().id, first.id);
        }
        const auto source = put(138, 133709);
        const auto next = timeline.present();
        QCOMPARE(next.source.sequence, 138u);
        QCOMPARE(next.updatedPanels, 1);
        QVERIFY(!next.synthetic);
        QCOMPARE(timeline.deadline(source), origin + timeline.delay + 133709);
    }
    void rawGapRecoveryRespectsBufferAndReturnsToClock() {
        for (const int delayMs : {20, 100}) {
            Timeline timeline(delayMs);
            timeline.reset(7);
            const auto origin = clockUs() - 3000000;
            QImage image(8, 8, QImage::Format_RGB888);
            image.fill(Qt::blue);
            QList<qint64> phase;
            quint64 cursor = 0;
            for (quint32 pts : {0u, 33333u, 167042u, 200375u, 233708u, 1267042u}) {
                auto source = timeline.arrival(7, pts, pts, origin + pts);
                timeline.put(source, image, {}, false, 30);
                QCOMPARE(timeline.present().source.pts, qint64(pts));
                for (const auto &event : timeline.diagnostics.since(cursor, clockUs()))
                    if (QLatin1String(event.kind) == QLatin1String("select"))
                        phase.append(event.target - timeline.deadline(source));
            }
            QCOMPARE(phase.size(), 6);
            QCOMPARE(phase[0], 0);
            QCOMPARE(phase[1], 0);
            if (delayMs == 100)
                QVERIFY(phase[2] >= -45000 && phase[2] <= -35000);
            else
                QCOMPARE(phase[2], -10000);
            QVERIFY(phase[2] < phase[3] && phase[3] < phase[4] && phase[4] < 0);
            QCOMPARE(phase[5], 0);
        }
    }
    void successiveCaptureGapsSharePlayoutBuffer() {
        Timeline timeline(100);
        timeline.reset(7);
        const auto origin = clockUs() - 1000000;
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::blue);
        QList<qint64> targets;
        quint64 cursor = 0;
        for (quint32 pts : {0u, 100282u, 217278u}) {
            auto source = timeline.arrival(7, pts, pts, origin + pts);
            timeline.put(source, image, {}, false, 30);
            QCOMPARE(timeline.present().source.pts, qint64(pts));
            for (const auto &event : timeline.diagnostics.since(cursor, clockUs()))
                if (QLatin1String(event.kind) == QLatin1String("select"))
                    targets.append(event.target);
        }
        QCOMPARE(targets.size(), 3);
        QVERIFY(targets[1] - targets[0] >= 80000);
        QVERIFY(targets[1] - targets[0] < 100000);
        QVERIFY(targets[2] - targets[1] < 100000);
    }
    void restoredPacingAcrossMissingFrames() {
        Timeline timeline(160);
        timeline.setPictureMode("fbcnn");
        timeline.reset(1);
        QImage image(8, 8, QImage::Format_RGB888);
        image.fill(Qt::green);
        const qint64 origin = clockUs() - 3000000;
        auto present = [&](quint32 session, quint32 seq, quint32 pts) {
            auto source = timeline.arrival(session, seq, pts, origin + pts);
            timeline.put(source, image, "fbcnn");
            const auto frame = timeline.present();
            QCOMPARE(frame.source.sequence, seq);
            QCOMPARE(frame.source.pts, qint64(pts));
            QCOMPARE(frame.image.cacheKey(), image.cacheKey());
            QVERIFY(!frame.synthetic);
        };
        present(1, 0, 0);
        present(1, 1, 33333);
        present(1, 5, 166665); // Three missing 30 Hz frames.
        present(1, 6, 199998);
        present(1, 49, 1633317); // Beyond the retained playback history.
        timeline.reset(2);
        present(2, 0, 0);

        QList<qint64> phase;
        quint64 cursor = 0;
        for (const auto &event : timeline.diagnostics.since(cursor, clockUs()))
            if (QLatin1String(event.kind) == QLatin1String("select"))
                phase.append(event.target - origin - timeline.delay - event.source.pts);
        QCOMPARE(phase.size(), 6);
        QCOMPARE(phase[0], 0);
        QCOMPARE(phase[1], 0);
        QCOMPARE(phase[2], -15500);
        QVERIFY(phase[3] > phase[2] && phase[3] < 0);
        QCOMPARE(phase[4], 0);
        QCOMPARE(phase[5], 0);
    }
    void lateWakePreservesUnshownSourceOverMidpoint() {
        Timeline t(30);
        t.setPictureMode("fbcnn", true);
        t.reset(1);
        QImage im(20, 20, QImage::Format_RGB888);
        im.fill(Qt::green);
        auto a = t.arrival(1, 5, 0, clockUs());
        t.put(a, im, "fbcnn");
        auto mid = a;
        mid.pts += 16667;
        t.put(mid, im, "fbcnn+flow", true);
        QTest::qWait(60);
        auto chosen = t.present();
        QVERIFY(!chosen.synthetic);
        QCOMPARE(chosen.quality, QString("fbcnn"));
        QCOMPARE(t.present().id, chosen.id);
    }
    void precisionSwitchKeepsQueuedRestoredFrames() {
        Timeline t(20);
        t.setPictureMode("fbcnn", true);
        t.reset(3);
        QImage im(20, 20, QImage::Format_RGB888);
        im.fill(Qt::green);
        const auto wall = clockUs();
        const auto first = t.arrival(3, 1, 0, wall);
        const auto second = t.arrival(3, 2, 33333, wall + 33333);
        t.put(first, im, "fbcnn");
        t.put(second, im, "fbcnn");
        auto midpoint = second;
        midpoint.pts += 16666;
        t.put(midpoint, im, "fbcnn+rife", true);
        t.clearSynthetic();
        QTest::qWait(55);
        const auto selected = t.present();
        QCOMPARE(selected.source.sequence, second.sequence);
        QCOMPARE(selected.quality, QString("fbcnn"));
        QVERIFY(!selected.synthetic);
        QTest::qWait(30);
        QCOMPARE(t.present().id, selected.id);
    }
    void upperDoesNotWaitForLowerAndLateLowerIsARevision() {
        Timeline t(0);
        t.setPictureMode("fbcnn");
        t.reset(8);
        const auto zero = clockUs() - 80000;
        QImage upper(400,480,QImage::Format_RGB888), both(upper.size(),upper.format());
        upper.fill(Qt::red);
        both.fill(Qt::green);
        auto top = t.arrival(8, 5, 30000, zero, 1);
        t.put(top, upper, "fbcnn");
        const auto first = t.present();
        QCOMPARE(first.updatedPanels, 1);
        QVERIFY(!first.source.panels[1]);
        auto lower = t.arrival(8, 5, 30000, zero + 1000, 2, 29000);
        auto revised = top;
        revised.panels[1] = lower.panels[1];
        t.put(revised, both, "fbcnn");
        const auto second = t.present();
        QCOMPARE(second.updatedPanels, 2);
        QCOMPARE(second.source.sequence, first.source.sequence);
        QVERIFY(second.id > first.id);
        QCOMPARE(t.recorded(first.time).id, first.id);
        QCOMPARE(t.recorded(second.time).image.cacheKey(), both.cacheKey());
        t.put(top, upper, "fbcnn");
        QCOMPARE(t.present().id, second.id);
        auto next = t.arrival(8, 6, 63333, zero + 33333, 1);
        next.panels[1] = revised.panels[1];
        t.put(next, both, "fbcnn");
        QCOMPARE(t.present().updatedPanels, 1);
        auto newerTop = t.present();
        auto outdated = revised;
        outdated.panels[1] = PanelStamp{4, 22000};
        t.put(outdated, upper, "fbcnn");
        QCOMPARE(t.present().id, newerTop.id);
        // A new lower capture can arrive with an already superseded upper.
        outdated.panels[1] = PanelStamp{7, 70000};
        t.put(outdated, upper, "fbcnn");
        auto late = t.present();
        QCOMPARE(late.updatedPanels, 2);
        QCOMPARE(late.source.sequence, newerTop.source.sequence);
        QCOMPARE(late.image.pixelColor(100,40), QColor(Qt::green));
        QCOMPARE(late.image.pixelColor(100,300), QColor(Qt::red));
    }
    void independentLowerUpdateHoldsTheLastGeneratedUpper() {
        Timeline t(100);
        t.reset(2);
        QImage first(400,480,QImage::Format_RGB888), blended(first.size(),first.format());
        first.fill(Qt::red);
        blended.fill(Qt::green);
        auto real = t.arrival(2, 1, 100000, clockUs() - 110000);
        t.put(real, first);
        QCOMPARE(t.present().updatedPanels, 3);
        auto midpoint = real;
        midpoint.pts += 70000;
        t.put(midpoint, blended, "flow", true, 0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(t.present().generatedPanels, 1, 220);
        auto updated = real;
        updated.panels[1] = PanelStamp{3, midpoint.pts + 1000};
        t.put(updated, first);
        QTRY_COMPARE_WITH_TIMEOUT(t.present().updatedPanels, 2, 200);
        const auto shown = t.present();
        QCOMPARE(shown.generatedPanels, 0);
        QCOMPARE(shown.image.pixelColor(100,80), QColor(Qt::green));
        QCOMPARE(shown.image.pixelColor(100,300), QColor(Qt::red));
    }
    void realCanvasWinsACollidingSyntheticTimestamp() {
        for (bool synthFirst : {false, true}) {
            Timeline t(50);
            t.reset(11);
            QImage real(400,480,QImage::Format_RGB888), generated(real.size(),real.format());
            real.fill(Qt::red);
            generated.fill(Qt::green);
            auto src = t.arrival(11, 4, 200000, clockUs());
            if (synthFirst) t.put(src, generated, "flow", true);
            t.put(src, real);
            if (!synthFirst) t.put(src, generated, "flow", true);
            QTRY_VERIFY_WITH_TIMEOUT(!t.present().image.isNull(), 200);
            const auto shown = t.present();
            QCOMPARE(shown.updatedPanels, 3);
            QCOMPARE(shown.generatedPanels, 0);
            QCOMPARE(shown.image.pixelColor(100,100), QColor(Qt::red));
        }
    }
    void selectionIsImmutable() {
        Timeline t(5);
        QImage raw(400, 480, QImage::Format_RGB888), enhanced(raw.size(), raw.format());
        raw.fill(Qt::red);
        enhanced.fill(Qt::green);
        t.reset(1);
        auto src = t.arrival(1, 99, 0, clockUs());
        t.put(src, raw);
        QTest::qWait(15);
        auto first = t.present();
        QCOMPARE(first.source.sequence, 99u);
        QCOMPARE(first.image.cacheKey(), raw.cacheKey());
        t.put(src, enhanced, "fbcnn");
        QCOMPARE(t.present().id, first.id);
        auto next = t.arrival(1, 100, 100000, clockUs());
        t.put(next, raw);
        t.put(next, enhanced, "fbcnn");
        QTest::qWait(15);
        auto second = t.present();
        QCOMPARE(second.quality, QString("fbcnn"));
        QCOMPARE(t.recorded(second.time - 1).id, first.id);
        QCOMPARE(t.recorded(second.time).image.cacheKey(), enhanced.cacheKey());
        t.setPictureMode("off");
        t.reset(2);
        QCOMPARE(t.present().id, second.id);
        QCOMPARE(t.pcm(clockUs(), 480), QByteArray(480 * 4, 0));
        auto restart = t.arrival(2, 0, 0, clockUs());
        t.put(restart, raw);
        QTest::qWait(15);
        auto third = t.present();
        QCOMPARE(third.source.session, 2u);
        QVERIFY(third.id > second.id);
        bool late = false;
        for (auto e : t.diagnostics.window(clockUs()))
            late |= e.toObject()["event"].toString() == "late";
        QVERIFY(late);
    }
    void firstEnhancedFrameTakesOverWithoutClearingThePreview() {
        Timeline timeline(10);
        timeline.reset(9);
        QImage picture(400, 480, QImage::Format_RGB888);
        picture.fill(Qt::blue);
        const qint64 origin = clockUs() - 500000;
        auto first = timeline.arrival(9, 1, 0, origin);
        timeline.put(first, picture);
        const auto raw = timeline.present();
        QCOMPARE(raw.quality, QString("original"));
        timeline.setPictureMode("fbcnn", false, true);
        auto second = timeline.arrival(9, 2, 33000, origin + 33000);
        timeline.put(second, picture);
        QCOMPARE(timeline.present().id, raw.id);
        timeline.put(second, picture, "fbcnn");
        QCOMPARE(timeline.present().quality, QString("fbcnn"));
        auto third = timeline.arrival(9, 3, 66000, origin + 66000);
        timeline.put(third, picture);
        QCOMPARE(timeline.present().source.sequence, 2u);
    }
    void fbcnnNeverShowsRawEvenAfterDeadline() {
        Timeline timeline(5);
        timeline.reset(9);
        QImage picture(400, 480, QImage::Format_RGB888);
        picture.fill(Qt::blue);
        auto early = timeline.arrival(9, 0, 1000, clockUs());
        timeline.put(early, picture);
        QTest::qWait(10);
        QCOMPARE(timeline.present().quality, QString("original"));
        timeline.setPictureMode("fbcnn");
        auto blank = timeline.present();
        QVERIFY(blank.image.isNull());
        QCOMPARE(blank.quality, QString("waiting_fbcnn"));
        auto source = timeline.arrival(9, 1, 21000, clockUs());
        timeline.put(source, picture);
        QTest::qWait(15);
        QCOMPARE(timeline.present().id, blank.id);
        timeline.put(source, picture, "fbcnn");
        auto restored = timeline.present();
        QCOMPARE(restored.quality, QString("fbcnn"));
        QCOMPARE(restored.source.sequence, 1u);
        auto lateMid = source;
        lateMid.pts += 1000;
        timeline.put(lateMid, picture, "fbcnn+flow", true);
        QCOMPARE(timeline.present().id, restored.id);
        auto slow = timeline.arrival(9, 2, 41000, clockUs());
        timeline.put(slow, picture);
        QTest::qWait(20);
        QCOMPARE(timeline.present().id, restored.id);
        QCOMPARE(timeline.recorded(clockUs()).id, restored.id);
        timeline.reset(10);
        QCOMPARE(timeline.present().quality, QString("fbcnn"));
        auto next = timeline.arrival(10, 0, 0, clockUs());
        timeline.put(next, picture);
        QTest::qWait(10);
        QCOMPARE(timeline.present().id, restored.id);
        timeline.put(next, picture, "fbcnn");
        QCOMPARE(timeline.present().source.session, 10u);
        timeline.setPictureMode("off");
        auto last = timeline.arrival(10, 1, 10000, clockUs());
        timeline.put(last, picture);
        QTest::qWait(15);
        QCOMPARE(timeline.present().quality, QString("original"));
    }
    void switchingDetailModesWaitsForMatchingFrames() {
        Timeline timeline(5);
        timeline.reset(9);
        QImage picture(400, 480, QImage::Format_RGB888);
        picture.fill(Qt::blue);
        auto source = timeline.arrival(9, 0, 0, clockUs());
        QTest::qWait(10);
        for (const auto &mode : QStringList{"fbcnn", "fbcnn-detail", "fbcnn-sr", "fbcnn"}) {
            timeline.setPictureMode(mode);
            timeline.put(source, picture);
            const QString other = mode == "fbcnn" ? "fbcnn-detail" : "fbcnn";
            timeline.put(source, picture, other);
            QVERIFY(timeline.present().image.isNull());
            timeline.put(source, picture, mode);
            QCOMPARE(timeline.present().quality, mode);
        }
    }
    void saveFailuresPreserveDestination() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("原始文件.mp4");
        const QByteArray sentinel = "keep existing bytes intact";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(sentinel), sentinel.size());
        file.close();
        auto verifyOriginal = [&] {
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), sentinel);
            file.close();
            QCOMPARE(QDir(dir.path()).entryList({".3ds-live-*"}, QDir::Files | QDir::Hidden).size(), 0);
        };
        simulateFullDisk = true;
        verifyOutput(path, false);
        simulateFullDisk = false;
        verifyOriginal();
        auto wide = reinterpret_cast<const wchar_t *>(path.utf16());
        HANDLE lock = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);
        verifyOutput(path, false);
        CloseHandle(lock);
        verifyOriginal();
        QVERIFY(SetFileAttributesW(wide, FILE_ATTRIBUTE_READONLY));
        verifyOutput(path, false);
        QVERIFY(SetFileAttributesW(wide, FILE_ATTRIBUTE_NORMAL));
        verifyOriginal();
        const auto readOnly = dir.filePath("readonly");
        QVERIFY(QDir().mkdir(readOnly));
        const auto aclTool = QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/icacls.exe");
        // WD on a directory controls FILE_ADD_FILE. Restrict only the test's private directory.
        QCOMPARE(QProcess::execute(aclTool, {QDir::toNativeSeparators(readOnly), "/deny", "*S-1-1-0:(WD)"}), 0);
        verifyOutput(QDir(readOnly).filePath("cannot-create.mp4"), false);
        QCOMPARE(QProcess::execute(aclTool, {QDir::toNativeSeparators(readOnly), "/remove:d", "*S-1-1-0"}), 0);
        verifyOriginal();
        verifyOutput(path, true);
        QVERIFY(file.size() > sentinel.size());
    }
};
QTEST_GUILESS_MAIN(MediaTests)
#include "test_media.moc"
