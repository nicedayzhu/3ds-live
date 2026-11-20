#include "imaging.h"
#include "native_detail.h"
#include <QtCore>
#include <QColor>
#include <QDebug>
using namespace live;

static bool checkLayout() {
    Images screens{QImage(400, 240, QImage::Format_RGB888), QImage(320, 240, QImage::Format_RGB888)};
    screens[0].fill(Qt::red);
    screens[1].fill(Qt::green);
    for (int scale : {1, 2}) {
        const auto canvas = compose(screens, scale);
        if (canvas.size() != QSize(400, 480) * scale ||
            canvas.pixelColor(0, 240 * scale - 1) != QColor(Qt::red) ||
            canvas.pixelColor(40 * scale - 1, 240 * scale) != QColor(Qt::black) ||
            canvas.pixelColor(40 * scale, 240 * scale) != QColor(Qt::green) ||
            canvas.pixelColor(360 * scale - 1, 480 * scale - 1) != QColor(Qt::green) ||
            canvas.pixelColor(360 * scale, 480 * scale - 1) != QColor(Qt::black))
            return false;
    }
    return true;
}

static bool checkJpegDetail() {
    QImage textEdges(320, 240, QImage::Format_RGB888);
    for (int y = 0; y < textEdges.height(); ++y) {
        auto *p = textEdges.scanLine(y);
        for (int x = 0; x < textEdges.width(); ++x) {
            const int values[] = {32, 35, 48, 215, 232, 233, 230, 215, 48, 35};
            p[3*x] = p[3*x+1] = p[3*x+2] = uchar(values[x % 10]);
        }
    }
    const auto normal = nativeDetail(textEdges);
    return normal != textEdges && nativeDetail(textEdges, 36) == normal &&
           nativeDetail(textEdges, 0) == normal && nativeDetail(textEdges, 14) == textEdges;
}

// Optional image-quality measurements use the production receiver's codecs and model.
// Pass an input folder of <name>-400.jpg / <name>-320.jpg and a fresh output folder.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    configureOpenCV();
    if (!checkLayout() || !checkJpegDetail())
        return 1;
    if (argc == 1)
        return 0;
    const auto args = app.arguments();
    int quality = 0;
    if (args.size() == 5 && args[3] == "--jpeg-quality") {
        quality = args[4].toInt();
        if (quality < 1 || quality > 95)
            return 2;
    } else if (argc != 3) {
        return 2;
    }
    const std::array<int, 2> jpegQuality{quality, quality};
    const QDir source(app.arguments()[1]), output(app.arguments()[2]);
    if (output.exists() || !QDir().mkpath(output.absolutePath()))
        return 2;
    const auto pairs = source.entryList({"*-400.jpg"}, QDir::Files);
    if (pairs.isEmpty())
        return 2;
    QFile timing(output.filePath("timings.csv"));
    if (!timing.open(QIODevice::WriteOnly | QIODevice::Text)) return 2;
    QTextStream rows(&timing);
    rows << "sample,mode,backend,pair_ms\n";
    Imaging imaging(QCoreApplication::applicationDirPath() + "/models");
    try {
        QSet<QString> warmed;
        for (const auto &name : pairs) {
            const QString stem = name.chopped(8);
            Images raw, restored;
            for (int i = 0; i < 2; ++i) {
                const int width = i ? 320 : 400;
                QFile file(source.filePath(stem + QString("-%1.jpg").arg(width)));
                if (!file.open(QIODevice::ReadOnly))
                    return 3;
                raw[i] = decode(file.readAll(), width);
                if (raw[i].isNull())
                    return 3;
            }
            if (!compose(raw, 1).save(output.filePath(stem + "-raw.png"))) return 4;
            for (const QString &mode : QStringList{"fbcnn", "fbcnn-detail", "fbcnn-sr"}) {
                if (!warmed.contains(mode)) {
                    imaging.restorePair(raw, restored, mode, nullptr, nullptr, jpegQuality);
                    warmed.insert(mode);
                }
                for (int repeat = 0; repeat < 5; ++repeat) {
                    QElapsedTimer timer;
                    timer.start();
                    imaging.restorePair(raw, restored, mode, nullptr, nullptr, jpegQuality);
                    const double ms = timer.nsecsElapsed() / 1e6;
                    rows << stem << ',' << mode << ',' << imaging.fbcnnBackend() << ',' << ms << '\n';
                }
                const int scale = usesSuperResolution(mode) ? 2 : 1;
                for (int i = 0; i < 2; ++i)
                    if (restored[i].size() != raw[i].size() * scale) return 5;
                if (!compose(restored, scale).save(output.filePath(stem + "-" + mode + ".png"))) return 4;
                if (mode == "fbcnn" && !compose(restored, 2).save(output.filePath(stem + "-fbcnn-2x.png")))
                    return 4;
                if (name == pairs.first()) {
                    const auto previous = restored[1].cacheKey();
                    imaging.restorePair({raw[0], QImage{}}, restored, mode, nullptr, nullptr, jpegQuality);
                    if (restored[1].cacheKey() != previous) return 5;
                }
            }
        }
        qInfo() << pairs.size() << "pairs" << imaging.fbcnnBackend() << imaging.fbcnnFallback();
    } catch (const std::exception &e) {
        qCritical() << e.what();
        return 5;
    }
    return 0;
}
