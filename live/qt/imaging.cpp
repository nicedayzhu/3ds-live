#include "imaging.h"
#include "runtime_paths.h"
#include "native_detail.h"
#ifdef LIVE_TENSORRT
#include "image_trt.h"
#endif
#include <QtCore>
#include <QImageReader>
#include <QPainter>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include "live_jpeg_wire.h"
extern "C" {
#include "live_codec.h"
}
#include <QtEndian>
#include <map>
#include <mutex>
namespace live {
QString resolveRifePrecision(QString precision) {
    if (precision.isEmpty())
        precision = qEnvironmentVariable("THREE_DS_LIVE_RIFE_PRECISION");
    precision = precision.trimmed().toLower();
    return precision == "fp16" || precision == "directml" ? precision : QStringLiteral("fp32");
}
void configureOpenCV() {
    // Two small panels share the PC with the GPU submission, audio and GUI threads.
    cv::setNumThreads(std::min(2, cv::getNumberOfCPUs()));
}
QImage decode(const QByteArray &wire, int w) {
    if (wire.size() < 12)
        return {};
    if (wire.startsWith("LTC1")) {
        if (qFromLittleEndian<quint16>(wire.data() + 4) != w)
            return {};
        QImage img(w, 240, QImage::Format_RGB888);
        return live_decode(reinterpret_cast<const uchar *>(wire.data()), wire.size(), img.bits(),
                           img.sizeInBytes()) > 0
                   ? img
                   : QImage();
    }
    QByteArray jpeg = wire;
    const bool compact = wire.startsWith("R8J1");
    if (compact) {
        int q = uchar(wire[5]);
        if (wire[4] != LIVE_JPEG_WIRE_LAYOUT || q < 1 || q > 95 ||
            qFromLittleEndian<quint16>(wire.data() + 6) != w || !wire.endsWith(QByteArray::fromHex("ffd9")))
            return {};
        jpeg = QByteArray(reinterpret_cast<const char *>(live_jpeg_wire_header_q50_400),
                          LIVE_JPEG_FULL_HEADER_SIZE);
        int factor = q < 50 ? 5000 / q : 200 - 2 * q;
        for (int at : {25, 94})
            for (int i = 0; i < 64; ++i)
                jpeg[at + i] = char(std::clamp((uchar(jpeg[at + i]) * factor + 50) / 100, 1, 255));
        jpeg[163] = char(w / 16);
        jpeg[171] = char(w >> 8);
        jpeg[172] = char(w & 255);
        jpeg.append(wire.constData() + 8, wire.size() - 8);
    }
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "jpeg");
    if (reader.size() != QSize(w, 240))
        return {};
    // R12 frames use our fixed baseline JPEG header. Decode straight into the
    // final RGB888 buffer, preserving Qt metadata handling for other JPEGs.
    const bool handheldJpeg = compact || (jpeg.size() >= LIVE_JPEG_FULL_HEADER_SIZE &&
        !memcmp(jpeg.constData(), live_jpeg_wire_header_q50_400, 25) &&
        !memcmp(jpeg.constData()+173, live_jpeg_wire_header_q50_400+173, LIVE_JPEG_FULL_HEADER_SIZE-173));
    if (handheldJpeg) {
        try {
            QImage result(w, 240, QImage::Format_RGB888);
            cv::Mat dest(240,w,CV_8UC3,result.bits(),result.bytesPerLine());
            cv::Mat data(1,int(jpeg.size()),CV_8UC1,const_cast<char *>(jpeg.constData()));
            auto decoded = cv::imdecode(data, cv::IMREAD_COLOR_RGB | cv::IMREAD_IGNORE_ORIENTATION, &dest);
            if (decoded.data == result.constBits()) return result;
        } catch (const cv::Exception &) {
            // The Qt decoder can still handle inputs rejected by this backend.
        }
    }
    return reader.read().convertToFormat(QImage::Format_RGB888);
}
QImage compose(const Images &screens, int scale) {
    QImage canvas(400 * scale, 480 * scale, QImage::Format_RGB888);
    if (scale == 1 && screens[0].format() == canvas.format() && screens[1].format() == canvas.format() &&
        screens[0].size() == QSize(400, 240) && screens[1].size() == QSize(320, 240)) {
        const int stride = int(canvas.bytesPerLine());
        uchar *dst = canvas.bits();
        for (int y = 0; y < 240; ++y) {
            memcpy(dst, screens[0].constScanLine(y), 1200);
            auto lower = dst + 240 * stride;
            memset(lower, 0, 120);
            memcpy(lower + 120, screens[1].constScanLine(y), 960);
            memset(lower + 1080, 0, 120);
            dst += stride;
        }
        return canvas;
    }
    if (scale == 2 && screens[0].format() == canvas.format() && screens[1].format() == canvas.format()) {
        cv::Mat rgbCanvas(canvas.height(), canvas.width(), CV_8UC3, canvas.bits(), canvas.bytesPerLine());
        for (int i = 0; i < 2; ++i) {
            const auto &screen = screens[i];
            cv::Mat source(screen.height(), screen.width(), CV_8UC3, const_cast<uchar *>(screen.constBits()),
                           screen.bytesPerLine());
            auto dest = rgbCanvas(cv::Rect(i * 80, i * 480, i ? 640 : 800, 480));
            if (source.size() == dest.size())
                source.copyTo(dest);
            else
                cv::resize(source, dest, dest.size(), 0, 0, cv::INTER_CUBIC);
        }
        rgbCanvas(cv::Rect(0, 480, 80, 480)).setTo(cv::Scalar::all(0));
        rgbCanvas(cv::Rect(720, 480, 80, 480)).setTo(cv::Scalar::all(0));
        return canvas;
    }
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, scale != 1);
    for (int i = 0; i < 2; ++i)
        painter.drawImage(QRect(i * 40 * scale, i * 240 * scale, (i ? 320 : 400) * scale, 240 * scale),
                          screens[i]);
    return canvas;
}
static cv::Mat rgb(const QImage &im) {
    return cv::Mat(im.height(), im.width(), CV_8UC3, const_cast<uchar *>(im.constBits()), im.bytesPerLine());
}
static QImage image(const cv::Mat &m) {
    return QImage(m.data, m.cols, m.rows, m.step, QImage::Format_RGB888).copy();
}
struct Imaging::Impl {
    QString folder;
    QString fbcnnBackend = "DirectML", fbcnnFallback, rifeBackend, rifeFallback;
    QStringList rifeCandidates;
#ifdef LIVE_TENSORRT
    QString accelerationDirectory;
    bool triedTrt = false;
    std::unique_ptr<ImageTrt> trt, rifeTrt;
#endif
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "3ds-live"};
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::map<QString, Ort::Session> models;
    std::vector<float> input;
    struct FlowState {
        std::array<cv::Ptr<cv::DISOpticalFlow>, 2> dis;
        cv::Mat grid, previousGray;
        qint64 previousKey = 0;
    };
    std::map<int, FlowState> flowByWidth;
    bool isCut(const QImage &a, const QImage &b, cv::Mat &ga, cv::Mat &gb, cv::Mat &delta) {
        auto &state = flowByWidth[a.width()];
        if (state.previousKey == a.cacheKey())
            ga = state.previousGray;
        else
            cv::cvtColor(rgb(a), ga, cv::COLOR_RGB2GRAY);
        cv::cvtColor(rgb(b), gb, cv::COLOR_RGB2GRAY);
        state.previousKey = b.cacheKey();
        state.previousGray = gb;
        cv::absdiff(ga, gb, delta);
        return cv::mean(delta)[0] > 38 || cv::countNonZero(delta > 38) > ga.total() * .40;
    }
    void inferRife(const Images &first, const Images &second, Images &output,
                   const DeviceImages &firstGpu = {}, const DeviceImages &secondGpu = {}) {
        while (!rifeCandidates.isEmpty()) {
            const QString backend = rifeCandidates.first();
            try {
#ifdef LIVE_TENSORRT
                if (backend.startsWith("TensorRT")) {
                    if (!rifeTrt) {
                        rifeTrt = std::make_unique<ImageTrt>(accelerationDirectory, ImageTrt::Rife,
                                                            backend.section(' ', 1).toLower());
                    }
                    rifeTrt->run(first, output, second, firstGpu, secondGpu);
                    rifeBackend = backend;
                    return;
                }
#endif
                const QString model = backend == "DirectML FP16" ? "rife-fp16" : "rife";
                if (backend == "DirectML FP16" && rifeBackend != backend) {
                    for (int width : {400, 320}) {
                        const auto path = QDir(folder).filePath(QString("rife-fp16-%1.onnx").arg(width));
                        if (!QFileInfo(path).isFile()) throw std::runtime_error(("缺少模型 " + path).toStdString());
                    }
                }
                for (int i = 0; i < 2; ++i) {
                    if (first[i].isNull()) continue;
                    cv::Mat pixels;
                    run(model, rgb(first[i]), rgb(second[i])).convertTo(pixels, CV_8UC3, 255);
                    output[i] = image(pixels);
                }
                rifeBackend = backend;
                return;
            } catch (const std::exception &e) {
                rifeBackend.clear();
                if (!rifeFallback.isEmpty()) rifeFallback += "\n";
                rifeFallback += backend + ": " + QString::fromUtf8(e.what());
                if (rifeCandidates.size() == 1) throw;
                rifeCandidates.removeFirst();
#ifdef LIVE_TENSORRT
                rifeTrt.reset();
#endif
                if (backend == "DirectML FP16") {
                    models.erase("rife-fp16320");
                    models.erase("rife-fp16400");
                }
            }
        }
    }
    Ort::Session &session(const QString &name, int width, const QString &key) {
        auto it = models.find(key);
        if (it != models.end())
            return it->second;
        const bool fixedWidth = name == "rife" || name == "rife-fp16";
        QString file = (fixedWidth ? name + "-" + QString::number(width) : name) + ".onnx";
        QString path = QDir(folder).absoluteFilePath(file);
        if (!QFileInfo::exists(path))
            throw std::runtime_error(("缺少模型 " + path).toStdString());
        Ort::SessionOptions options;
        options.SetExecutionMode(ORT_SEQUENTIAL);
        options.DisableMemPattern();
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);
        const OrtDmlApi *api = nullptr;
        Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION,
                                                                reinterpret_cast<const void **>(&api)));
        OrtDmlDeviceOptions device{HighPerformance, Gpu};
        Ort::ThrowOnError(api->SessionOptionsAppendExecutionProvider_DML2(options, &device));
        if (!fixedWidth) {
            Ort::ThrowOnError(Ort::GetApi().AddFreeDimensionOverrideByName(options, "width", width));
            Ort::ThrowOnError(Ort::GetApi().AddFreeDimensionOverrideByName(options, "output_width",
                                                                           width * (name == "sr" ? 4 : 1)));
        }
        return models.emplace(key, Ort::Session(env, path.toStdWString().c_str(), options)).first->second;
    }
    cv::Mat run(const QString &mode, const cv::Mat &a, const cv::Mat &b = {}) {
        int w = a.cols, h = a.rows, channels = a.channels(), plane = w * h;
        std::array<int64_t, 4> shape{1, channels * (b.empty() ? 1 : 2), h, w};
        input.resize(plane * shape[1]);
        int offset = 0;
        for (const auto &im : {a, b}) {
            if (im.empty())
                continue;
            for (int c = 0; c < channels; ++c) {
                cv::Mat dst(h, w, CV_32F, input.data() + offset);
                cv::Mat component;
                cv::extractChannel(im, component, c);
                component.convertTo(dst, CV_32F, 1. / 255);
                offset += plane;
            }
        }
        Ort::Value values[] = {
            Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), 4),
            Ort::Value(nullptr)};
        float alpha = .5f;
        int64_t one = 1;
        if (!b.empty())
            values[1] = Ort::Value::CreateTensor<float>(memory, &alpha, 1, &one, 1);
        const char *names[] = {b.empty() ? "image" : "images", "alpha"}, *out = "output";
        const QString key = mode + QString::number(w);
        // The shared DirectML runtime can be entered concurrently by the restoration and RIFE workers.
        // Serialize new sessions through their first dispatch. Warm model inference stays independent.
        static std::mutex initializeDml;
        std::unique_lock initialization(initializeDml, std::defer_lock);
        if (models.find(key) == models.end()) initialization.lock();
        auto result = session(mode, w, key).Run(Ort::RunOptions{nullptr}, names, values, b.empty() ? 1 : 2, &out, 1);
        if (initialization.owns_lock()) initialization.unlock();
        auto dims = result[0].GetTensorTypeAndShapeInfo().GetShape();
        int scale = mode == "sr" ? 4 : 1;
        if (dims != std::vector<int64_t>{1, channels, h * scale, w * scale})
            throw std::runtime_error("Unexpected ONNX output dimensions");
        float *ptr = result[0].GetTensorMutableData<float>();
        std::vector<cv::Mat> planes;
        for (int c = 0; c < channels; ++c)
            planes.emplace_back(h * scale, w * scale, CV_32F, ptr + c * plane * scale * scale);
        cv::Mat merged;
        cv::merge(planes, merged);
        return merged;
    }
};
Imaging::Imaging(QString folder, QString precision) : impl(new Impl) {
    impl->folder = std::move(folder);
    precision = resolveRifePrecision(std::move(precision));
#ifdef LIVE_TENSORRT
    const QDir app = runtimeDirectory();
    if (QDir(impl->folder) == QDir(app.filePath("models")) && QFileInfo(app.filePath("acceleration")).isDir())
        impl->accelerationDirectory = app.filePath("acceleration");
    const bool allowTrt = precision != "directml" && !impl->accelerationDirectory.isEmpty();
    if (allowTrt && precision == "fp16") impl->rifeCandidates << "TensorRT FP16";
#endif
    if (precision == "fp16") impl->rifeCandidates << "DirectML FP16";
#ifdef LIVE_TENSORRT
    if (allowTrt) impl->rifeCandidates << "TensorRT FP32";
#endif
    impl->rifeCandidates << "DirectML";
}
Imaging::~Imaging() = default;
QString Imaging::fbcnnBackend() const {
    return impl->fbcnnBackend;
}
QString Imaging::fbcnnFallback() const {
    return impl->fbcnnFallback;
}
QString Imaging::rifeBackend() const { return impl->rifeBackend; }
QString Imaging::rifeFallback() const { return impl->rifeFallback; }
void Imaging::warmRife() {
    Images blank{QImage(400, 240, QImage::Format_RGB888), QImage(320, 240, QImage::Format_RGB888)}, result;
    for (auto &im : blank) im.fill(Qt::black);
    impl->inferRife(blank, blank, result);
}
quint64 Imaging::rifeDeviceInputs() const {
#ifdef LIVE_TENSORRT
    if (impl->rifeTrt) return impl->rifeTrt->deviceInputs();
#endif
    return 0;
}
#ifndef LIVE_TENSORRT
int Imaging::deviceRgbMismatches(const std::shared_ptr<DeviceEndpoint> &, const QImage &) { return -1; }
#endif
Images Imaging::rifeMidpoints(const Images &before, const Images &after, std::array<bool, 2> *cuts,
                              const DeviceImages &beforeGpu, const DeviceImages &afterGpu) {
    Images changed, result = before;
    if (cuts) cuts->fill(false);
    for (int i = 0; i < 2; ++i) {
        if (before[i].isNull() || after[i].isNull() || before[i].cacheKey() == after[i].cacheKey()) continue;
        if (before[i].size() != QSize(i ? 320 : 400, 240) || after[i].size() != before[i].size() ||
            before[i].format() != QImage::Format_RGB888 || after[i].format() != QImage::Format_RGB888)
            throw std::runtime_error("RIFE expects native RGB888 panels");
        cv::Mat ga, gb, delta;
        if (impl->isCut(before[i], after[i], ga, gb, delta)) {
            if (cuts) (*cuts)[i] = true;
            continue;
        }
        changed[i] = before[i];
    }
    if (!changed[0].isNull() || !changed[1].isNull())
        impl->inferRife(changed, after, result, beforeGpu, afterGpu);
    return result;
}
void Imaging::restorePair(const Images &changed, Images &output, const QString &mode, Images *nativeRestored,
                          DeviceImages *deviceOutput, const std::array<int, 2> &jpegQuality) {
    if (deviceOutput)
        for (int i = 0; i < 2; ++i)
            if (!changed[i].isNull()) (*deviceOutput)[i].reset();
    if (mode == "fbcnn-sr" || mode == "fbcnn-detail") {
        Images temporary;
        auto &clean = nativeRestored ? *nativeRestored : temporary;
        restorePair(changed, clean, "fbcnn");
        for (int i = 0; i < 2; ++i)
            if (!changed[i].isNull())
                output[i] = mode == "fbcnn-detail" ? nativeDetail(clean[i], jpegQuality[i]) : restore(clean[i], "sr");
        return;
    }
#ifdef LIVE_TENSORRT
    if (mode == "fbcnn") {
        try {
            if (!impl->triedTrt) {
                impl->triedTrt = true;
                if (!impl->accelerationDirectory.isEmpty()) {
                    impl->trt = std::make_unique<ImageTrt>(impl->accelerationDirectory, ImageTrt::Fbcnn, "fp16");
                    impl->fbcnnBackend = "TensorRT FP16";
                }
            }
            if (impl->trt) {
                impl->trt->run(changed, output, {}, {}, {}, deviceOutput);
                return;
            }
        } catch (const std::exception &e) {
            impl->trt.reset();
            impl->fbcnnBackend = "DirectML";
            impl->fbcnnFallback = QString::fromUtf8(e.what());
        }
    }
#endif
    for (int i = 0; i < 2; ++i)
        if (!changed[i].isNull())
            output[i] = restore(changed[i], mode);
}
QImage Imaging::restore(const QImage &im, const QString &mode) {
    if (mode == "fast-detail")
        return nativeDetail(restore(im, "gpu"));
    if (mode == "fbcnn-sr" || mode == "fbcnn-detail") {
        Images changed, output;
        const int panel = im.width() == 400 ? 0 : 1;
        changed[panel] = im;
        restorePair(changed, output, mode);
        return output[panel];
    }
    cv::Mat in = rgb(im), result;
    if (mode == "off")
        return im;
    if (mode == "fbcnn" || mode == "sr") {
        auto net = impl->run(mode, in);
        if (mode == "sr") {
            cv::Mat reduced, base;
            cv::resize(net, reduced, {}, .5, .5, cv::INTER_AREA);
            cv::resize(in, base, {}, 2, 2, cv::INTER_LANCZOS4);
            reduced.convertTo(result, CV_8UC3, 255);
            cv::addWeighted(result, .65, base, .35, 0, result);
        } else
            net.convertTo(result, CV_8UC3, 255);
    } else {
        cv::Mat gray, low, high, estimate;
        cv::cvtColor(in, gray, cv::COLOR_RGB2GRAY);
        cv::erode(gray, low, {});
        cv::dilate(gray, high, {});
        if (mode == "gpu")
            impl->run("dncnn", gray).convertTo(estimate, CV_8U, 255);
        else
            cv::GaussianBlur(gray, estimate, cv::Size(5, 5), .85);
        // Both filters apply a bounded grayscale adjustment to all three channels.
        // Only low-contrast pixels can change. Cache the exact original rounding rule.
        using Weights = std::array<std::array<qint8, 511>, 48>;
        static const auto weights = [] {
            std::array<Weights, 2> tables{};
            for (int contrast = 0; contrast < 48; ++contrast)
                for (int delta = -255; delta <= 255; ++delta) {
                    tables[0][contrast][delta + 255] =
                        qint8(std::clamp(cvRound(delta * std::max(0, 192 - contrast * 8) / 255.), -3, 3));
                    tables[1][contrast][delta + 255] = qint8(
                        cvRound(std::clamp(delta, -12, 12) * .5 * std::clamp((contrast - 1) / 3., 0., 1.) *
                                std::clamp((48 - contrast) / 24., 0., 1.)));
                }
            return tables;
        }();
        const bool denoise = mode == "gpu";
        const auto &table = weights[denoise];
        const int cutoff = denoise ? 48 : 24;
        QImage filtered = im.copy();
        for (int y = 1; y < in.rows - 1; ++y) {
            auto dst = filtered.scanLine(y);
            auto hi = high.ptr<uchar>(y), lo = low.ptr<uchar>(y);
            auto est = estimate.ptr<uchar>(y), lum = gray.ptr<uchar>(y);
            for (int x = 1; x < in.cols - 1; ++x) {
                const int contrast = hi[x] - lo[x];
                if (contrast >= cutoff)
                    continue;
                const int delta = table[contrast][255 + est[x] - lum[x]];
                if (delta)
                    for (int c = 0; c < 3; ++c)
                        dst[3 * x + c] = cv::saturate_cast<uchar>(dst[3 * x + c] + delta);
            }
        }
        return filtered;
    }
    return image(result);
}
struct Warp {
    cv::Mat color, error;
};
static cv::Mat sample(const cv::Mat &source, const cv::Mat &coords) {
    cv::Mat sampled;
    cv::remap(source, sampled, coords, {}, cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    return sampled;
}
static Warp projectEndpoint(const cv::Mat &source, const cv::Mat &other, const cv::Mat &forward,
                            const cv::Mat &reverse, const cv::Mat &grid, float fraction) {
    // Map each output pixel back to its source. A second lookup accounts for
    // motion boundaries where the vector changes along the path.
    cv::Mat velocity = sample(forward, grid - fraction * forward);
    cv::Mat at = grid - fraction * velocity, across = at + velocity;
    Warp warped;
    warped.color = sample(source, at);
    cv::Mat diff, gray, cost;
    cv::absdiff(warped.color, sample(other, across), diff);
    cv::cvtColor(diff, gray, cv::COLOR_RGB2GRAY);
    gray.convertTo(cost, CV_32F);
    cv::blur(cost, cost, {3,3});
    // An endpoint is less reliable where its tracked colour changes or its
    // forward and backward vectors fail to return to the original pixel.
    cv::Mat roundTrip = velocity + sample(reverse, across);
    std::array<cv::Mat, 2> xy;
    cv::split(roundTrip, xy.data());
    cv::Mat distance;
    cv::magnitude(xy[0], xy[1], distance);
    cost += 4 * cv::min(distance, 16.);
    cv::blur(cost, warped.error, {5,5});
    return warped;
}
QImage Imaging::midpoint(const QImage &a, const QImage &b, const QString &mode, bool fineFlow, bool *unreliable) {
    if (unreliable) *unreliable = false;
    if (a.cacheKey() == b.cacheKey())
        return a;
    if (mode == "rife") {
        const int i = a.width() == 400 ? 0 : 1;
        Images before, after;
        before[i] = a;
        after[i] = b;
        std::array<bool, 2> cuts;
        auto result = rifeMidpoints(before, after, &cuts);
        if (unreliable) *unreliable = cuts[i];
        return result[i];
    }
    const auto frames = flowBetween(a, b, 2, fineFlow, unreliable);
    return frames.isEmpty() ? a : frames.first();
}
QList<QImage> Imaging::flowBetween(const QImage &a, const QImage &b, int divisions, bool fineFlow, bool *unreliable) {
    if (unreliable) *unreliable = false;
    if (a.isNull() || a.size() != b.size() || a.cacheKey() == b.cacheKey() || divisions < 2 || divisions > 4)
        return {};
    cv::Mat first = rgb(a), second = rgb(b), ga, gb, delta;
    auto &state = impl->flowByWidth[a.width()];
    if (impl->isCut(a, b, ga, gb, delta)) {
        if (unreliable) *unreliable = true;
        return {};
    }
    auto &flow = state.dis[int(fineFlow)];
    if (!flow) {
        flow = cv::DISOpticalFlow::create(fineFlow ? cv::DISOpticalFlow::PRESET_FAST :
                                                     cv::DISOpticalFlow::PRESET_ULTRAFAST);
        flow->setFinestScale(fineFlow ? 1 : 2);
        if (fineFlow) flow->setVariationalRefinementIterations(3);
    }
    if (state.grid.empty()) {
        state.grid.create(ga.size(), CV_32FC2);
        for (int y = 0; y < ga.rows; ++y) {
            auto row = state.grid.ptr<cv::Vec2f>(y);
            for (int x = 0; x < ga.cols; ++x)
                row[x] = cv::Vec2f(float(x), float(y));
        }
    }
    cv::Mat forward, reverse, stationary;
    flow->calc(ga, gb, forward);
    flow->calc(gb, ga, reverse);
    cv::dilate(delta, stationary, cv::getStructuringElement(cv::MORPH_RECT, {5,5}));
    QList<QImage> frames;
    for (int step = 1; step < divisions; ++step) {
        const float fraction = float(step) / divisions;
        const auto left = projectEndpoint(first, second, forward, reverse, state.grid, fraction);
        const auto right = projectEndpoint(second, first, reverse, forward, state.grid, 1.f - fraction);
        cv::Mat out(first.size(), first.type());
        cv::Mat ambiguous(first.size(), CV_8U);
        int ambiguousPixels = 0;
        for (int y = 0; y < out.rows; ++y) {
            const auto aRow = left.color.ptr<cv::Vec3b>(y), bRow = right.color.ptr<cv::Vec3b>(y);
            const auto le = left.error.ptr<float>(y), re = right.error.ptr<float>(y);
            auto dest = out.ptr<cv::Vec3b>(y);
            auto uncertain = ambiguous.ptr<uchar>(y);
            for (int x = 0; x < out.cols; ++x) {
                const auto aColor = aRow[x], bColor = bRow[x];
                int disagreement = 0;
                for (int c = 0; c < 3; ++c)
                    disagreement = std::max(disagreement, std::abs(aColor[c] - bColor[c]));
                uncertain[x] = disagreement > 56 && std::min(le[x], re[x]) > 40;
                ambiguousPixels += uncertain[x];
                // Agreeing warps can be blended; occluded edges progressively
                // use a single endpoint to avoid a translucent second outline.
                const auto preferred = le[x] <= re[x] ? aColor : bColor;
                const int weight = std::clamp(disagreement - 16, 0, 48);
                for (int c = 0; c < 3; ++c) {
                    const int blend = (divisions-step)*aColor[c] + step*bColor[c];
                    dest[x][c] = ((48-weight)*blend + divisions*weight*preferred[c] + 24*divisions) / (48*divisions);
                }
            }
        }
        // A broad coherent patch that neither endpoint can track (for
        // example a kart flipping across the camera) is safer left ungenerated.
        if (ambiguousPixels > first.total() * .06) {
            const int side = first.rows / 5;
            cv::Mat counts;
            cv::boxFilter(ambiguous, counts, CV_32F, {side,side}, {-1,-1}, false, cv::BORDER_CONSTANT);
            double maximum;
            cv::minMaxLoc(counts, nullptr, &maximum);
            if (maximum > side * side / 2) {
                if (unreliable) *unreliable = true;
                return {};
            }
        }
        first.copyTo(out, stationary <= 2);
        frames.append(image(out));
    }
    return frames;
}
} // namespace live
