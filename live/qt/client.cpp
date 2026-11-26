#include "client.h"
#include "runtime_paths.h"
#include "viewer_native.h"
#include "receiver.h"
#include "stream_presets.h"
#include "live_fps.h"
#include "recording.h"
#include "model_check.h"
#include "effect_load.h"
#include <QtWidgets>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QVersionNumber>
#include <QJsonDocument>
using namespace live;
static const QMap<QString, QString> labels = {{"off", "原始"},
                                              {"clean", "轻度增强"},
                                              {"fbcnn", "FBCNN 高画质"},
                                              {"fbcnn-detail", "1× 原生清晰 · FBCNN"},
                                              {"fast-detail", "1× 实时清晰 · DnCNN"},
                                              {"fbcnn-sr", "2× 修复超分 · FBCNN + Real-ESRGAN"},
                                              {"gpu", "DnCNN 降噪"},
                                              {"sr", "Real-ESRGAN 2× 超分"},
                                              {"span", "2× 实时超分 · SPAN"}};
static bool newerVersion(const QString &candidate)
{
    const auto current = QVersionNumber::fromString(QCoreApplication::applicationVersion());
    const auto latest = QVersionNumber::fromString(candidate);
    return !latest.isNull() && QVersionNumber::compare(latest, current) > 0;
}

