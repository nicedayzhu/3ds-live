#include "imaging.h"
#include "pipeline.h"
#include <QtCore>
#include <QPainter>
using namespace live;

static QImage scene(int displacement, bool obstruction) {
    QImage im(400, 240, QImage::Format_RGB888);
    im.fill(QColor(48, 55, 64));
    QPainter p(&im);
    for (int x = -32; x < 430; x += 21)
        p.fillRect(x + displacement, 73, 5, 146, QColor(188, 186, 172));
    p.fillRect(27, 12, 50, 9, Qt::white);
    if (obstruction) p.fillRect(160, 94, 83, 67, QColor(201, 21, 34));
    return im;
}

// Optional hardware test: test_flow --rife-switch TensorRT|DirectML, staged next to the models.
static int checkPrecisionSwitch(const QString &backend) {
    if (backend != "DirectML" && backend != "TensorRT") return 2;
    const QString mode = backend == "TensorRT" ? "fbcnn" : "fast-detail";
    QVector<Screens> inputs;
    for (int step = 0; step < 20; ++step) {
        Screens encoded;
        for (int i = 0; i < 2; ++i) {
            QBuffer buffer(&encoded[i]);
            if (!buffer.open(QIODevice::WriteOnly) ||
                !scene(i ? 19-step : step, false).copy(0, 0, i ? 320 : 400, 240).save(&buffer, "JPEG", 80))
                return 3;
        }
        inputs.append(encoded);
    }
    Timeline timeline(160);
    timeline.reset(1);
    Pipeline pipeline(timeline, QCoreApplication::applicationDirPath() + "/models", 1, mode, "rife", "fp32");
    const auto start = clockUs();
    quint32 sequence = 0;
    qint64 lastId = 0, lastRealAt = 0;
    Images previous;
    for (const QString &precision : QStringList{"fp32", "fp16", "fp32"}) {
        const bool switching = sequence != 0;
        const quint32 first = sequence;
        if (switching) pipeline.configure(mode, "rife", 1, precision);
        const auto phaseStart = clockUs();
        const QString expected = backend == "DirectML" && precision == "fp32" ? backend : backend + " " + precision.toUpper();
        qint64 maxGap = 0, firstSyntheticAt = 0;
        int real = 0, generated = 0, realBeforeGeneration = 0;
        std::array<int, 2> changes{};
        QJsonObject stats;
        while (clockUs() - phaseStart < 30000000) {
            const auto now = clockUs();
            const quint32 pts = quint64(sequence) * 1000000 / 30;
            if (now >= start + pts) {
                const auto source = timeline.arrival(1, sequence, pts, now);
                pipeline.submit({1, sequence, pts, inputs[sequence % inputs.size()], 30}, source);
                ++sequence;
                stats = pipeline.stats();
                if (!stats["model_error"].toString().isEmpty() || !stats["motion_error"].toString().isEmpty()) break;
            }
            const auto frame = timeline.present();
            if (!frame.image.isNull() && frame.id != lastId) {
                lastId = frame.id;
                if (!frame.synthetic) {
                    if (lastRealAt) maxGap = std::max(maxGap, frame.time - lastRealAt);
                    lastRealAt = frame.time;
                }
                if (frame.source.sequence >= first && frame.synthetic && stats["motion_backend"] == expected) {
                    if (!firstSyntheticAt) firstSyntheticAt = frame.time;
                    ++generated;
                }
                if (frame.source.sequence >= first && !frame.synthetic) {
                    if (frame.image.size() != QSize(400,480) ||
                        (frame.quality != mode && (switching || firstSyntheticAt || frame.quality != "original"))) {
                        qCritical() << "Unexpected source frame" << frame.image.size() << frame.quality;
                        return 4;
                    }
                    const Images screens{frame.image.copy(0,0,400,240), frame.image.copy(40,240,320,240)};
                    for (int i = 0; i < 2; ++i) changes[i] += !previous[i].isNull() && previous[i] != screens[i];
                    previous = screens;
                    ++real;
                    if (!firstSyntheticAt) ++realBeforeGeneration;
                }
            }
            if (real >= 30 && generated >= 30 && changes[0] >= 25 && changes[1] >= 25) break;
            QThread::msleep(1);
        }
        qInfo() << "precision" << precision << "backend" << stats["motion_backend"].toString()
                << "real" << real << "generated" << generated << "top/bottom changes" << changes[0] << changes[1]
                << "max real gap ms" << maxGap / 1000. << "first midpoint ms"
                << (firstSyntheticAt ? (firstSyntheticAt - phaseStart) / 1000. : -1.) << "real before" << realBeforeGeneration;
        if (stats["motion_backend"] != expected || stats["rife_precision"] != precision ||
            real < 30 || generated < 30 || changes[0] < 25 || changes[1] < 25 || (switching && maxGap > 100000)) {
            qCritical() << stats;
            return 5;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    configureOpenCV();
    if (argc == 3 && app.arguments()[1] == "--rife-switch") return checkPrecisionSwitch(app.arguments()[2]);
    Imaging ai(app.applicationDirPath() + "/models");
    if (argc == 1) {
        const auto a = scene(0, false);
        bool rejected = false;
        const auto clean = ai.midpoint(a, scene(4, false), "flow", true, &rejected);
        if (rejected || clean.cacheKey() == a.cacheKey() || clean.pixelColor(40, 15) != Qt::white) return 1;
        const auto phases = ai.flowBetween(a, scene(4, false), 4, true, &rejected);
        if (phases.size() != 3 || phases[0] == phases[1] || phases[1] == phases[2] ||
            phases[0].pixelColor(40, 15) != Qt::white || phases[2].pixelColor(40, 15) != Qt::white) {
            qCritical() << "Quarter-step motion" << phases.size() << "guarded" << rejected;
            return 21;
        }
        QImage cyan(a.size(), a.format());
        cyan.fill(Qt::cyan);
        if (!ai.flowBetween(a, cyan, 4, false, &rejected).isEmpty() || !rejected) return 22;
        const auto occluded = ai.midpoint(a, scene(4, true), "flow", true, &rejected);
        // The foreground is saturated red and the background neutral. A
        // half-exposed red copy creates intermediate red-green differences.
        int transparent = 0;
        for (int y = 102; y < 151; ++y)
            for (int x = 171; x < 231; ++x) {
                auto pixel = occluded.pixelColor(x,y);
                int red = pixel.red()-pixel.green();
                transparent += red > 40 && red < 145;
            }
        if (transparent > 60) return 2;
        auto cut = a.copy();
        cut.fill(Qt::white);
        const auto held = ai.midpoint(a, cut, "flow", true, &rejected);
        if (!rejected || held.cacheKey() != a.cacheKey()) return 3;
        const Images first{a, a.copy(0,0,320,240)}, second{cut, first[1]};
        std::array<bool, 2> cuts{};
        const auto skipped = ai.rifeMidpoints(first, second, &cuts);
        if (!cuts[0] || cuts[1] || skipped[0].cacheKey() != a.cacheKey() ||
            skipped[1].cacheKey() != first[1].cacheKey()) return 4;
        return 0;
    }
    // A sequence-numbered capture lets us compare a generated midpoint with
    // the actual middle source frame after the exact same FBCNN restoration.
    if (argc != 3 && argc != 4) return 3;
    const auto method = app.arguments().value(3, "flow");
    const bool paired = method == "rife-pair";
    if (!paired && method != "flow" && method != "rife") return 3;
    const QDir input(app.arguments()[1]), output(app.arguments()[2]);
    if (output.exists() || !QDir().mkpath(output.absolutePath())) return 3;
    QFile log(output.filePath("flow.csv"));
    if (!log.open(QFile::WriteOnly | QFile::Text)) return 3;
    QTextStream csv(&log);
    csv << "middle,width,protected,ms,method,pair_ms,backend\n";
    QMap<qint64,QString> names;
    for (const auto &f : input.entryList({"*-400.jpg"}, QDir::Files)) {
        bool ok = false;
        auto n = f.chopped(8).toLongLong(&ok);
        if (ok) names[n] = f.chopped(8);
    }
    Images previous, middle;
    DeviceImages previousDevice, middleDevice;
    const bool verifyDevice = paired && qEnvironmentVariableIsSet("THREE_DS_LIVE_TEST_DEVICE_RGB");
    int verifiedScreens = 0;
    int triples = 0;
    for (auto it = names.begin(); it != names.end(); ++it) {
        Images raw, restored;
        DeviceImages restoredDevice;
        for (int i = 0; i < 2; ++i) {
            QFile f(input.filePath(it.value() + (i ? "-320.jpg" : "-400.jpg")));
            if (!f.open(QFile::ReadOnly)) return 4;
            raw[i] = decode(f.readAll(), i ? 320 : 400);
            if (raw[i].isNull()) return 4;
        }
        ai.restorePair(raw, restored, "fbcnn", nullptr, paired ? &restoredDevice : nullptr);
        if (verifyDevice) {
            for (int i = 0; i < 2; ++i) {
                const auto mismatches = ai.deviceRgbMismatches(restoredDevice[i], restored[i]);
                if (mismatches != 0) {
                    qCritical() << "FBCNN device reference mismatch" << it.key() << i << mismatches;
                    return 7;
                }
                ++verifiedScreens;
            }
        }
        const bool consecutive = names.contains(it.key()-1) && names.contains(it.key()-2);
        std::array<bool, 2> cuts{};
        Images predictions;
        double pairMs = 0;
        if (paired && consecutive) {
            QElapsedTimer timer;
            timer.start();
            predictions = ai.rifeMidpoints(previous, restored, &cuts, previousDevice, restoredDevice);
            pairMs = timer.nsecsElapsed()/1e6;
        }
        for (int i = 0; i < 2; ++i) {
            const int width = raw[i].width();
            if (!restored[i].save(output.filePath(QString("%1-%2.png").arg(it.key()).arg(width)))) return 5;
            if (!consecutive) continue;
            csv << it.key()-1 << ',' << width << ',';
            if (paired) {
                csv << int(cuts[i]) << ",,rife," << pairMs << ',' << ai.rifeBackend() << '\n';
            } else {
                QElapsedTimer timer;
                timer.start();
                predictions[i] = ai.midpoint(previous[i], restored[i], method, true, &cuts[i]);
                csv << int(cuts[i]) << ',' << timer.nsecsElapsed()/1e6 << ',' << method << ",,"
                    << (method == "rife" ? ai.rifeBackend() : "CPU") << '\n';
            }
            if (!predictions[i].save(output.filePath(QString("%1-%2-mid.png").arg(it.key()-1).arg(width)))) return 5;
            ++triples;
        }
        previous = middle;
        middle = restored;
        previousDevice = std::move(middleDevice);
        middleDevice = std::move(restoredDevice);
    }
    if (paired) qInfo() << "RIFE backend" << ai.rifeBackend() << "device inputs" << ai.rifeDeviceInputs()
                        << "FBCNN device screens verified" << verifiedScreens;
    if (verifyDevice && ai.rifeDeviceInputs() == 0) return 7;
    return triples ? 0 : 6;
}
