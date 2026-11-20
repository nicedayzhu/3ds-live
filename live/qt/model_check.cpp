#include "model_check.h"
#include "imaging.h"
#include "timeline.h"
#include <QtCore>
int checkModels(QString directory, QString file, QString modes, QString rifePrecision) {
    QJsonArray results;
    bool passed = true;
    try {
        rifePrecision = live::resolveRifePrecision(std::move(rifePrecision));
        live::Imaging ai(directory, rifePrecision);
        for (auto mode : modes.split(','))
            for (int w : {400, 320}) {
                QImage input(w, 240, QImage::Format_RGB888);
                for (int y = 0; y < 240; ++y)
                    for (int x = 0; x < w; ++x) {
                        uchar *p = input.scanLine(y) + x * 3;
                        p[0] = 30 + x * 180 / w + y * 35 / 240;
                        p[1] = 40 + ((x / 12 + y / 12) % 2) * 80 + y * 70 / 240;
                        p[2] = 25 + (x * 7 + y * 11) % 161;
                        if (x >= 30 && x < 75 && y >= 50 && y < 90)
                            memset(p, 0, 3);
                        if (x >= 80 && x < 155 && y >= 110 && y < 135)
                            memset(p, 255, 3);
                    }
                QImage shifted = input.copy();
                for (int y = 0; y < input.height(); ++y)
                    memcpy(shifted.scanLine(y) + 3, input.constScanLine(y), (w - 1) * 3);
                auto process = [&] {
                    if (live::usesFbcnn(mode)) {
                        const int index = w == 400 ? 0 : 1;
                        live::Images changed, restored;
                        changed[index] = input;
                        ai.restorePair(changed, restored, mode);
                        return restored[index];
                    }
                    return mode == "rife" || mode == "flow" ? ai.midpoint(input, shifted, mode)
                                                            : ai.restore(input, mode);
                };
                process();
                auto start = live::clockUs();
                QImage output = process();
                QJsonObject row{{"mode", mode}, {"width", w}, {"ms", (live::clockUs() - start) / 1000.}};
                row["output_width"] = output.width();
                row["output_height"] = output.height();
                passed &= output.size() == input.size() * (live::usesSuperResolution(mode) ? 2 : 1);
                if (mode == "rife" || mode == "flow")
                    passed &= (output.cacheKey() != input.cacheKey() && output.size() == input.size());
                if (live::usesFbcnn(mode)) {
                    row["backend"] = ai.fbcnnBackend();
                    row["backend_fallback"] = ai.fbcnnFallback();
                } else if (mode == "rife") {
                    row["rife_precision"] = rifePrecision;
                    row["backend"] = ai.rifeBackend();
                    row["backend_fallback"] = ai.rifeFallback();
                }
                if (mode == "fbcnn") {
                    QImage ref(QDir(directory).filePath(QString("fbcnn-reference-%1.png").arg(w)));
                    ref = ref.convertToFormat(QImage::Format_RGB888);
                    if (ref.size() != output.size())
                        throw std::runtime_error("Missing reference image");
                    qint64 sum = 0;
                    int max = 0;
                    for (int y = 0; y < 240; ++y)
                        for (int i = 0; i < w * 3; ++i) {
                            int d = std::abs(int(output.constScanLine(y)[i]) - ref.constScanLine(y)[i]);
                            sum += d;
                            max = std::max(max, d);
                        }
                    double mean = double(sum) / (w * 240 * 3);
                    row["mean_rgb_delta"] = mean;
                    row["max_rgb_delta"] = max;
                    passed &= (mean <= .5 && max <= 8);
                }
                results.append(row);
            }
    } catch (const std::exception &e) {
        results.append(QString::fromUtf8(e.what()));
        passed = false;
    }
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly))
        return 2;
    f.write(QJsonDocument(QJsonObject{{"passed", passed}, {"screens", results}}).toJson());
    return passed ? 0 : 1;
}
