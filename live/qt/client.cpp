#include "client.h"
#include "runtime_paths.h"
#include "viewer_native.h"
#include "receiver.h"
#include "recording.h"
#include "model_check.h"
#include <QtWidgets>
using namespace live;
static const QMap<QString, QString> labels = {{"off", "原始"},
                                              {"clean", "轻度增强"},
                                              {"fbcnn", "FBCNN 高画质"},
                                              {"fbcnn-detail", "1× 原生清晰 · FBCNN"},
                                              {"fast-detail", "1× 实时清晰 · DnCNN"},
                                              {"fbcnn-sr", "2× 修复超分 · FBCNN + Real-ESRGAN"},
                                              {"gpu", "DnCNN 降噪"},
                                              {"sr", "Real-ESRGAN 2× 超分"}};
struct Options {
    QString ip, mode = "off", motion = "off", rifePrecision = "fp32", record, rtsp, stats, models;
    int port = 8001, scale = 1, fps = 30, rate = 800000, delay = 100;
    bool mute = false, debug = false;
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
static bool validIp(const QString &ip) {
    return ip.isEmpty() || QHostAddress(ip).protocol() == QAbstractSocket::IPv4Protocol;
}
static bool validRtsp(const QString &url) {
    return url.isEmpty() || (QUrl(url).scheme() == "rtsp" && !QUrl(url).host().isEmpty());
}
int runClient(QApplication &app) {
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
    arg("enhance", "off, clean, fbcnn, fbcnn-detail or fast-detail (1x), fbcnn-sr (2x), gpu or sr", "off");
    arg("interpolate", "off, flow or rife", "off");
    arg("rife-precision", "RIFE inference precision: fp32 (default) or fp16");
    arg("scale", "Display scale 1 or 2", "1");
    arg("fps", "Source FPS", "30");
    arg("rate", "Sender byte/s ceiling", "800000");
    arg("delay", "Playout delay in ms (100 standard, 160 for FBCNN)", "100");
    arg("record", "Save MP4 on launch");
    arg("rtsp", "Publish to an RTSP server");
    arg("seconds", "Automatically close after this many seconds");
    arg("trace", "Write source, presentation and recording events as JSON lines");
    arg("stats", "Append diagnostic snapshots as JSON lines");
    arg("models", "ONNX model directory", live::runtimeDirectory().filePath("models"));
    arg("check-models", "Run comma-separated native model checks and write results to --stats");
    cli.addOption({"mute", "Mute speakers"});
    cli.addOption({"debug", "Open F12 diagnostics"});
    cli.addOption({"preview", "Open preview directly"});
    cli.process(app);
    Options o;
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
    o.fps = cli.value("fps").toInt();
    o.rate = cli.value("rate").toInt();
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
        o.fps = prefs.value("fps", 30).toInt();
        o.rate = prefs.value("rate", 800000).toInt();
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
        if (!QList<int>{15, 20, 24, 30}.contains(o.fps))
            o.fps = 30;
        if (!rates().contains(o.rate))
            o.rate = 800000;
    }
    if (!cli.isSet("scale"))
        o.scale = pictureScale(o.mode, o.scale);
    if (!cli.isSet("delay") && usesFbcnn(o.mode))
        o.delay = 160;
    if ((!o.ip.isEmpty() && QHostAddress(o.ip).protocol() != QAbstractSocket::IPv4Protocol) || o.port < 1 ||
        o.port > 65535 || !labels.contains(o.mode) || !QStringList{"off", "rife", "flow"}.contains(o.motion) ||
        (o.scale != 1 && o.scale != 2) || o.scale != pictureScale(o.mode, o.scale) ||
        !QList<int>{15, 20, 24, 30}.contains(o.fps) || !rates().contains(o.rate) || o.delay < 20 ||
        o.delay > 500 || (!o.record.isEmpty() && QFileInfo::exists(o.record)) ||
        (!o.rtsp.isEmpty() && QUrl(o.rtsp).scheme() != "rtsp"))
        return 2;
    QFile log(o.stats);
    if (!o.stats.isEmpty() && !log.open(QIODevice::WriteOnly | QIODevice::Append))
        return 2;
    Timeline timeline(o.delay);
    TraceWriter trace;
    if (cli.isSet("trace") && !trace.start(cli.value("trace"), timeline.diagnostics))
        return 2;
    Pipeline pipeline(timeline, o.models, o.scale, o.mode, o.motion, o.rifePrecision);
    Speaker speaker(timeline);
    speaker.mute = o.mute;
    std::unique_ptr<Receiver> receiver;
    auto listen = [&] {
        receiver.reset();
        receiver = std::make_unique<Receiver>(timeline, pipeline, QHostAddress(o.ip), o.port, o.fps, o.rate);
    };
    listen();
    QJsonObject modes;
    for (auto it = labels.begin(); it != labels.end(); ++it)
        modes[it.key()] = it.value();
    void *view = createView({{"title", "3DS Live"},
                             {"ip", o.ip},
                             {"scale", o.scale},
                             {"port", o.port},
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
    auto options = [&](int mask, int max) {
        QJsonArray caps, speeds;
        for (int r : rates())
            if (r <= max)
                caps.append(r);
        for (int f : {15, 20, 24, 30})
            if (mask & (1 << f))
                speeds.append(f);
        updateView(view, {{"fpsOptions", speeds}, {"rateOptions", caps}, {"fps", o.fps}, {"rate", o.rate}});
    };
    int mask = (1 << 15) | (1 << 20) | (1 << 24) | (1 << 30), max = 2000000;
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
    qint64 lastPts = std::numeric_limits<qint64>::min(), presented = 0, onTime = 0, intermediate = 0,
           lastPresentWall = 0, largestGap = 0;
    qint64 lastSourceWall = 0, sourceBucketGap = 0;
    quint32 lastSourceSession = 0;
    std::optional<SourceFrame> lastRealBottom;
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
        prefs.setValue("mode", o.mode);
        prefs.setValue("motion", o.motion);
        prefs.setValue("scale", o.scale);
        prefs.setValue("muted", o.mute);
    };
    auto notify = [&](const QString &text) {
        toast = text;
        toastUntil = clockUs() + 5000000;
    };
    auto configurePicture = [&] {
        if (o.motion != "off") lastMotion = o.motion;
        o.scale = pictureScale(o.mode, o.scale);
        pipeline.configure(o.mode, o.motion, o.scale, o.rifePrecision);
        updateView(view, {{"mode", o.mode}, {"scale", o.scale}, {"motion", o.motion}, {"rife_precision", o.rifePrecision}});
        fallbackWindows = 0;
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
                const int hz = e["fps"].toInt(o.fps), cap = e["rate"].toInt(o.rate);
                if (e.contains("fps") && (hz < 0 || hz > 30 || !(mask & (1 << hz))))
                    continue;
                if (e.contains("rate") && (!rates().contains(cap) || cap > max))
                    continue;
                if (hz != o.fps || cap != o.rate) {
                    o.fps = hz;
                    o.rate = cap;
                    receiver->configure(hz, cap);
                    remember();
                }
                updateView(view, {{"fps", hz}, {"rate", cap}});
            }
            if (type == "apply") {
                QString ip = e["ip"].toString(), url = e["rtsp"].toString();
                if (!validIp(ip) || !validRtsp(url)) {
                    notify("请检查掌机 IPv4 或 RTSP 地址");
                    continue;
                }
                bool changed = ip != o.ip || e["port"].toInt() != o.port;
                const bool limitsChanged = e["fps"].toInt() != o.fps || e["rate"].toInt() != o.rate;
                o.ip = ip;
                o.port = e["port"].toInt();
                o.fps = e["fps"].toInt();
                o.rate = e["rate"].toInt();
                if (changed)
                    listen();
                else if (limitsChanged)
                    receiver->configure(o.fps, o.rate);
                updateView(view, {{"fps", o.fps}, {"rate", o.rate}});
                int scale = e["scale"].toInt();
                QString motion = e["motion"].toString();
                QString precision = e["rife_precision"].toString(o.rifePrecision);
                // A legacy directml override requests FP32 without TensorRT.
                if (precision == "fp32" && o.rifePrecision == "directml")
                    precision = "directml";
                if (scale != o.scale || motion != o.motion || precision != o.rifePrecision) {
                    if (interactive && precision != o.rifePrecision)
                        prefs.setValue("rifePrecision", precision);
                    o.rifePrecision = precision;
                    o.scale = scale;
                    o.motion = motion;
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
            if (type == "mute") {
                o.mute = !o.mute;
                speaker.mute = o.mute;
                remember();
                updateView(view, {{"muted", o.mute}});
            }
            if (type == "mode" || type == "motion" || type == "enhance") {
                if (type == "mode")
                    o.mode = e["value"].toString();
                if (type == "enhance") {
                    if (o.mode == "off")
                        o.mode = lastMode;
                    else {
                        lastMode = o.mode;
                        o.mode = "off";
                    }
                }
                if (type == "motion") {
                    const QString method = e.value("value").toString(o.motion == "off" ? lastMotion : QStringLiteral("off"));
                    if (method == o.motion || (method != "off" && method != "flow" && method != "rife")) continue;
                    o.motion = method;
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
            for (int i = 0; i < 2; ++i)
                if (choice.updatedPanels & (1 << i))
                    panelCadence[i].select(choice.source.session, choice.time);
            bottomGenerated += bool(choice.generatedPanels & 2);
            if ((choice.updatedPanels & 2) && choice.source.panels[1]) {
                lastRealBottom = choice.source;
                lastRealBottom->pts = choice.source.panels[1]->pts;
            }
            const bool realTop = choice.updatedPanels & 1;
            if (realTop || synthetic) ++presented;
            if (realTop) {
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
            showFrame(view, im);
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
        for (auto it = processing.begin(); it != processing.end(); ++it)
            report[it.key()] = it.value();
        const auto paintStats = takePaintStats(view);
        for (auto it = paintStats.begin(); it != paintStats.end(); ++it)
            report[it.key()] = it.value();
        int delta = int(presented - previousCount), sourceDelta = delta - int(intermediate - previousSynthetic);
        report["time"] = QTime::currentTime().toString("HH:mm:ss");
        report["wall_us"] = now;
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
        if (lastRealBottom && report["session"].toInteger() == lastRealBottom->session)
            report["bottom_source_age_ms"] =
                std::max<qint64>(0, now - timeline.deadline(*lastRealBottom) + timeline.delay) / 1000.;
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
        report["presented_changes"] = painted;
        report["max_visual_change_gap_ms"] = maxPaint / 1000.;
        report["playout_delay_ms"] = o.delay;
        report["presented_frames"] = presented;
        report["enhanced_at_presentation"] = onTime;
        report["synthetic_at_presentation"] = intermediate;
        report["max_presentation_gap_ms"] = largestGap / 1000.;
        report["audio_device"] = speaker.opened.load();
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
        QString notice, quality;
        if (!report["socket_error"].toString().isEmpty()) {
            notice = QString("无法监听 UDP %1\n%2\n在菜单中重新监听或更改端口")
                         .arg(o.port)
                         .arg(report["socket_error"].toString());
            status = "端口不可用";
        } else if (!hasFrame) {
            notice = (o.ip.isEmpty() ? QString("正在发现同一局域网内的 3DS") : QString("等待掌机 %1").arg(o.ip))
                     + QString(" · UDP %1\n在掌机上按 R + SELECT").arg(o.port);
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
                quality = "正在准备增强";
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