static void checkForUpdates(QWidget *parent)
{
    auto *manager = new QNetworkAccessManager(parent);
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/nicedayzhu/3ds-live/releases/latest")));
    request.setRawHeader("User-Agent", "3DS-Live-Desktop/" + QCoreApplication::applicationVersion().toUtf8());
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setTransferTimeout(6000);
    auto *reply = manager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, parent, [parent, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;
        const auto data = QJsonDocument::fromJson(reply->readAll()).object();
        auto tag = data.value("tag_name").toString().trimmed();
        if (tag.startsWith('v') || tag.startsWith('V'))
            tag.remove(0, 1);
        if (tag.isEmpty() || !newerVersion(tag))
            return;
        const auto url = data.value("html_url").toString().trimmed();
        QMessageBox box(parent);
        box.setWindowTitle(QStringLiteral("发现新版本"));
        box.setIcon(QMessageBox::Information);
        box.setText(QStringLiteral("发现 3DS Live v%1（当前 v%2）。")
                        .arg(tag, QCoreApplication::applicationVersion()));
        box.setInformativeText(QStringLiteral("是否打开 GitHub Releases 下载新版本？"));
        auto *open = box.addButton(QStringLiteral("打开下载页"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("稍后"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == open)
            QDesktopServices::openUrl(url.isEmpty() ? QUrl(QStringLiteral("https://github.com/nicedayzhu/3ds-live/releases")) : QUrl(url));
    });
}

struct Options {
    QString ip, mode = "off", motion = "off", rifePrecision = "fp32", record, rtsp, stats, models,
        layout = "stacked";
    int port = 8001, scale = 1, fps = 30, rate = 800000, jpegQuality = 0, delay = 100;
    std::optional<int> packetPacing;
    bool adaptiveRate = true, mute = false, debug = false;
};
struct PanelCadence {
    qint64 count = 0, previousCount = 0, last = 0, longest = 0, longGaps = 0;
    quint32 session = 0;
    void select(quint32 id, qint64 wall) {
        if (last && session == id) {
            const auto gap = wall - last;
            longest = std::max(longest, gap);
            longGaps += gap > 100000;
        }
        session = id;
        last = wall;
        ++count;
    }
    double fps(double seconds) {
        const auto delta = count - previousCount;
        previousCount = count;
        return delta / seconds;
    }
};
static int pictureScale(const QString &mode, int requested) {
    if (usesSuperResolution(mode)) return 2;
    if (isNativeDetail(mode)) return 1;
    return requested;
}
static int layoutMask(const QString &) {
    // Display layout is a client-side crop. The sender always streams both
    // panels so switching to a single-screen view cannot freeze it.
    return 3;
}
bool validIp(const QString &ip) {
    return ip.isEmpty() || QHostAddress(ip).protocol() == QAbstractSocket::IPv4Protocol;
}
static bool validRtsp(const QString &url) {
    return url.isEmpty() || (QUrl(url).scheme() == "rtsp" && !QUrl(url).host().isEmpty());
}
static QString effectFailureSummary(const QString &error) {
    if (error.contains(QStringLiteral("缺少模型")))
        return QStringLiteral("缺少模型文件，请检查 runtime/models");
    return QStringLiteral("推理失败，可在 F12 查看后端错误");
}
int runClient(QApplication &app, quint16 handheldDiscoveryPort) {
    configureOpenCV();
    app.setApplicationVersion(LIVE_VERSION);
    app.setOrganizationName("3ds-live");
    app.setApplicationName("3DS Live");
    QCommandLineParser cli;
    cli.setApplicationDescription("Native Qt 3DS live receiver");
    cli.addHelpOption();
    cli.addVersionOption();
    auto arg = [&](QString name, QString help, QString def = {}) { cli.addOption({name, help, "value", def}); };
    arg("sender", "3DS IPv4 (empty for automatic discovery)");
    arg("port", "UDP listen port", "8001");
    arg("enhance", "off, clean, fbcnn, fbcnn-detail or fast-detail (1x), fbcnn-sr, sr or span (2x), or gpu", "off");
    arg("interpolate", "off, flow or rife", "off");
    arg("rife-precision", "RIFE inference precision: fp32 (default) or fp16");
    arg("scale", "Display scale 1 or 2", "1");
    arg("layout", "Screen layout: stacked, top-only or bottom-only", "stacked");
    arg("fps", "Source FPS (15, 20, 24 or 30)", "30");
    arg("rate", "Sender byte/s ceiling", "800000");
    arg("jpeg-quality", "Handheld JPEG quality target: 0 (automatic), 15, 20, 25, 30, 35, 40, 50, 60, 70 or 80", "0");
    arg("stream-preset", "Fixed-rate streaming settings: hd30, balanced30 or low24. Explicit fps, rate and JPEG options override the preset.");
    arg("delay", "Playout delay in ms (100 standard, 160 for FBCNN)", "100");
    arg("record", "Save MP4 on launch");
    arg("rtsp", "Publish to an RTSP server");
    arg("seconds", "Automatically close after this many seconds");
    arg("trace", "Write source, presentation and recording events as JSON lines");
    arg("packet-pacing", "Compare handheld send timing: baseline or spaced (requires a compatible handheld)");
    arg("stats", "Append diagnostic snapshots as JSON lines");
    arg("models", "ONNX model directory", live::runtimeDirectory().filePath("models"));
    arg("check-models", "Run comma-separated native model checks and write results to --stats");
    cli.addOption({"mute", "Mute speakers"});
    cli.addOption({"debug", "Open F12 diagnostics"});
    cli.addOption({"preview", "Open preview directly"});
    cli.addOption({"fixed-rate", "Keep the requested sender rate cap fixed"});
    cli.addOption({"adaptive-rate", "Adapt only the sender rate cap to the connection"});
    cli.process(app);
    Options o;
    if (cli.isSet("packet-pacing")) {
        const auto mode = cli.value("packet-pacing");
        if (mode == "baseline") o.packetPacing = 0;
        else if (mode == "spaced") o.packetPacing = 1;
        else {
            qWarning("--packet-pacing expects baseline or spaced");
            return 2;
        }
    }
    o.models = cli.value("models");
    o.stats = cli.value("stats");
    QSettings prefs;
    const bool interactive = QCoreApplication::arguments().size() == 1;
    if (interactive && !prefs.contains("importedPreferences")) {
        QSettings previous(QSettings::defaultFormat(), QSettings::UserScope, "3ds-recorder", "3DS Live");
        for (const auto &key : {"port", "fps", "rate", "mode", "motion", "scale", "muted",
                                "rifePrecision", "recordDirectory"}) {
            if (!prefs.contains(key) && previous.contains(key))
                prefs.setValue(key, previous.value(key));
        }
        prefs.setValue("importedPreferences", true);
    }
    const auto envPrecision = qEnvironmentVariable("THREE_DS_LIVE_RIFE_PRECISION").toLower();
    if (cli.isSet("rife-precision")) {
        o.rifePrecision = cli.value("rife-precision").toLower();
        if (o.rifePrecision != "fp16" && o.rifePrecision != "fp32") {
            qWarning("--rife-precision expects fp32 or fp16");
            return 2;
        }
    } else if (QStringList{"fp32", "fp16", "directml"}.contains(envPrecision)) {
        o.rifePrecision = envPrecision;
    } else if (interactive && prefs.value("rifePrecision").toString() == "fp16") {
        o.rifePrecision = "fp16";
    }
    if (cli.isSet("check-models"))
        return checkModels(o.models, o.stats, cli.value("check-models"), o.rifePrecision);
    o.ip = cli.value("sender");
    o.port = cli.value("port").toInt();
    o.mode = cli.value("enhance");
    o.motion = cli.value("interpolate");
    o.scale = cli.value("scale").toInt();
    const StreamPreset *preset = nullptr;
    if (cli.isSet("stream-preset")) {
        for (const auto &candidate : streamPresets)
            if (cli.value("stream-preset") == candidate.id) preset = &candidate;
        if (!preset) {
            qWarning("--stream-preset expects hd30, balanced30 or low24");
            return 2;
        }
    }
    if (cli.isSet("fixed-rate") && cli.isSet("adaptive-rate")) {
        qWarning("Choose one rate control mode");
        return 2;
    }
    o.fps = preset && !cli.isSet("fps") ? preset->fps : cli.value("fps").toInt();
    o.rate = preset && !cli.isSet("rate") ? preset->rate : cli.value("rate").toInt();
    o.adaptiveRate = cli.isSet("adaptive-rate") || (!preset && !cli.isSet("fixed-rate"));
    bool qualityParsed = false;
    o.jpegQuality = cli.value("jpeg-quality").toInt(&qualityParsed);
    if (!qualityParsed || !isSourceJpegQuality(o.jpegQuality)) {
        qWarning("--jpeg-quality expects 0, 15, 20, 25, 30, 35, 40, 50, 60, 70 or 80");
        return 2;
    }
    if (preset && !cli.isSet("jpeg-quality")) o.jpegQuality = preset->jpegQuality;
    o.delay = cli.value("delay").toInt();
    o.record = cli.value("record");
    o.rtsp = cli.value("rtsp");
    o.mute = cli.isSet("mute");
    o.debug = cli.isSet("debug");
    if (interactive) {
        o.ip = prefs.value("ip").toString();
        o.port = prefs.value("port", 8001).toInt();
        o.mode = prefs.value("mode", "off").toString();
        o.motion = prefs.value("motion", "off").toString();
        o.scale = prefs.value("scale", 1).toInt();
        const int savedFps = prefs.value("fps", 30).toInt();
        o.fps = int(live_migrate_fps(savedFps));
        if (o.fps != savedFps) prefs.setValue("fps", o.fps);
        o.rate = prefs.value("rate", 800000).toInt();
        o.adaptiveRate = prefs.value("adaptiveRate", true).toBool();
        o.layout = prefs.value("layout", "stacked").toString();
    o.jpegQuality = prefs.value("jpegQuality", 0).toInt();
        if (!isSourceJpegQuality(o.jpegQuality))
            o.jpegQuality = 0;
        o.mute = prefs.value("muted", false).toBool();
        if (!validIp(o.ip))
            o.ip.clear();
        if (o.port < 1 || o.port > 65535)
            o.port = 8001;
        if (!labels.contains(o.mode))
            o.mode = "off";
        if (!QStringList{"off", "flow", "rife"}.contains(o.motion))
            o.motion = "off";
        if (o.scale != 1 && o.scale != 2)
            o.scale = 1;
        if (!live_valid_fps(o.fps))
            o.fps = 30;
        if (!rates().contains(o.rate))
            o.rate = 800000;
    }
    if (!cli.isSet("scale"))
        o.scale = pictureScale(o.mode, o.scale);
    if (!cli.isSet("delay") && usesFbcnn(o.mode))
        o.delay = 160;
    if (!live_valid_fps(o.fps)) {
        qWarning("--fps expects 15, 20, 24 or 30");
        return 2;
    }
    if ((!o.ip.isEmpty() && QHostAddress(o.ip).protocol() != QAbstractSocket::IPv4Protocol) || o.port < 1 ||
        o.port > 65535 || !labels.contains(o.mode) || !QStringList{"off", "rife", "flow"}.contains(o.motion) ||
        (o.scale != 1 && o.scale != 2) || o.scale != pictureScale(o.mode, o.scale) ||
        !rates().contains(o.rate) || o.delay < 20 ||
        o.delay > 500 || (!o.record.isEmpty() && QFileInfo::exists(o.record)) ||
        (!o.rtsp.isEmpty() && QUrl(o.rtsp).scheme() != "rtsp"))
        return 2;
    QFile log(o.stats);
    if (!o.stats.isEmpty() && !log.open(QIODevice::WriteOnly | QIODevice::Append))
        return 2;
    Timeline timeline(o.delay);
    TraceWriter trace;
    const bool tracePaints = cli.isSet("trace");
    if (tracePaints && !trace.start(cli.value("trace"), timeline.diagnostics))
        return 2;
    Pipeline pipeline(timeline, o.models, o.scale, o.mode, o.motion, o.rifePrecision);
    Speaker speaker(timeline);
    speaker.mute = o.mute;
    std::unique_ptr<Receiver> receiver;
    auto listen = [&] {
        receiver.reset();
        receiver = std::make_unique<Receiver>(timeline, pipeline, QHostAddress(o.ip), o.port, o.fps, o.rate,
                                              handheldDiscoveryPort, o.jpegQuality, o.adaptiveRate, o.packetPacing);
        receiver->configure(o.fps, o.rate, o.jpegQuality, o.adaptiveRate, layoutMask(o.layout));
    };
    listen();
    QJsonObject modes;
    for (auto it = labels.begin(); it != labels.end(); ++it)
        modes[it.key()] = it.value();
    auto waitingNotice = [&] {
        return (o.ip.isEmpty() ? QString("正在发现同一局域网内的 3DS") : QString("等待掌机 %1").arg(o.ip))
               + QString(" · UDP %1\n在掌机上按 R + SELECT").arg(o.port);
    };
    void *view = createView({{"title", QStringLiteral("3DS Live · v%1").arg(QCoreApplication::applicationVersion())},
                             {"notice", waitingNotice()},
                             {"ip", o.ip},
                             {"scale", o.scale},
                             {"port", o.port},
                             {"jpegQuality", o.jpegQuality}, {"layout", o.layout},
                             {"adaptiveRate", o.adaptiveRate},
                             {"rtsp", o.rtsp},
                             {"interpolate", o.motion},
                             {"rife_precision", o.rifePrecision},
                             {"width", 400 * o.scale},
                             {"height", 480 * o.scale},
                             {"mute", true},
                             {"mode", o.mode},
                             {"modes", modes},
                             {"motion", true},
                             {"record", true},
                             {"debug", o.debug}});
    QTimer::singleShot(2500, static_cast<QWidget *>(view), [view] {
        checkForUpdates(static_cast<QWidget *>(view));
    });
    auto options = [&](int mask, int max) {
        QJsonArray caps, speeds;
        for (int r : rates())
            if (r <= max)
                caps.append(r);
        for (int f : {15, 20, 24, 30})
            if (mask & live_fps_flag(f))
                speeds.append(f);
        updateView(view, {{"fpsOptions", speeds}, {"rateOptions", caps}, {"fps", o.fps}, {"rate", o.rate}});
    };
    int mask = LIVE_FPS_MASK, max = 2000000;
    options(mask, max);
    updateView(view, {{"muted", o.mute}});
    std::unique_ptr<Recording> recording, publish;
    auto recordingFps = [&] { return o.motion != "off" ? 60 : o.fps; };
    auto begin = [&](QString path, bool allowReplace) {
        recording = std::make_unique<Recording>(timeline, QFileInfo(path).absoluteFilePath(), o.scale,
                                                recordingFps(), allowReplace);
    };
    if (!o.record.isEmpty())
        begin(o.record, false);
    if (!o.rtsp.isEmpty())
        publish = std::make_unique<Recording>(timeline, o.rtsp, o.scale, recordingFps());
    app.setQuitOnLastWindowClosed(false);
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setSingleShot(true);
    QObject::connect(&timeline, &Timeline::earlierFrameReady, &timer, [&timer] {
        timer.start(0);
    }, Qt::QueuedConnection);
    qint64 lastStats = 0, lastImage = 0, painted = 0, lastPaint = 0, maxPaint = 0;
    QString oldState, recordError, lastMode = o.mode == "off" ? "fbcnn" : o.mode,
                                   lastMotion = o.motion == "off" ? "flow" : o.motion;
    QString requestedMode = o.mode, requestedMotion = o.motion;
    QJsonObject pictureFailures, motionFailures;
    EffectLoad pictureLoad, motionLoad;
    bool pictureBusy = false, motionBusy = false;
    qint64 lastBusyToastUs = 0;
    QString busyToastKey;
    qint64 loadSession = -1;
    qint64 lastPts = std::numeric_limits<qint64>::min(), presented = 0, onTime = 0, intermediate = 0,
           lastPresentWall = 0, largestGap = 0;
    qint64 lastSourceWall = 0, sourceBucketGap = 0;
    quint32 lastSourceSession = 0;
    std::deque<qint64> recentSourcePresentations;
    std::array<std::optional<SourceFrame>,2> lastShownPanels;
    std::array<PanelCadence,2> panelCadence{};
    qint64 bottomGenerated = 0, previousBottomGenerated = 0, canvasChoices = 0, previousCanvasChoices = 0;
    int exitCode = 0, fallbackWindows = 0;
    qint64 bucketGap = 0, previousCount = 0, previousSynthetic = 0, previousEnhanced = 0, toastUntil = 0;
    QString toast;
    qint64 fbcnnFrames = 0, fbcnnOriginals = 0;
    QJsonObject latest;
    QJsonArray samples;
    setDebugCapture(view, [&](bool mark) {
        auto now = clockUs();
        if (mark)
            timeline.diagnostics.add({"mark", {}, now});
        return QJsonObject{{"schema", 1},        {"version", QCoreApplication::applicationVersion()},
                           {"captured_us", now}, {"snapshot", latest},
                           {"samples", samples}, {"events", timeline.diagnostics.window(now)}};
    });
    auto remember = [&] {
        if (!interactive)
            return;
        prefs.setValue("ip", o.ip);
        prefs.setValue("port", o.port);
        prefs.setValue("fps", o.fps);
        prefs.setValue("rate", o.rate);
        prefs.setValue("adaptiveRate", o.adaptiveRate);
        prefs.setValue("jpegQuality", o.jpegQuality);
    prefs.setValue("layout", o.layout);
        prefs.setValue("mode", requestedMode);
        prefs.setValue("motion", requestedMotion);
        prefs.setValue("scale", o.scale);
        prefs.setValue("muted", o.mute);
    };
    auto notify = [&](const QString &text) {
        toast = text;
        toastUntil = clockUs() + 5000000;
    };
    auto motionLabel = [](const QString &method) {
        return method == "rife" ? QStringLiteral("RIFE") : method == "flow" ? QStringLiteral("DIS 光流") : QStringLiteral("关闭补帧");
    };
    auto lighterMode = [&] {
        return o.mode == "clean" || pictureFailures.contains("clean") ? QStringLiteral("off") : QStringLiteral("clean");
    };
    auto lighterMotion = [&] {
        return o.motion == "flow" || motionFailures.contains("flow") ? QStringLiteral("off") : QStringLiteral("flow");
    };
    auto publishEffects = [&] {
        QJsonObject pictures, motions;
        QStringList lines;
        for (auto it = pictureFailures.begin(); it != pictureFailures.end(); ++it) {
            pictures[it.key()] = effectFailureSummary(it.value().toString());
            lines << labels.value(it.key()) + " · " + pictures.value(it.key()).toString();
        }
        for (auto it = motionFailures.begin(); it != motionFailures.end(); ++it) {
            motions[it.key()] = effectFailureSummary(it.value().toString());
            lines << motionLabel(it.key()) + " · " + motions.value(it.key()).toString();
        }
        if (o.mode != requestedMode) lines << "当前画质：" + labels.value(o.mode);
        if (o.motion != requestedMotion) lines << "当前补帧：" + motionLabel(o.motion);
        if (pictureBusy) lines << "本机画质处理持续赶不上收帧，建议切换到" + labels.value(lighterMode()) + "。";
        if (motionBusy) lines << "本机补帧持续超时，建议使用：" + motionLabel(lighterMotion()) + "。";
        updateView(view, {{"effects", QJsonObject{{"blocked_picture", pictures}, {"blocked_motion", motions},
            {"message", lines.join('\n')}, {"can_reduce", pictureBusy || motionBusy},
            {"can_retry", !pictures.isEmpty() || !motions.isEmpty()}}}});
    };
    auto configurePicture = [&] {
        o.mode = requestedMode;
        if (pictureFailures.contains(o.mode)) o.mode = lighterMode();
        o.motion = requestedMotion;
        if (motionFailures.contains(o.motion)) o.motion = lighterMotion();
        if (requestedMode != "off") lastMode = requestedMode;
        if (requestedMotion != "off") lastMotion = requestedMotion;
        o.scale = pictureScale(o.mode, o.scale);
        pipeline.configure(o.mode, o.motion, o.scale, o.rifePrecision);
        updateView(view, {{"mode", o.mode}, {"scale", o.scale}, {"motion", o.motion}, {"rife_precision", o.rifePrecision}});
        fallbackWindows = 0;
        pictureLoad = motionLoad = {};
        pictureBusy = motionBusy = false;
        publishEffects();
        remember();
    };
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        for (auto event : takeEvents(view)) {
            auto e = event.toObject();
            auto type = e["type"].toString();
            if (type == "close") {
                app.quit();
                return;
            }
            if (type == "limit") {
                if (e.contains("layout")) {
                    const QString next = e["layout"].toString();
                    if (next != "stacked" && next != "side-by-side" && next != "separate" &&
                        next != "top-only" && next != "bottom-only") continue;
                    o.layout = next;
                    receiver->configure(o.fps, o.rate, o.jpegQuality, o.adaptiveRate, layoutMask(next));
                    notify(next == "top-only" ? "仅上屏：已释放下屏编码与带宽" :
                           next == "bottom-only" ? "仅下屏：已释放上屏编码与带宽" :
                           next == "separate" ? "已分离下屏窗口" : "已切换画面布局");
                    updateView(view, {{"layout", next}});
                    remember();
                    continue;
                }
                const int hz = e["fps"].toInt(o.fps), cap = e["rate"].toInt(o.rate);
                const int jpegQuality = e["jpegQuality"].toInt(o.jpegQuality);
                const bool adaptive = e["adaptiveRate"].toBool(o.adaptiveRate);
                if (!live_valid_fps(hz) || !isSourceJpegQuality(jpegQuality))
                    continue;
                if (e.contains("fps") && !(mask & live_fps_flag(hz)))
                    continue;
                if (e.contains("rate") && (!rates().contains(cap) || cap > max))
                    continue;
                if (hz != o.fps || cap != o.rate || jpegQuality != o.jpegQuality || adaptive != o.adaptiveRate) {
                    o.fps = hz;
                    o.rate = cap;
                    o.jpegQuality = jpegQuality;
                    o.adaptiveRate = adaptive;
                    receiver->configure(hz, cap, jpegQuality, adaptive, layoutMask(o.layout));
                    remember();
                }
                updateView(view, {{"fps", hz}, {"rate", cap}, {"jpegQuality", jpegQuality}, {"adaptiveRate", adaptive}});
            }
            if (type == "apply") {
                QString ip = e["ip"].toString(), url = e["rtsp"].toString();
                if (!validIp(ip) || !validRtsp(url)) {
                    notify("请检查掌机 IPv4 或 RTSP 地址");
                    continue;
                }
                bool changed = ip != o.ip || e["port"].toInt() != o.port;
                const QString nextLayout = e["layout"].toString(o.layout);
                const bool layoutChanged = nextLayout != o.layout && (nextLayout == "stacked" || nextLayout == "side-by-side" || nextLayout == "separate" ||
                            nextLayout == "top-only" || nextLayout == "bottom-only");
                const int jpegQuality = e["jpegQuality"].toInt(o.jpegQuality);
                const bool adaptive = e["adaptiveRate"].toBool(o.adaptiveRate);
                if (!live_valid_fps(e["fps"].toInt()) || !isSourceJpegQuality(jpegQuality))
                    continue;
                const bool limitsChanged = e["fps"].toInt() != o.fps || e["rate"].toInt() != o.rate ||
                                           jpegQuality != o.jpegQuality || adaptive != o.adaptiveRate ||
                                           layoutChanged;
                o.ip = ip;
                o.port = e["port"].toInt();
                o.fps = e["fps"].toInt();
                o.rate = e["rate"].toInt();
                o.jpegQuality = jpegQuality;
                o.adaptiveRate = adaptive;
                if (layoutChanged) {
                    o.layout = nextLayout;
                    receiver->configure(o.fps, o.rate, o.jpegQuality, o.adaptiveRate,
                                        3);
                    notify(nextLayout == "top-only" ? "仅上屏：已释放下屏编码与带宽" :
                           nextLayout == "bottom-only" ? "仅下屏：已释放上屏编码与带宽" : "已恢复上下双屏");
                }
                if (changed)
                    listen();
                else if (limitsChanged)
                    receiver->configure(o.fps, o.rate, o.jpegQuality, o.adaptiveRate, layoutMask(o.layout));
                updateView(view, {{"fps", o.fps}, {"rate", o.rate}, {"jpegQuality", o.jpegQuality}, {"layout", o.layout},
                                  {"adaptiveRate", o.adaptiveRate}});
                int scale = e["scale"].toInt();
                QString motion = e["motion"].toString();
                QString precision = e["rife_precision"].toString(o.rifePrecision);
                // A legacy directml override requests FP32 without TensorRT.
                if (precision == "fp32" && o.rifePrecision == "directml")
                    precision = "directml";
                if (scale != o.scale || motion != o.motion || precision != o.rifePrecision) {
                    if (interactive && precision != o.rifePrecision)
                        prefs.setValue("rifePrecision", precision);
                    if (precision != o.rifePrecision) motionFailures.remove("rife");
                    if (motion != o.motion) requestedMotion = motion;
                    o.rifePrecision = precision;
                    o.scale = scale;
                    configurePicture();
                }
                if (url != o.rtsp) {
                    if (publish)
                        publish->stop();
                    o.rtsp = url;
                    if (!publish && !url.isEmpty())
                        publish =
                            std::make_unique<Recording>(timeline, url, o.scale, recordingFps());
                }
                remember();
            }
            if (type == "retry")
                listen();
            if (type == "retry-effects") {
                pictureFailures = {};
                motionFailures = {};
                configurePicture();
                notify("已恢复效果选项，将重新检测选用的模式");
            }
            if (type == "reduce-effects" && (pictureBusy || motionBusy)) {
                if (pictureBusy) requestedMode = lighterMode();
                if (motionBusy) requestedMotion = lighterMotion();
                configurePicture();
                notify("已应用较轻的画面设置");
            }
            if (type == "mute") {
                o.mute = !o.mute;
                speaker.mute = o.mute;
                remember();
                updateView(view, {{"muted", o.mute}});
            }
            if (type == "mode" || type == "motion" || type == "enhance") {
                if (type == "mode")
                    requestedMode = e["value"].toString();
                if (type == "enhance") {
                    if (o.mode == "off")
                        requestedMode = pictureFailures.contains(lastMode) ? lighterMode() : lastMode;
                    else {
                        lastMode = requestedMode;
                        requestedMode = "off";
                    }
                }
                if (type == "motion") {
                    const QString method = e.value("value").toString(o.motion == "off" ? lastMotion : QStringLiteral("off"));
                    if (method != "off" && method != "flow" && method != "rife") continue;
                    const QString next = motionFailures.contains(method) ? lighterMotion() : method;
                    if (next == requestedMotion) continue;
                    requestedMotion = next;
                }
                configurePicture();
            }
            if (type == "record" && !recording) {
                begin(e["path"].toString(), e["replace"].toBool());
                oldState.clear();
                recordError.clear();
            }
            if (type == "stop-record" && recording)
                recording->stop();
        }
        auto now = clockUs();
        int nextWakeMs;
        auto choice = timeline.present(&nextWakeMs);
        timer.start(nextWakeMs);
        const auto &im = choice.image;
        bool synthetic = choice.generatedPanels & 1, enhanced = !synthetic && choice.quality != "original";
        if (!im.isNull() && choice.id != lastPts) {
            lastPts = choice.id;
            ++canvasChoices;
            for (int i = 0; i < 2; ++i) {
                if (choice.updatedPanels & (1 << i)) {
                    panelCadence[i].select(choice.source.session, choice.time);
                    if (choice.source.panels[i]) {
                        lastShownPanels[i] = choice.source;
                        lastShownPanels[i]->pts = choice.source.panels[i]->pts;
                    }
                }
            }
            bottomGenerated += bool(choice.generatedPanels & 2);
            const bool realTop = choice.updatedPanels & 1;
            if (realTop || synthetic) ++presented;
            if (realTop) {
                if (lastSourceSession != choice.source.session) recentSourcePresentations.clear();
                recentSourcePresentations.push_back(choice.time);
                if (lastSourceWall && lastSourceSession == choice.source.session)
                    sourceBucketGap = std::max(sourceBucketGap, now - lastSourceWall);
                lastSourceWall = now;
                lastSourceSession = choice.source.session;
            }
            onTime += realTop && enhanced;
            if (usesFbcnn(o.mode) && realTop) {
                ++fbcnnFrames;
                fbcnnOriginals += choice.quality != o.mode;
            }
            intermediate += synthetic;
            if ((realTop || synthetic) && lastPresentWall) {
                largestGap = std::max(largestGap, now - lastPresentWall);
                bucketGap = std::max(bucketGap, now - lastPresentWall);
            }
            if (realTop || synthetic) lastPresentWall = now;
        }
        if (im.isNull() && lastImage) {
            showFrame(view, {});
            lastImage = 0;
        }
        if (!im.isNull() && im.cacheKey() != lastImage) {
            std::function<void()> onPainted;
            if (tracePaints) {
                TraceEvent event{"paint", choice.source, 0, 0, choice.id, im.cacheKey(), choice.quality};
                event.generatedPanels = choice.generatedPanels;
                onPainted = [&timeline, event = std::move(event)]() mutable {
                    timeline.diagnostics.add(std::move(event));
                };
            }
            showFrame(view, im, std::move(onPainted));
            lastImage = im.cacheKey();
            ++painted;
            if (lastPaint)
                maxPaint = std::max(maxPaint, now - lastPaint);
            lastPaint = now;
        }
        if (recording) {
            auto state = recording->stats();
            QString name = state["state"].toString();
            if (name != oldState) {
                updateView(view, {{"recording", name}, {"path", state["file"]}, {"error", state["error"]}});
                if (name == "finished")
                    notify("录像已保存 · " + QFileInfo(state["file"].toString()).fileName());
                oldState = name;
                if (name == "failed") {
                    recordError = state["error"].toString();
                    exitCode = 1;
                }
                if (name == "finished" || name == "failed")
                    recording.reset();
            }
        }
        if (publish) {
            auto ps = publish->stats();
            auto val = ps["state"].toString();
            if (val == "finished" || val == "failed") {
                auto old = ps["file"].toString();
                if (val == "failed") {
                    notify("RTSP · " + ps["error"].toString());
                    if (!interactive)
                        exitCode = 1;
                }
                publish.reset();
                if (!o.rtsp.isEmpty() && o.rtsp != old)
                    publish =
                        std::make_unique<Recording>(timeline, o.rtsp, o.scale, recordingFps());
                else
                    o.rtsp.clear();
            }
        }
        if (now - lastStats < 1000000)
            return;
        double interval = lastStats ? (now - lastStats) / 1e6 : 1.;
        lastStats = now;
        auto report = receiver->stats(), processing = pipeline.stats();
        const auto pictureError = processing["model_error"].toString();
        const auto motionError = processing["motion_error"].toString();
        QStringList failedEffects;
        if (o.mode != "off" && !pictureError.isEmpty()) {
            failedEffects << labels.value(o.mode);
            pictureFailures[o.mode] = pictureError;
        }
        if (o.motion != "off" && !motionError.isEmpty()) {
            failedEffects << motionLabel(o.motion);
            motionFailures[o.motion] = motionError;
        }
        if (!failedEffects.isEmpty()) {
            configurePicture();
            QStringList replacements;
            if (!pictureError.isEmpty()) replacements << labels.value(o.mode);
            if (!motionError.isEmpty()) replacements << motionLabel(o.motion);
            notify(failedEffects.join(" / ") + " 不可用，已切换为" + replacements.join(" / "));
            processing = pipeline.stats();
        }
        for (auto it = processing.begin(); it != processing.end(); ++it)
            report[it.key()] = it.value();
        const auto paintStats = takePaintStats(view);
        for (auto it = paintStats.begin(); it != paintStats.end(); ++it)
            report[it.key()] = it.value();
        int delta = int(presented - previousCount), sourceDelta = delta - int(intermediate - previousSynthetic);
        report["time"] = QTime::currentTime().toString("HH:mm:ss");
        report["wall_us"] = now;
        while (!recentSourcePresentations.empty() && recentSourcePresentations.front() < now - 10000000)
            recentSourcePresentations.pop_front();
        auto window = report.value("delivery_window").toObject();
        if (!window.isEmpty() && lastSourceSession == report.value("session").toInteger()) {
            const auto begin = window.value("start_us").toInteger();
            const auto end = window.value("end_us").toInteger();
            if (begin > 0 && end > begin && end <= now && begin >= now - 10000000) {
                const auto first = std::upper_bound(recentSourcePresentations.begin(), recentSourcePresentations.end(), begin);
                const auto last = std::upper_bound(first, recentSourcePresentations.end(), end);
                const auto count = qint64(last-first);
                window["presented_tops"] = count;
                window["presented_top_fps"] = count * 1000000. / (end-begin);
                report["delivery_window"] = window;
            }
        }
        {
            const double sentFps = report.value("device_sent_fps").toDouble();
            const double media = report.value("device_media_bytes_per_s").toDouble();
            if (sentFps > 0 && media > 0)
                report["pair_bytes"] = media / sentFps;
        }
        report["fps_presented"] = delta / interval;
        report["fps_source_presented"] = sourceDelta / interval;
        report["fps_generated"] = (delta - sourceDelta) / interval;
        report["fps_top_presented"] = panelCadence[0].fps(interval);
        report["fps_bottom_presented"] = panelCadence[1].fps(interval);
        report["top_gaps_over_100ms"] = panelCadence[0].longGaps;
        report["bottom_gaps_over_100ms"] = panelCadence[1].longGaps;
        report["top_max_gap_ms"] = panelCadence[0].longest / 1000.;
        report["bottom_max_gap_ms"] = panelCadence[1].longest / 1000.;
        report["top_presented_frames"] = panelCadence[0].count;
        report["bottom_presented_frames"] = panelCadence[1].count;
        report["fps_bottom_generated"] = (bottomGenerated - previousBottomGenerated) / interval;
        report["bottom_generated_at_presentation"] = bottomGenerated;
        report["fps_canvas_presented"] = (canvasChoices - previousCanvasChoices) / interval;
        previousCanvasChoices = canvasChoices;
        previousBottomGenerated = bottomGenerated;
        for (int i=0; i<2; ++i) {
            const auto &panel = lastShownPanels[i];
            if (panel && report["session"].toInteger() == panel->session)
                report[i ? "bottom_source_age_ms" : "top_source_age_ms"] =
                    std::max<qint64>(0, now - timeline.deadline(*panel) + timeline.delay) / 1000.;
        }
        report["source_interval_max_gap_ms"] = sourceBucketGap / 1000.;
        sourceBucketGap = 0;
        report["interval_max_gap_ms"] = bucketGap / 1000.;
        bucketGap = 0;
        if (sourceDelta > 0 && o.mode != "off")
            report["coverage_pct"] = (onTime - previousEnhanced) * 100. / sourceDelta;
        previousCount = presented;
        previousEnhanced = onTime;
        previousSynthetic = intermediate;
        report["fbcnn_presented"] = fbcnnFrames;
        report["fbcnn_original_frames"] = fbcnnOriginals;
        report["scale"] = o.scale;
        report["version"] = QCoreApplication::applicationVersion();
        report["mode"] = o.mode;
        report["motion"] = o.motion;
        report["requested_mode"] = requestedMode;
        report["requested_motion"] = requestedMotion;
        if (!pictureFailures.isEmpty()) report["picture_failures"] = pictureFailures;
        if (!motionFailures.isEmpty()) report["motion_failures"] = motionFailures;
        report["presented_changes"] = painted;
        report["max_visual_change_gap_ms"] = maxPaint / 1000.;
        report["playout_delay_ms"] = o.delay;
        report["presented_frames"] = presented;
        report["enhanced_at_presentation"] = onTime;
        report["synthetic_at_presentation"] = intermediate;
        report["max_presentation_gap_ms"] = largestGap / 1000.;
        report["audio_device"] = speaker.opened.load();
        report["speaker_output"] = timeline.speakerStats();
        report["muted"] = o.mute;
        report["stats_file"] = o.stats;
        report["recording"] =
            recording ? recording->stats() : QJsonObject{{"state", "idle"}, {"error", recordError}};
        if (publish)
            report["rtsp"] = publish->stats();
        int newMask = report["fps_mask"].toInt(mask), newMax = report["max_rate"].toInt(max);
        if (newMask != mask || newMax != max) {
            mask = newMask;
            max = newMax;
            options(mask, max);
        }
        const QString sourceRate = QString::number(report["fps_source_presented"].toDouble(), 'f', 1);
        const QString displayedTop = QString::number(report["fps_presented"].toDouble(), 'f', 1);
        QString cadence = o.motion == "off" ? QString("源 %1 FPS").arg(sourceRate)
                                            : QString("显示 %1 · 源 %2 FPS").arg(displayedTop, sourceRate);
        const double topReceived = report.value("fps_top_received").toDouble(report["fps_received"].toDouble());
        QString status = topReceived > 0 ? cadence : QString("等待 3DS");
        for (auto error : {report["socket_error"].toString(), report["model_error"].toString(),
                           report["motion_error"].toString(), recordError})
            if (!error.isEmpty()) {
                status = error;
                break;
            }
        const qint64 lastTop = report.value("last_top_us").toInteger(report["last_pair_us"].toInteger());
        bool hasFrame = lastTop > 0;
        bool frozen = hasFrame && now - lastTop > 1200000;
        const auto session = report["session"].toInteger();
        if (session != loadSession) {
            pictureLoad = motionLoad = {};
            loadSession = session;
        }
        const double liveFps = hasFrame && !frozen ? std::min(topReceived, double(o.fps)) : 0.;
        const bool slowPicture = pictureLoad.observe(now, report["restored_pairs"].toInteger(),
            report["restore_total_us"].toInteger(), report["enhancement_overwrites"].toInteger(),
            o.mode != "off" && !report["model_preparing"].toBool() ? liveFps : 0.);
        const bool slowMotion = motionLoad.observe(now, report["interpolated"].toInteger(),
            report["motion_work_total_us"].toInteger(), report["motion_deadline_skips"].toInteger(),
            o.motion != "off" && !report["motion_preparing"].toBool() ? liveFps : 0.);
        if (slowPicture != pictureBusy || slowMotion != motionBusy) {
            pictureBusy = slowPicture;
            motionBusy = slowMotion;
            publishEffects();
            const QString key = o.mode + "|" + o.motion + "|" + (pictureBusy ? "p" : "") + (motionBusy ? "m" : "");
            // A mode that cannot reach the source cadence flips the busy flag
            // every few frames; warn once per mode instead of on every flap.
            if ((pictureBusy || motionBusy) && (key != busyToastKey || now - lastBusyToastUs > 30000000)) {
                if (pictureBusy && usesFbcnn(o.mode)) {
                    notify(QString("%1 单帧约 %2 ms，当前约 %3 FPS；可换用快速档或把目标帧率降到 24")
                               .arg(labels.value(o.mode))
                               .arg(report.value("processing_ms").toDouble(), 0, 'f', 1)
                               .arg(report.value("fps_presented").toDouble(), 0, 'f', 1));
                } else {
                    notify("本机处理持续超时，可在画面与补帧设置中降低负载");
                }
                busyToastKey = key;
                lastBusyToastUs = now;
            }
            if (!pictureBusy && !motionBusy) busyToastKey.clear();
        }
        report["picture_over_budget"] = pictureBusy;
        report["motion_over_budget"] = motionBusy;
        QString notice, quality;
        if (!report["socket_error"].toString().isEmpty()) {
            notice = QString("无法监听 UDP %1\n%2\n在菜单中重新监听或更改端口")
                         .arg(o.port)
                         .arg(report["socket_error"].toString());
            status = "端口不可用";
        } else if (!hasFrame) {
            notice = waitingNotice();
            status = "等待连接";
        } else if (frozen) {
            notice = "视频已中断 · 保留最后一帧\n收到新上屏后自动恢复";
            status = "等待恢复";
        }
        if (o.mode != "off") {
            if (usesFbcnn(o.mode)) {
                if (!frozen && topReceived > 0)
                    fallbackWindows =
                        report["fps_source_presented"].toDouble() < topReceived * .85
                            ? fallbackWindows + 1
                            : 0;
            } else if (report["coverage_pct"].isDouble())
                fallbackWindows = report["coverage_pct"].toDouble() < 95 ? fallbackWindows + 1 : 0;
            if (!report["model_error"].toString().isEmpty())
                quality = usesFbcnn(o.mode) ? "增强失败 · 可切换原始" : "增强失败 · 原始";
            else if (report["model_preparing"].toBool())
                quality = im.isNull() ? "正在准备增强" : "正在准备增强 · 当前显示原图";
            else
                quality =
                    labels.value(o.mode) + (!usesFbcnn(o.mode) && fallbackWindows >= 3 ? " · 部分原始帧" : "");
            if (usesFbcnn(o.mode) && hasFrame && !frozen && !im.isNull()) {
                status = cadence;
                if (fallbackWindows >= 3)
                    quality += " · 保持增强画质";
            }
            if (usesFbcnn(o.mode) && im.isNull() && hasFrame && !frozen)
                notice = report["model_error"].toString().isEmpty() ? "等待首张 FBCNN 画面"
                                                                    : "FBCNN 未就绪 · 请在菜单中检查画质设置";
        }
        if (!recordError.isEmpty())
            status = "录像失败 · " + recordError;
        else if (!report["motion_error"].toString().isEmpty())
            status = "补帧失败 · " + report["motion_error"].toString();
        else if (now < toastUntil)
            status = toast;
        report["connection"] = !report["socket_error"].toString().isEmpty() ? "bind_error"
                               : !hasFrame                                  ? "waiting"
                               : frozen                                     ? "interrupted"
                                                                            : "live";
        report["frame_health"] = diagnoseFrameDrops(report, samples);
        latest = report;
        samples.append(report);
        while (!samples.isEmpty() && samples.first().toObject()["wall_us"].toInteger() <= now - 60000000)
            samples.removeFirst();
        if (log.isOpen()) {
            log.write(QJsonDocument(report).toJson(QJsonDocument::Compact) + "\n");
            log.flush();
        }
        updateView(view, {{"status", status},
                          {"detail", status},
                          {"notice", notice},
                          {"quality", quality},
                          {"debug", report},
                          {"history", samples},
                          {"duration", report["recording"].toObject()["seconds"]}});
    });
    timer.start(2);
    if (cli.isSet("seconds"))
        QTimer::singleShot(std::max(1, int(cli.value("seconds").toDouble() * 1000)), &app, &QApplication::quit);
    app.exec();
    timer.stop();
    if (recording)
        recording->stop();
    if (publish)
        publish->stop();
    auto finish = [&](std::unique_ptr<Recording> &rec) {
        if (!rec)
            return;
        for (;;) {
            auto info = rec->stats();
            auto state = info["state"].toString();
            if (state == "finished" || state == "failed") {
                if (state == "failed") {
                    exitCode = 1;
                    if (interactive)
                        QMessageBox::critical(nullptr, "保存或发布失败",
                                              info["file"].toString() + "\n" + info["error"].toString());
                }
                if (log.isOpen())
                    log.write(QJsonDocument(info).toJson(QJsonDocument::Compact) + "\n");
                break;
            }
            QThread::msleep(5);
        }
    };
    finish(recording);
    finish(publish);
    destroyView(view);
    return exitCode;
}
