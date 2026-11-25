#include "viewer_native.h"
#include "debug_window.h"
#include "stream_presets.h"
#include "live_fps.h"
#include <QtWidgets>
#include <algorithm>
#include <utility>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <imm.h>
#endif
namespace {
class Canvas final : public QWidget {
    QSize sourceSize;
    QImage image;
    QString screenLayout = "stacked";
    QString targetLayout = "stacked";
    QActionGroup *layoutGroup = nullptr;
    QWidget *bottomWindow = nullptr;
    Canvas *bottomCanvas = nullptr;
    std::function<void()> onPainted;
    QElapsedTimer submitClock;
    qint64 paintedChanges = 0, coalesced = 0, maxPaintNs = 0;

  public:
    QLabel *notice;
    explicit Canvas(QSize size, QWidget *parent) : QWidget(parent), sourceSize(size) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        notice = new QLabel(this);
        notice->setAlignment(Qt::AlignCenter);
        notice->setWordWrap(true);
        notice->setAttribute(Qt::WA_TransparentForMouseEvents);
        notice->setStyleSheet(
            "QLabel { background: rgba(15,18,23,210); color: white; padding: 8px; border-radius: 4px; }");
        notice->hide();
    }
    QSize sizeHint() const override { return sourceSize; }
    void setLayoutMode(const QString &mode) { screenLayout = mode; update(); }
    void setSourceSize(QSize size) {
        sourceSize = size;
        updateGeometry();
    }
    void setImage(const QImage &frame, std::function<void()> painted) {
        if (submitClock.isValid())
            ++coalesced;
        submitClock.start();
        image = frame;
        onPainted = std::move(painted);
        update();
    }
    QJsonObject paintStats() {
        return {{"canvas_painted_changes", paintedChanges},
                {"canvas_visible", isVisible() && !window()->isMinimized()},
                {"canvas_coalesced_updates", coalesced},
                {"canvas_interval_max_submit_to_paint_ms", std::exchange(maxPaintNs, qint64(0)) / 1e6}};
    }
    void message(const QString &s) {
        if (s == notice->text())
            return;
        notice->setText(s);
        notice->setVisible(!s.isEmpty());
        position();
    }
    void position() {
        int w = std::max(1, std::min(width() - 24, 520));
        notice->setFixedWidth(w);
        notice->adjustSize();
        notice->move((width() - w) / 2, (height() - notice->height()) / 2);
    }

  protected:
    void resizeEvent(QResizeEvent *) override { position(); }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        if (image.isNull()) {
            p.fillRect(rect(), Qt::black);
            submitClock.invalidate();
            onPainted = {};
            return;
        }
        QImage shown = image;
        if (screenLayout == "top-only" && image.height() > 1)
            shown = image.copy(0, 0, image.width(), image.height() / 2);
        else if (screenLayout == "bottom-only" && image.height() > 1)
            shown = image.copy(0, image.height() / 2, image.width(), image.height() - image.height() / 2);
        else if (screenLayout == "side-by-side" && image.height() > 1) {
            const int half = image.height() / 2;
            const int scale = std::max(1, image.width() / 400);
            shown = QImage(720 * scale, 240 * scale, QImage::Format_RGB888);
            shown.fill(Qt::black);
            QPainter composer(&shown);
            composer.drawImage(QRect(0, 0, 400 * scale, 240 * scale),
                               image, QRect(0, 0, 400, half));
            composer.drawImage(QRect(400 * scale, 0, 320 * scale, 240 * scale),
                               image, QRect(0, half, 320, half));
        }
        QSize sz = shown.size().scaled(size(), Qt::KeepAspectRatio);
        QRect target(QPoint((width() - sz.width()) / 2, (height() - sz.height()) / 2), sz);
        if (target != rect())
            p.fillRect(rect(), Qt::black);
        p.setRenderHint(QPainter::SmoothPixmapTransform, sz != shown.size());
        p.drawImage(target, shown);
        if (submitClock.isValid()) {
            ++paintedChanges;
            maxPaintNs = std::max(maxPaintNs, submitClock.nsecsElapsed());
            submitClock.invalidate();
            if (onPainted) {
                p.end();
                auto notify = std::move(onPainted);
                notify();
            }
        }
    }
};
class StatusLabel final : public QLabel {
  public:
    using QLabel::QLabel;
  protected:
    void paintEvent(QPaintEvent *event) override {
        const auto line = fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width());
        if (line == text()) {
            QLabel::paintEvent(event);
            return;
        }
        QPainter painter(this);
        style()->drawItemText(&painter, contentsRect(), alignment(), palette(), isEnabled(), line, foregroundRole());
    }
};
class RecordButton final : public QToolButton {
  public:
    bool active = false;
    explicit RecordButton(QWidget *p) : QToolButton(p) {
        setObjectName("record");
        setFixedSize(28, 26);
        setAutoRaise(true);
    }

  protected:
    void paintEvent(QPaintEvent *e) override {
        QToolButton::paintEvent(e);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(active ? QColor("#df3543") : QColor("#777d85"));
        p.drawEllipse(rect().center(), 6, 6);
    }
};
class Viewer final : public QWidget {
    Canvas *canvas;
    QWidget *panel;
    QHBoxLayout *mainBar;
    QLabel *status, *elapsed, *quality;
    QToolButton *quickFps = nullptr, *quickRate = nullptr, *quickJpeg = nullptr, *generation = nullptr, *performance = nullptr;
    QLabel *performanceCard = nullptr;
    QToolButton *restore = nullptr, *more = nullptr;
    RecordButton *record;
    QMenu *menu;
    QAction *jpegMenuStatus = nullptr;
    QAction *rateStatus = nullptr;
    QActionGroup *rateModes = nullptr;
    QLabel *jpegSettingsStatus = nullptr;
    QLabel *fpsFallback = nullptr;
    int targetFps = 0, targetJpeg = 0, targetRate = 0;
    bool targetAdaptive = true;
    QAction *recordAction, *mute = nullptr, *openFile, *openFolder;
    QDialog *settings;
    QTabWidget *settingsTabs;
    QComboBox *fps, *rate, *rateMode, *jpegQuality, *layoutChoice, *motion, *scale, *rifePrecision = nullptr;
    QString screenLayout = "stacked";
    QString targetLayout = "stacked";
    QActionGroup *layoutGroup = nullptr;
    QWidget *bottomWindow = nullptr;
    Canvas *bottomCanvas = nullptr;
    QLineEdit *ip, *rtsp;
    QSpinBox *port;
    struct EffectChoice {
        QString label, tip;
        QAction *action = nullptr;
        QStandardItem *item = nullptr;
    };
    QMap<QString, EffectChoice> pictures, motionChoices;
    QJsonObject effects;
    QLabel *effectMessage;
    QDialogButtonBox *effectButtons;
    QPushButton *reduceEffects, *retryEffects;
    QAction *effectSettings = nullptr;
    QPointer<QFileDialog> saveDialog;
    live::DebugWindow *debug = nullptr;
    bool activateAfterFirstPaint = false;
    QJsonObject snapshot;
    QJsonArray history, queue;
    QString lastFile;
    QString jpegStatus() const {
        const int target = snapshot.value("requested_jpeg_quality").toInt();
        if (target == 70 && !snapshot.value("jpeg_q70_supported").toBool()) {
            QString note = "已选 Q70，等待掌机报告 Q70 支持";
            if (snapshot.value("connection") == "live" && snapshot.contains("device_jpeg_quality")) {
                const int actual = snapshot.value("device_jpeg_quality").toInt();
                note += actual ? QString("，掌机当前目标 Q%1").arg(actual) : QStringLiteral("，掌机当前为自动画质");
            }
            return note;
        }
        if (target == 0 || snapshot.value("connection").toString() != "live")
            return {};
        if (snapshot.value("jpeg_quality_confirmed").toBool())
            return QString("掌机已采用 Q%1 目标").arg(target);
        if (snapshot.contains("device_jpeg_quality"))
            return QString("Q%1 目标等待掌机确认").arg(target);
        return QString("Q%1 目标未确认，需支持此档位的掌机端").arg(target);
    }
    QToolButton *quickControl(QHBoxLayout *bar, const QString &key, const QString &title) {
        auto *button = new QToolButton(panel);
        button->setObjectName("quick" + key);
        button->setAccessibleName(title);
        button->setToolTip(title);
        button->setPopupMode(QToolButton::InstantPopup);
        button->setAutoRaise(true);
        button->setMenu(new QMenu(title, button));
        bar->addWidget(button);
        return button;
    }
    void fillQuick(QToolButton *button, QComboBox *combo, const QString &field) {
        auto *choices = button->menu();
        // Keep the rate policy actions while rebuilding device-supported numeric choices.
        for (auto *a : choices->actions())
            if (a->data().isValid()) delete a;
        for (int i = 0; i < combo->count(); ++i) {
            auto *a = choices->addAction(combo->itemText(i));
            a->setData(combo->itemData(i));
            a->setCheckable(true);
            connect(a, &QAction::triggered, this, [this, a, field] {
                queue.append(QJsonObject{{"type", "limit"}, {field, a->data().toInt()}});
            });
        }
    }
    void selectQuick(QToolButton *button, const QString &label, const QString &text, int value) {
        button->setText(text);
        button->menu()->setTitle(label + QStringLiteral("　") + text);
        for (auto *a : button->menu()->actions())
            if (a->data().isValid()) a->setChecked(a->data().toInt() == value);
    }
    void selectJpeg(int target) {
        targetJpeg = target;
        jpegQuality->setCurrentIndex(jpegQuality->findData(target));
        updateJpegLabel();
    }
    void updateJpegLabel() {
        QString label = targetJpeg ? QString("Q%1").arg(targetJpeg) : QStringLiteral("自动Q");
        const bool current = snapshot.value("connection") == "live" &&
                             snapshot.value("requested_jpeg_quality").toInt() == targetJpeg;
        if (targetJpeg == 70 && !(current && snapshot.value("jpeg_quality_confirmed").toBool()))
            label += QStringLiteral(" 待确认");
        selectQuick(quickJpeg, "掌机 JPEG 画质", label, targetJpeg);
        const auto note = current ? jpegStatus() : QString();
        quickJpeg->setToolTip(note.isEmpty() ? QStringLiteral("修改掌机 JPEG 画质") : note);
    }
    void updateFpsLabel() {
        if (targetFps <= 0) return;
        const auto effective = snapshot.value("effective_fps").toInt();
        const bool fallback = snapshot.value("connection") == "live" &&
            snapshot.value("requested_fps").toInt() == targetFps && effective > 0 && effective != targetFps &&
            !(snapshot.value("fps_mask").toInt() & live_fps_flag(targetFps));
        const QString value = fallback ? QString("%1→%2 FPS").arg(targetFps).arg(effective) : QString("%1 FPS").arg(targetFps);
        const QString note = fallback ? QString("掌机不支持目标 %1 FPS，当前向掌机请求 %2 FPS。").arg(targetFps).arg(effective) : QString();
        selectQuick(quickFps, "目标帧率", value, targetFps);
        quickFps->setToolTip(fallback ? note : QStringLiteral("修改掌机目标帧率"));
        fpsFallback->setText(note);
        fpsFallback->setVisible(fallback);
    }
    static void showRequestedLimit(QComboBox *combo, int value, const QString &label) {
        auto *model = static_cast<QStandardItemModel *>(combo->model());
        // A previous request can exceed a newly connected device's capabilities.
        // Preserve it when edits to other settings are applied.
        const int last = combo->count() - 1;
        if (last >= 0 && !model->item(last)->isEnabled() && combo->itemData(last).toInt() != value)
            combo->removeItem(last);
        int index = combo->findData(value);
        if (index < 0) {
            combo->addItem(label + QStringLiteral("（当前掌机不支持）"), value);
            index = combo->count() - 1;
            model->item(index)->setEnabled(false);
        }
        combo->setCurrentIndex(index);
    }
    void addStreamPresets(QMenu *choices, bool applyNow) {
        auto *group = new QActionGroup(choices);
        for (const auto preset : live::streamPresets) {
            const QString label = QString("%1 · %2 FPS · Q%3 · %4 MB/s")
                .arg(QString::fromUtf8(preset.name)).arg(preset.fps).arg(preset.jpegQuality)
                .arg(preset.rate / 1e6, 0, 'g', 2);
            auto *action = choices->addAction(label);
            action->setData(preset.id);
            action->setCheckable(true);
            group->addAction(action);
            const auto available = [this, preset] {
                auto supported = [](QComboBox *combo, int value) {
                    const int index = combo->findData(value);
                    return index >= 0 && combo->model()->flags(combo->model()->index(index,0)).testFlag(Qt::ItemIsEnabled);
                };
                return supported(fps, preset.fps) && supported(rate, preset.rate);
            };
            connect(choices, &QMenu::aboutToShow, this, [this, action, preset, applyNow, available] {
                action->setEnabled(available());
                const bool matches = applyNow
                    ? targetFps == preset.fps && targetJpeg == preset.jpegQuality && targetRate == preset.rate && !targetAdaptive
                    : fps->currentData().toInt() == preset.fps && jpegQuality->currentData().toInt() == preset.jpegQuality &&
                      rate->currentData().toInt() == preset.rate && !rateMode->currentData().toBool();
                action->setChecked(matches);
            });
            connect(action, &QAction::triggered, this, [this, preset, applyNow, available] {
                if (!available()) return;
                if (applyNow) {
                    queue.append(QJsonObject{{"type","limit"}, {"fps",preset.fps}, {"jpegQuality",preset.jpegQuality},
                                             {"rate",preset.rate}, {"adaptiveRate",false}});
                    return;
                }
                fps->setCurrentIndex(fps->findData(preset.fps));
                rate->setCurrentIndex(rate->findData(preset.rate));
                jpegQuality->setCurrentIndex(jpegQuality->findData(preset.jpegQuality));
                rateMode->setCurrentIndex(rateMode->findData(false));
            });
        }
    }
    void generationLabel(const QString &method) {
        QString text = method == "flow" ? "补帧 DIS" : method == "rife" ? "补帧 RIFE" : "补帧 关";
        generation->setText(text);
        generation->setAccessibleName(generation->text());
        generation->setToolTip(text + QStringLiteral(" · F 切换运动补帧"));
    }
    QString number(const QString &key, int decimals = 0, double divisor = 1.) const {
        if (snapshot.contains("rx_sample_us") &&
            (key == "device_sent_fps" || key == "device_cap" || key.startsWith("captured_")) &&
            snapshot.value("wall_us").toInteger() - snapshot.value("device_status_us").toInteger() >= 2500000)
            return QStringLiteral("—");
        const auto v = snapshot.value(key);
        return v.isDouble() ? QString::number(v.toDouble() / divisor, 'f', decimals) : QStringLiteral("—");
    }
    void refreshCard(bool showHint = true) {
        if (!performanceCard || performanceCard->isHidden())
            return;
        auto esc = [](const QString &text) { return text.toHtmlEscaped(); };
        auto number = [this](const QString &key, int decimals = 0, double divisor = 1.) {
            return this->number(key, decimals, divisor);
        };
        QString html =
            "<style>"
            "table{border-collapse:collapse;}"
            "td{padding:1px 0;font-size:12px;}"
            ".title{color:#ffffff;font-size:13px;font-weight:600;padding-bottom:4px;}"
            ".sec{color:#7f93a8;font-size:10px;letter-spacing:1px;padding-top:8px;padding-bottom:2px;}"
            ".k{color:#aeb9c7;padding-right:14px;}"
            ".v{color:#ffffff;font-family:Consolas,'Cascadia Mono',monospace;}"
            ".note{color:#c7d0da;font-size:11px;padding-top:3px;}"
            ".hint{color:#7f8b99;font-size:10px;padding-top:8px;}"
            "</style><table width='100%' cellspacing='0' cellpadding='0'>";
        auto section = [&](const QString &title) { html += "<tr><td colspan='2' class='sec'>" + esc(title) + "</td></tr>"; };
        auto row = [&](const QString &key, const QString &value) {
            html += "<tr><td class='k'>" + esc(key) + "</td><td class='v' align='right'>" + esc(value) + "</td></tr>";
        };
        auto note = [&](const QString &text) {
            html += "<tr><td colspan='2' class='note'>" + esc(text) + "</td></tr>";
        };
        html += "<tr><td colspan='2' class='title'>直播性能 · " + esc(snapshot["time"].toString()) + "</td></tr>";
        const auto health = snapshot.value("frame_health").toObject();
        if (!health.isEmpty()) {
            section(snapshot.value("connection") == "interrupted" ? QStringLiteral("收帧状态") : QStringLiteral("掉帧观察"));
            for (const auto &line : health.value("evidence").toArray())
                note(line.toString());
        }
        const auto method = snapshot["motion"].toString();
        const QString motionName = method == "flow" ? "DIS 光流" : method == "rife" ? "RIFE AI" : "关闭";
        if (snapshot["connection"].toString() == "live") {
            section("帧率");
            row("源帧 / 生成 / 合计", QString("%1 / %2 / %3 FPS")
                    .arg(number("fps_source_presented", 1), number("fps_generated", 1), number("fps_presented", 1)));
            row("掌机发送 / 目标", QString("%1 / %2 对/s")
                    .arg(number("device_sent_fps", 1), number("requested_fps")));
            row("上屏 / 下屏发送", QString("%1 / %2 FPS")
                    .arg(number("device_sent_top_fps", 1), number("device_sent_bottom_fps", 1)));
            row("双屏收齐", number("fps_received", 1) + " FPS");
            row("运动补帧 / 缓冲", QString("%1 / %2 ms").arg(motionName, number("playout_delay_ms")));
            section("编码");
            row("上屏 / 下屏编码", QString("%1 / %2 FPS")
                    .arg(number("device_encoded_top_fps", 1), number("device_encoded_bottom_fps", 1)));
            row("每对字节", number("pair_bytes", 1, 1024.) + " KB");
            row("单帧编码耗时", QString("%1 / %2 ms")
                    .arg(number("top_encode_us", 1, 1000.), number("bottom_encode_us", 1, 1000.)));
            row("上 / 下屏 JPEG Q", number("jpeg_top_q") + " / " + number("jpeg_bottom_q"));
            row("增强", QString("%1 · %2 · %3 ms")
                    .arg(quality->text(), snapshot["backend"].toString(), number("processing_ms", 1)));
            section("网络");
            row(snapshot.value("adaptive_rate").toBool(true) ? "自适应上限" : "固定上限",
                number("requested_rate", 2, 1e6) + " MB/s");
            row("掌机提交 / PC 实收", QString("%1 / %2 MB/s")
                    .arg(number("device_media_bytes_per_s", 2, 1e6), number("bytes_per_s", 2, 1e6)));
            row("掌机当前上限", number("device_cap", 2, 1e6) + " MB/s");
            row("本秒最长源帧间隔", number("source_interval_max_gap_ms", 1) + " ms");
            if (snapshot.contains("jpeg_header_saving_pct"))
                row("JPEG 表头累计节省", QString("%1 MB (%2%)")
                        .arg(number("jpeg_header_saved_bytes", 1, 1000000.), number("jpeg_header_saving_pct", 1)));
            section("呈现");
            row("真实更新 上 / 下", QString("%1 / %2 FPS")
                    .arg(number("fps_top_presented", 1), number("fps_bottom_presented", 1)));
            row("上屏 >100 ms", QString("%1 次 · 最长 %2 ms")
                    .arg(number("top_gaps_over_100ms"), number("top_max_gap_ms", 1)));
            row("下屏 >100 ms", QString("%1 次 · 最长 %2 ms")
                    .arg(number("bottom_gaps_over_100ms"), number("bottom_max_gap_ms", 1)));
            if (snapshot.contains("captured_top_fps"))
                row("掌机采集 上 / 下", QString("%1 / %2 FPS")
                        .arg(number("captured_top_fps", 1), number("captured_bottom_fps", 1)));
            if (method == "flow")
                row("光流保护跳过", number("motion_quality_holds") + " 对");
            const auto jpeg = jpegStatus();
            if (!jpeg.isEmpty())
                note(jpeg);
        } else {
            note(status->text());
        }
        if (method == "rife") {
            section("补帧");
            const QString requested = snapshot["rife_precision"].toString() == "fp16" ? "FP16" : "FP32";
            QString backend = snapshot["motion_backend"].toString();
            if (backend == "DirectML") backend = QStringLiteral("DirectML FP32");
            QString state = QStringLiteral("等待推理");
            if (!snapshot["motion_error"].toString().isEmpty())
                state = QStringLiteral("推理失败 · F12 查看原因");
            else if (!backend.isEmpty())
                state = QString("%1 · %2 ms").arg(backend, number("motion_processing_ms", 1));
            row("RIFE 设置", requested);
            row("RIFE 状态", state);
            if (!backend.isEmpty() && !snapshot["motion_backend_fallback"].toString().isEmpty())
                note("RIFE 已回退 · F12 查看原因");
        }
        if (showHint) {
            html += "<tr><td colspan='2' class='hint'>" +
                    esc(effectSettings && effectSettings->isVisible()
                            ? QStringLiteral("右键性能按钮打开「画面与补帧」或复制截图")
                            : QStringLiteral("点击性能按钮固定 · 右键复制截图")) +
                    "</td></tr>";
        }
        html += "</table>";
        performanceCard->setTextFormat(Qt::RichText);
        performanceCard->setText(html);
        performanceCard->setWordWrap(true);
        const int width = std::min(340, std::max(1, canvas->width() - 12));
        performanceCard->setFixedWidth(width);
        int height = performanceCard->heightForWidth(width);
        if (height <= 0) height = performanceCard->sizeHint().height();
        height = std::min(height, std::max(1, canvas->height() - 12));
        performanceCard->setFixedSize(width, height);
        performanceCard->move(canvas->width() - performanceCard->width() - 6,
                              std::max(0, canvas->height() - performanceCard->height() - 6));
        performanceCard->raise();
    }

    void showCard(bool visible) {
        performanceCard->setVisible(visible);
        refreshCard();
    }
    void arrangeBar(int width) {
        if (!more) return;
        const int gap = mainBar->spacing();
        const auto margins = mainBar->contentsMargins();
        // Reserve the full two-counter readout even while waiting for a stream.
        const int cadenceWidth = status->fontMetrics().horizontalAdvance(QStringLiteral("显示 60.0 · 源 30.0 FPS"));
        int free = width - margins.left() - margins.right() - cadenceWidth
                   - performance->sizeHint().width() - record->width() - more->sizeHint().width() - 3 * gap;
        elapsed->setVisible(!elapsed->text().isEmpty());
        if (!elapsed->isHidden()) free -= elapsed->sizeHint().width() + gap;
        auto fit = [&](QWidget *widget) {
            const int needed = widget->sizeHint().width() + gap;
            const bool visible = needed <= free;
            widget->setVisible(visible);
            if (visible) free -= needed;
        };
        fit(generation);
        fit(quickFps);
        fit(quickJpeg);
        fit(quickRate);
        if (quality->text().isEmpty()) quality->hide();
        else fit(quality);
    }
    void emitAction(QString type) { queue.append(QJsonObject{{"type", type}}); }
    void controls(bool visible) {
        panel->setVisible(visible);
        restore->setVisible(!visible);
        arrangeBar(width());
        queue.append(QJsonObject{{"type", "controls"}, {"visible", visible}});
    }
    void fullscreen() { isFullScreen() ? showNormal() : showFullScreen(); }
    void setRecording(const QString &state, const QString &path, const QString &error) {
        record->active = state == "recording";
        bool busy = state == "saving" || state == "starting";
        record->setEnabled(!busy);
        recordAction->setEnabled(!busy);
        QString text = state == "saving"     ? "正在保存录像"
                       : state == "starting" ? "正在开始录像"
                       : record->active      ? "停止录像"
                                             : "开始录像";
        record->setToolTip(path.isEmpty() ? text : text + "\n" + path);
        record->setAccessibleName(text);
        recordAction->setText(text);
        record->update();
        if (!record->active)
            elapsed->setText(busy ? text : QString());
        if (state == "finished") {
            lastFile = path;
            openFile->setEnabled(true);
            openFolder->setEnabled(true);
            status->setText("录像已保存");
        }
        if (state == "failed") {
            status->setText("录像失败");
            status->setToolTip(error);
        }
    }
    void toggleRecording() {
        if (record->active) {
            emitAction("stop-record");
            return;
        }
        if (saveDialog) {
            saveDialog->raise();
            return;
        }
        QSettings prefs;
        QString dir =
            prefs.value("recordDirectory", QStandardPaths::writableLocation(QStandardPaths::MoviesLocation))
                .toString();
        QString file =
            QDir(dir).filePath("3DS-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".mp4");
        saveDialog = new QFileDialog(this, "保存录像", file, "MP4 视频 (*.mp4)");
        auto d = saveDialog.data();
        d->setAcceptMode(QFileDialog::AcceptSave);
        d->setFileMode(QFileDialog::AnyFile);
        d->setDefaultSuffix("mp4");
        d->setOption(QFileDialog::DontUseNativeDialog);
        d->setAttribute(Qt::WA_DeleteOnClose);
        connect(d, &QFileDialog::fileSelected, this, [this](const QString &path) {
            QSettings().setValue("recordDirectory", QFileInfo(path).absolutePath());
            queue.append(QJsonObject{{"type", "record"}, {"path", path}, {"replace", QFileInfo::exists(path)}});
        });
        d->open();
    }
    void showDebug() {
        if (!debug)
            debug = new live::DebugWindow(this, capture);
        debug->show();
        debug->receive(snapshot, history);
        debug->raise();
    }
    enum SettingsTab { StreamTab, PictureTab, ConnectionTab };
    void openSettings(SettingsTab tab) {
        settingsTabs->setCurrentIndex(tab);
        settings->show();
        settings->raise();
        settings->activateWindow();
    }
    void openEffectSettings() {
        openSettings(PictureTab);
    }
    void updatePrecisionControl() {
        const bool retryRife = effects.value("blocked_motion").toObject().contains("rife");
        rifePrecision->setEnabled(retryRife || motion->currentData().toString() == "rife");
        rifePrecision->setToolTip(retryRife ? QStringLiteral("更换精度后点「应用」可重新检测 RIFE。")
                                             : QStringLiteral("选择 RIFE 的推理精度。应用后可悬停 ⓘ 查看实际后端与耗时。"));
    }
    void updateEffects(const QJsonObject &next) {
        if (effects == next) return;
        auto updateChoices = [&](const QMap<QString, EffectChoice> &choices, const QString &key) {
            const auto oldBlocked = effects.value(key).toObject();
            const auto blocked = next.value(key).toObject();
            for (auto it = choices.cbegin(); it != choices.cend(); ++it) {
                const QString reason = blocked.value(it.key()).toString();
                if (oldBlocked.value(it.key()).toString() == reason) continue;
                const auto &choice = it.value();
                const bool enabled = reason.isEmpty();
                const QString text = enabled ? choice.label : choice.label + QStringLiteral("（不可用）");
                if (choice.action) {
                    choice.action->setText(enabled ? text : text + QStringLiteral(" · ") + reason);
                    choice.action->setToolTip(enabled ? choice.tip : reason);
                    choice.action->setEnabled(enabled);
                }
                if (choice.item) {
                    choice.item->setText(text);
                    choice.item->setToolTip(reason);
                    choice.item->setData(reason, Qt::AccessibleDescriptionRole);
                    choice.item->setEnabled(enabled);
                }
            }
        };
        updateChoices(pictures, "blocked_picture");
        updateChoices(motionChoices, "blocked_motion");
        // Discard an unapplied choice if the running session just ruled it out.
        const auto blockedMotion = next.value("blocked_motion").toObject();
        if (blockedMotion.contains(motion->currentData().toString())) {
            QString fallback = "off";
            for (auto it = motionChoices.cbegin(); it != motionChoices.cend(); ++it)
                if (it->action && it->action->isChecked() && it->action->isEnabled()) fallback = it.key();
            motion->setCurrentIndex(motion->findData(fallback));
        }
        QString message = next.value("message").toString();
        if (message.isEmpty()) {
            QStringList reasons;
            for (const auto &key : {"blocked_picture", "blocked_motion"}) {
                const auto blocked = next.value(key).toObject();
                for (auto it = blocked.begin(); it != blocked.end(); ++it)
                    if (!it.value().toString().isEmpty()) reasons << it.value().toString();
            }
            reasons.removeDuplicates();
            message = reasons.join('\n');
        }
        if (blockedMotion.contains("rife"))
            message += QStringLiteral("\n更换 RIFE 推理精度后，点「应用」可重新检测。");
        effectMessage->setText(message);
        effectMessage->setVisible(!message.isEmpty());
        const bool reduce = next.value("can_reduce").toBool();
        const bool retry = next.value("can_retry").toBool();
        reduceEffects->setVisible(reduce);
        retryEffects->setVisible(retry);
        effectButtons->setVisible(reduce || retry);
        effectSettings->setVisible(!message.isEmpty() || reduce || retry);
        effects = next;
        updatePrecisionControl();
        refreshCard();
    }
    void makeSettings(const QJsonObject &o) {
        settings = new QDialog(this);
        settings->setWindowTitle("投屏设置");
        settings->setObjectName("streamSettings");
        auto *layout = new QVBoxLayout(settings);
        settingsTabs = new QTabWidget(settings);
        settingsTabs->setObjectName("streamSettingsTabs");
        layout->addWidget(settingsTabs);
        auto *streamPage = new QWidget(settingsTabs);
        auto *picturePage = new QWidget(settingsTabs);
        auto *connectionPage = new QWidget(settingsTabs);
        settingsTabs->addTab(streamPage, "掌机推流");
        settingsTabs->addTab(picturePage, "显示与补帧");
        settingsTabs->addTab(connectionPage, "连接与输出");
        auto *stream = new QFormLayout(streamPage);
        auto *connection = new QFormLayout(connectionPage);
        auto *picture = new QFormLayout(picturePage);
        for (auto *form : {stream, picture, connection}) {
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            form->setVerticalSpacing(8);
        }
        settings->setMinimumWidth(340);
        ip = new QLineEdit(o["ip"].toString(), settings);
        ip->setObjectName("senderIp");
        ip->setPlaceholderText("留空自动发现");
        connection->addRow("掌机 IPv4", ip);
        port = new QSpinBox(settings);
        port->setRange(1, 65535);
        port->setValue(o["port"].toInt(8001));
        port->setObjectName("listenPort");
        connection->addRow("监听端口", port);
        fps = new QComboBox(settings);
        rate = new QComboBox(settings);
        fps->setObjectName("sourceFps");
        rate->setObjectName("sourceRate");
        auto *presets = new QToolButton(streamPage);
        presets->setObjectName("streamPresets");
        presets->setText("选择固定上限组合…");
        presets->setToolTip("填入掌机目标帧率、双屏 JPEG 画质和固定带宽上限。可以逐项修改后再应用。");
        presets->setPopupMode(QToolButton::InstantPopup);
        presets->setMenu(new QMenu(presets));
        addStreamPresets(presets->menu(), false);
        stream->addRow("常用组合", presets);
        stream->addRow("目标帧率", fps);
        fpsFallback = new QLabel(streamPage);
        fpsFallback->setWordWrap(true);
        stream->addRow(fpsFallback);
        fpsFallback->hide();
        rateMode = new QComboBox(settings);
        rateMode->setObjectName("rateMode");
        rateMode->addItem("自适应上限", true);
        rateMode->addItem("固定上限", false);
        rateMode->setCurrentIndex(rateMode->findData(o.value("adaptiveRate").toBool(true)));
        rateMode->setToolTip("自适应只根据收包情况调整带宽上限。固定模式持续使用所选上限，实际流量随画面而变。");
        jpegQuality = new QComboBox(settings);
        jpegQuality->setObjectName("jpegQuality");
        jpegQuality->addItem("自动", 0);
        for (int q : {20, 40, 60, 80})
            jpegQuality->addItem(QString("Q%1").arg(q), q);
        jpegQuality->setCurrentIndex(std::max(0, jpegQuality->findData(o["jpegQuality"].toInt())));
        jpegQuality->setToolTip("控制掌机上下屏的 JPEG 编码。Q 越高，通常越清晰、每帧也越大。自动档按带宽预算选择质量。");
        stream->addRow("JPEG 质量", jpegQuality);
        layoutChoice = new QComboBox(settings);
        layoutChoice->setObjectName("layoutChoice");
        layoutChoice->addItem("上下双屏", "stacked");
        layoutChoice->addItem("左右双屏", "side-by-side");
        layoutChoice->addItem("分离窗口", "separate");
        layoutChoice->addItem("仅上屏", "top-only");
        layoutChoice->addItem("仅下屏", "bottom-only");
        layoutChoice->setCurrentIndex(std::max(0, layoutChoice->findData(o.value("layout").toString())));
        layoutChoice->setToolTip("仅显示一个屏幕时，掌机会停止另一屏的编码和发送，释放算力与带宽。");
        picture->addRow("画面布局", layoutChoice);
        jpegSettingsStatus = new QLabel(streamPage);
        jpegSettingsStatus->setWordWrap(true);
        stream->addRow(jpegSettingsStatus);
        jpegSettingsStatus->hide();
        stream->addRow("带宽上限", rate);
        stream->addRow("带宽模式", rateMode);
        scale = new QComboBox(settings);
        scale->addItem("原生 1×", 1);
        scale->addItem("2×", 2);
        scale->setCurrentIndex(o["scale"].toInt() == 2);
        picture->addRow("显示倍率", scale);
        motion = new QComboBox(settings);
        motion->setObjectName("motionMethod");
        motion->addItem("关闭", "off");
        motion->addItem("DIS 光流", "flow");
        motion->addItem("RIFE AI", "rife");
        auto *motionModel = qobject_cast<QStandardItemModel *>(motion->model());
        for (int i = 0; i < motion->count(); ++i)
            motionChoices.insert(motion->itemData(i).toString(), {motion->itemText(i), {}, nullptr, motionModel->item(i)});
        motion->setCurrentIndex(std::max(0, motion->findData(o["interpolate"].toString())));
        picture->addRow("运动补帧", motion);
        rifePrecision = new QComboBox(settings);
        rifePrecision->setObjectName("rifePrecision");
        rifePrecision->addItem("FP32 · 全精度（默认）", "fp32");
        rifePrecision->addItem("FP16 · 半精度", "fp16");
        rifePrecision->setCurrentIndex(o["rife_precision"].toString() == "fp16" ? 1 : 0);
        picture->addRow("RIFE 推理精度", rifePrecision);
        updatePrecisionControl();
        connect(motion, &QComboBox::currentIndexChanged, this, &Viewer::updatePrecisionControl);
        effectMessage = new QLabel(picturePage);
        effectMessage->setObjectName("effectMessage");
        effectMessage->setTextFormat(Qt::PlainText);
        effectMessage->setWordWrap(true);
        picture->addRow(effectMessage);
        effectMessage->hide();
        effectButtons = new QDialogButtonBox(picturePage);
        reduceEffects = effectButtons->addButton("使用建议设置", QDialogButtonBox::ActionRole);
        retryEffects = effectButtons->addButton("重新检测", QDialogButtonBox::ActionRole);
        reduceEffects->setObjectName("reduceEffects");
        retryEffects->setObjectName("retryEffects");
        reduceEffects->setAutoDefault(false);
        retryEffects->setAutoDefault(false);
        connect(reduceEffects, &QPushButton::clicked, this, [this] { emitAction("reduce-effects"); });
        connect(retryEffects, &QPushButton::clicked, this, [this] { emitAction("retry-effects"); });
        picture->addRow(effectButtons);
        effectButtons->hide();
        rtsp = new QLineEdit(o["rtsp"].toString(), settings);
        rtsp->setPlaceholderText("留空关闭发布");
        connection->addRow("RTSP 发布地址", rtsp);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, settings);
        buttons->setObjectName("settingsButtons");
        buttons->button(QDialogButtonBox::Apply)->setText("应用");
        buttons->button(QDialogButtonBox::Close)->setText("关闭");
        layout->addWidget(buttons);
        connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked, settings, &QDialog::hide);
        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] {
            QJsonObject changes{{"type", "apply"},
                                     {"fps", fps->currentData().toInt()},
                                     {"rate", rate->currentData().toInt()},
                                     {"adaptiveRate", rateMode->currentData().toBool()},
                                     {"jpegQuality", jpegQuality->currentData().toInt()},
                                     {"layout", layoutChoice->currentData().toString()},
                                     {"ip", ip->text().trimmed()},
                                     {"port", port->value()},
                                     {"scale", scale->currentData().toInt()},
                                     {"motion", motion->currentData().toString()},
                                     {"rtsp", rtsp->text().trimmed()}};
            if (rifePrecision)
                changes["rife_precision"] = rifePrecision->currentData().toString();
            queue.append(changes);
            settings->hide();
        });
    }

  public:
    live::DebugCapture capture;
    explicit Viewer(const QJsonObject &o) {
        setWindowTitle(o["title"].toString());
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        QSize size(o["width"].toInt(), o["height"].toInt());
        screenLayout = o.value("layout").toString("stacked");
        canvas = new Canvas(size, this);
        applyCanvasLayout();
        canvas->setObjectName("canvas");
        canvas->installEventFilter(this);
        layout->addWidget(canvas, 1);
        panel = new QWidget(this);
        auto *bar = mainBar = new QHBoxLayout(panel);
        bar->setContentsMargins(4, 2, 4, 2);
        bar->setSpacing(2);
        status = new StatusLabel("等待 3DS", panel);
        status->setTextFormat(Qt::PlainText);
        status->setObjectName("connectionStatus");
        status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        bar->addWidget(status, 1);
        quality = new QLabel(panel);
        quality->setObjectName("qualityStatus");
        bar->addWidget(quality);
        quickFps = quickControl(bar, "Fps", "修改掌机目标帧率");
        quickJpeg = quickControl(bar, "Jpeg", "修改掌机 JPEG 画质");
        quickRate = quickControl(bar, "Rate", "修改掌机带宽上限 (MB/s)");
        generation = new QToolButton(panel);
        generation->setObjectName("frameGeneration");
        generation->setAutoRaise(true);
        generationLabel(o["interpolate"].toString());
        bar->addWidget(generation);
        connect(generation, &QToolButton::clicked, this, [this] { emitAction("motion"); });
        performance = new QToolButton(panel);
        performance->setObjectName("performance");
        performance->setText("ⓘ");
        performance->setAccessibleName("直播性能，悬停查看，点击固定");
        performance->setCheckable(true);
        performance->setAutoRaise(true);
        bar->addWidget(performance);
        performanceCard = new QLabel(canvas);
        performanceCard->setObjectName("performanceCard");
        performanceCard->setTextFormat(Qt::PlainText);
        performanceCard->setWordWrap(true);
        performanceCard->setAttribute(Qt::WA_TransparentForMouseEvents);
        performanceCard->setStyleSheet("QLabel { background: rgba(20,24,30,235); color: white; padding: 8px; border-radius: 5px; }");
        performanceCard->hide();
        performance->installEventFilter(this);
        connect(performance, &QToolButton::toggled, this, [this](bool pinned) { showCard(pinned || performance->underMouse()); });
        auto *copyShot = new QAction("复制双屏和性能截图", performance);
        copyShot->setObjectName("copyPerformanceShot");
        copyShot->setShortcut(QKeySequence("Ctrl+Shift+C"));
        performance->setContextMenuPolicy(Qt::ActionsContextMenu);
        performance->addAction(copyShot);
        effectSettings = new QAction("显示与补帧…", performance);
        effectSettings->setObjectName("effectSettings");
        effectSettings->setVisible(false);
        performance->addAction(effectSettings);
        connect(effectSettings, &QAction::triggered, this, &Viewer::openEffectSettings);
        addAction(copyShot);
        connect(copyShot, &QAction::triggered, this, [this] {
            bool visible = !performanceCard->isHidden();
            showCard(true);
            refreshCard(false);
            QApplication::clipboard()->setPixmap(canvas->grab());
            showCard(visible);
        });
        elapsed = new QLabel(panel);
        elapsed->setObjectName("recordDuration");
        bar->addWidget(elapsed);
        record = new RecordButton(panel);
        bar->addWidget(record);
        connect(record, &QToolButton::clicked, this, [this] { toggleRecording(); });
        menu = new QMenu(this);
        menu->setObjectName("viewerMenu");
        more = new QToolButton(panel);
        more->setText("⋯");
        more->setObjectName("settings");
        more->setAccessibleName("更多设置");
        more->setToolTip("观看与投屏设置");
        bar->addWidget(more);
        connect(more, &QToolButton::clicked, this, [this] {
            const QSize size = menu->sizeHint();
            const QRect anchor(more->mapToGlobal(QPoint()), more->size());
            const QRect available = more->screen()->availableGeometry();
            const int x = std::max(available.left(), anchor.right() + 1 - size.width());
            const int y = anchor.top() - size.height() >= available.top()
                          ? anchor.top() - size.height() : anchor.bottom() + 1;
            menu->popup(QPoint(x, y));
        });
        layout->addWidget(panel);
        restore = new QToolButton(canvas);
        restore->setText("⋯");
        restore->setToolTip("显示工具栏 (H)");
        restore->setFixedSize(32, 22);
        connect(restore, &QToolButton::clicked, this, [this] { controls(true); });
        auto action = [this](QMenu *parent, const QString &label, QKeySequence key, auto callback) {
            auto *a = parent->addAction(label);
            if (!key.isEmpty()) {
                a->setShortcut(key);
                addAction(a);
            }
            connect(a, &QAction::triggered, this, callback);
            return a;
        };
        auto submenu = [this](const QString &title, const QString &name) {
            auto *sub = menu->addMenu(title);
            sub->setObjectName(name);
            return sub;
        };
        makeSettings(o);
        auto *limitsMenu = submenu("掌机推流", "limitsMenu");
        limitsMenu->addSection("固定上限组合");
        addStreamPresets(limitsMenu, true);
        limitsMenu->addSeparator();
        auto *editStream = action(limitsMenu, "自定义参数…", {}, [this] { openSettings(StreamTab); });
        editStream->setObjectName("editStreamSettings");
        menu->addSeparator();
        auto *bandwidth = quickRate->menu();
        bandwidth->setToolTipsVisible(true);
        rateStatus = bandwidth->addAction(QString());
        rateStatus->setEnabled(false);
        rateStatus->setVisible(false);
        rateModes = new QActionGroup(bandwidth);
        for (int i = 0; i < rateMode->count(); ++i) {
            const bool adaptive = rateMode->itemData(i).toBool();
            auto *choice = bandwidth->addAction(rateMode->itemText(i));
            choice->setCheckable(true);
            choice->setToolTip(rateMode->toolTip());
            rateModes->addAction(choice);
            choice->setChecked(i == rateMode->currentIndex());
            connect(choice, &QAction::triggered, this, [this, adaptive] {
                queue.append(QJsonObject{{"type", "limit"}, {"adaptiveRate", adaptive}});
            });
        }
        bandwidth->addSeparator();
        auto *jpegMenu = quickJpeg->menu();
        jpegMenu->setObjectName("jpegQualityMenu");
        jpegMenu->setToolTipsVisible(true);
        auto *jpegGroup = new QActionGroup(jpegMenu);
        for (int i = 0; i < jpegQuality->count(); ++i) {
            const int target = jpegQuality->itemData(i).toInt();
            auto *a = action(jpegMenu, jpegQuality->itemText(i), {}, [this, target] {
                queue.append(QJsonObject{{"type", "limit"}, {"jpegQuality", target}});
            });
            a->setData(target);
            a->setCheckable(true);
            a->setToolTip(jpegQuality->toolTip());
            jpegGroup->addAction(a);
        }
        selectJpeg(jpegQuality->currentData().toInt());
        jpegMenuStatus = jpegMenu->addAction(QString());
        jpegMenuStatus->setEnabled(false);
        jpegMenuStatus->setVisible(false);
        updatePictureScale(o["mode"].toString(), o["scale"].toInt(1));
        const auto modes = o["modes"].toObject();
        if (!modes.isEmpty()) {
            auto *pictureMenu = submenu("PC 画质增强", "pictureMenu");
            pictureMenu->setToolTipsVisible(true);
            auto *group = new QActionGroup(pictureMenu);
            for (auto id : {"off", "fast-detail", "fbcnn", "fbcnn-detail", "span", "fbcnn-sr", "clean", "gpu", "sr"}) {
                if (QLatin1String(id) == "clean") pictureMenu->addSeparator();
                if (!modes.contains(id)) continue;
                auto *a = pictureMenu->addAction(modes[id].toString());
                if (QLatin1String(id) == "fbcnn")
                    a->setToolTip("修复 JPEG 压缩产生的色块、噪点，保留原生分辨率。");
                if (QLatin1String(id) == "fbcnn-detail")
                    a->setToolTip("先修复 JPEG，再提升原尺寸清晰度。高压缩时按双屏各自的 JPEG Q 保护文字和锐利轮廓。");
                if (QLatin1String(id) == "span")
                    a->setToolTip("用 SPAN 放大到 2×。优先使用 DirectML FP16，不可用时自动尝试 FP32。");
                if (QLatin1String(id) == "sr")
                    a->setToolTip("用 Real-ESRGAN AnimeVideo-v3 放大画面，输出 2×。");
                a->setData(id);
                a->setCheckable(true);
                a->setChecked(o["mode"].toString() == id);
                group->addAction(a);
                pictures.insert(id, {a->text(), a->toolTip(), a});
                connect(a, &QAction::triggered, this,
                        [this, id] { queue.append(QJsonObject{{"type", "mode"}, {"value", id}}); });
            }
            pictureMenu->addSeparator();
            action(pictureMenu, "开关画质增强", Qt::Key_E, [this] { emitAction("enhance"); });
        }
        if (o["motion"].toBool()) {
            auto *motionMenu = submenu("运动补帧", "motionMenu");
            motionMenu->setToolTipsVisible(true);
            auto *group = new QActionGroup(motionMenu);
            for (int i = 0; i < motion->count(); ++i) {
                const auto method = motion->itemData(i).toString();
                auto *a = action(motionMenu, motion->itemText(i), {}, [this, method] {
                    queue.append(QJsonObject{{"type", "motion"}, {"value", method}});
                });
                a->setData(method);
                a->setCheckable(true);
                a->setChecked(method == motion->currentData().toString());
                group->addAction(a);
                motionChoices[method].action = a;
            }
            motionMenu->addSeparator();
            action(motionMenu, "开关补帧", Qt::Key_F, [this] { emitAction("motion"); });
            action(motionMenu, "显示与补帧设置…", {}, [this] { openEffectSettings(); });
        }
        menu->addSeparator();
        auto *layoutMenu = submenu("画面布局", "layoutMenu");
        layoutGroup = new QActionGroup(layoutMenu);
        for (const auto &entry : {std::pair{"上下双屏", "stacked"}, std::pair{"左右双屏", "side-by-side"},
                                   std::pair{"分离窗口", "separate"}, std::pair{"仅上屏", "top-only"},
                                   std::pair{"仅下屏", "bottom-only"}}) {
            auto *a = action(layoutMenu, entry.first, {}, [this, id = QString(entry.second)] {
                queue.append(QJsonObject{{"type", "limit"}, {"layout", id}});
            });
            a->setData(QString(entry.second));
            a->setCheckable(true);
            layoutGroup->addAction(a);
            a->setChecked(o["layout"].toString() == entry.second);
        }
        action(layoutMenu, "更多布局设置…", {}, [this] { openSettings(PictureTab); });
        auto *viewing = submenu("观看", "viewingMenu");
        if (o["mute"].toBool())
            mute = action(viewing, "静音", {}, [this] { emitAction("mute"); });
        auto *toolbarAction = action(viewing, "隐藏工具栏", Qt::Key_H, [this] { controls(panel->isHidden()); });
        auto *fullAction = action(viewing, "全屏", Qt::Key_F11, [this] { fullscreen(); });
        connect(viewing, &QMenu::aboutToShow, this, [this, toolbarAction, fullAction] {
            toolbarAction->setText(panel->isHidden() ? "显示工具栏" : "隐藏工具栏");
            fullAction->setText(isFullScreen() ? "退出全屏" : "全屏");
        });
        auto *recordings = submenu("录像", "recordingMenu");
        recordAction = action(recordings, "开始录像", QKeySequence("Ctrl+R"), [this] { toggleRecording(); });
        recordings->addSeparator();
        openFile =
            action(recordings, "打开最近录像", {}, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(lastFile)); });
        openFolder = action(recordings, "打开录像目录", {}, [this] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(lastFile).absolutePath()));
        });
        openFile->setEnabled(false);
        openFolder->setEnabled(false);
        menu->addSeparator();
        action(menu, "显示与布局…", {}, [this] { openSettings(PictureTab); });
        action(menu, "连接与输出…", QKeySequence("Ctrl+,"), [this] { openSettings(ConnectionTab); });
        auto *diagnostics = submenu("性能诊断", "diagnosticsMenu");
        action(diagnostics, "性能详情", Qt::Key_F12, [this] { showDebug(); });
        diagnostics->addAction(copyShot);
        diagnostics->addSeparator();
        action(diagnostics, "重新监听", {}, [this] { emitAction("retry"); });
        connect(new QShortcut(Qt::Key_Escape, this), &QShortcut::activated, this, [this] {
            if (isFullScreen())
                showNormal();
            else
                controls(false);
        });
        setContextMenuPolicy(Qt::CustomContextMenu);
        connect(this, &QWidget::customContextMenuRequested, this,
                [this](QPoint p) { menu->popup(mapToGlobal(p)); });
        panel->setVisible(o["controls"].toBool(true));
        restore->setVisible(panel->isHidden());
        setRecording("idle", {}, {});
        if (o.contains("effects")) updateEffects(o.value("effects").toObject());
        if (!o["record"].toBool()) {
            record->setDisabled(true);
            recordAction->setDisabled(true);
        }
        setMinimumSize(240, 180);
        fitCanvas(size);
        canvas->message(o["notice"].toString());
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == QStringLiteral("windows")) {
            // This top-level window is a viewer. Text entry lives in separate
            // settings and save dialogs, which keep their own input contexts.
            setAttribute(Qt::WA_ShowWithoutActivating);
            ImmAssociateContext(reinterpret_cast<HWND>(winId()), nullptr);
            STARTUPINFOW startup{};
            GetStartupInfoW(&startup);
            activateAfterFirstPaint = !(startup.dwFlags & STARTF_USESHOWWINDOW) ||
                (startup.wShowWindow != SW_HIDE && startup.wShowWindow != SW_SHOWNOACTIVATE &&
                 startup.wShowWindow != SW_SHOWNA && startup.wShowWindow != SW_SHOWMINNOACTIVE);
        }
#endif
        show();
        setAttribute(Qt::WA_ShowWithoutActivating, false);
        if (o["debug"].toBool())
            QTimer::singleShot(0, this, [this] { showDebug(); });
    }
    void fitCanvas(QSize target) {
        const int barHeight = panel->isHidden() ? 0 : panel->sizeHint().height();
        const auto margins = windowHandle() ? windowHandle()->frameMargins() : QMargins(8, 32, 8, 8);
        const QSize available = screen()->availableGeometry().size() -
            QSize(margins.left() + margins.right(), margins.top() + margins.bottom() + barHeight);
        if (target.width() > available.width() || target.height() > available.height())
            target.scale(available, Qt::KeepAspectRatio);
        resize(target.width(), target.height() + barHeight);
    }
    void updatePictureScale(const QString &mode, int value) {
        scale->setCurrentIndex(value == 2);
        scale->setEnabled(mode != "sr" && mode != "span" && mode != "fast-detail" && mode != "fbcnn-detail" && mode != "fbcnn-sr");
        QSize target = (screenLayout == "top-only" || screenLayout == "separate") ? QSize(400 * value, 240 * value)
                     : screenLayout == "bottom-only" ? QSize(320 * value, 240 * value)
                     : screenLayout == "side-by-side" ? QSize(720 * value, 240 * value)
                     : QSize(400 * value, 480 * value);
        if (target == canvas->sizeHint()) return;
        canvas->setSourceSize(target);
        if (!isMaximized() && !isFullScreen()) fitCanvas(target);
    }
    void ensureBottomWindow() {
        if (!bottomWindow) {
            bottomWindow = new QWidget(this, Qt::Tool);
            bottomWindow->setWindowTitle("3DS Live · 下屏");
            bottomCanvas = new Canvas(QSize(320, 240), bottomWindow);
            bottomCanvas->setObjectName("bottomCanvas");
            bottomCanvas->show();
            auto *lay = new QVBoxLayout(bottomWindow);
            lay->setContentsMargins(0, 0, 0, 0);
            lay->addWidget(bottomCanvas);
        }
        bottomWindow->show();
    }
    void hideBottomWindow() {
        if (!bottomWindow) return;
        bottomWindow->hide();
    }
    void applyCanvasLayout() {
        if (!canvas) return;
        canvas->setLayoutMode(screenLayout == "separate" ? "top-only" : screenLayout);
        if (screenLayout == "separate") ensureBottomWindow();
        else hideBottomWindow();
        if (bottomCanvas) bottomCanvas->setLayoutMode("bottom-only");
    }
    void setImage(const QImage &im, std::function<void()> painted) {
        if (screenLayout == "separate") {
            canvas->setImage(im, {});
            if (bottomCanvas) bottomCanvas->setImage(im, std::move(painted));
        } else {
            canvas->setImage(im, std::move(painted));
        }
    }
    QJsonObject paintStats() { return canvas->paintStats(); }
    QJsonArray pullEvents() { return std::exchange(queue, QJsonArray{}); }
    void receive(const QJsonObject &v) {
        if (v.contains("status")) {
            status->setText(v["status"].toString());
            status->setToolTip(v["detail"].toString());
        }
        if (v.contains("notice"))
            canvas->message(v["notice"].toString());
        if (v.contains("quality"))
            quality->setText(v["quality"].toString());
        if (v.contains("layout")) {
            const QString next = v["layout"].toString();
            if (targetLayout != next) {
                targetLayout = next;
                if (layoutGroup)
                    for (auto *a : layoutGroup->actions()) a->setChecked(a->data().toString() == next);
            }
            if (next != screenLayout) {
                screenLayout = next;
                applyCanvasLayout();
                if (layoutChoice) layoutChoice->setCurrentIndex(std::max(0, layoutChoice->findData(next)));
                updatePictureScale("", scale ? scale->currentData().toInt() : 1);
            }
        }
        if (v.contains("fpsOptions")) {
            fps->clear();
            rate->clear();
            for (auto n : v["fpsOptions"].toArray())
                fps->addItem(QString::number(n.toInt()) + " FPS", n.toInt());
            for (auto n : v["rateOptions"].toArray())
                rate->addItem(QString::number(n.toDouble() / 1e6, 'f', 2) + " MB/s", n.toInt());
            fillQuick(quickFps, fps, "fps");
            fillQuick(quickRate, rate, "rate");
        }
        if (v.contains("fps")) {
            targetFps = v["fps"].toInt();
            showRequestedLimit(fps, targetFps, QString("%1 FPS").arg(targetFps));
            updateFpsLabel();
        }
        if (v.contains("jpegQuality"))
            selectJpeg(v["jpegQuality"].toInt());
        if (v.contains("rate")) {
            targetRate = v["rate"].toInt();
            const auto label = QString("%1 MB/s").arg(targetRate / 1e6);
            showRequestedLimit(rate, targetRate, label);
            selectQuick(quickRate, "带宽上限", label, targetRate);
        }
        if (v.contains("adaptiveRate")) {
            targetAdaptive = v["adaptiveRate"].toBool();
            const int index = rateMode->findData(targetAdaptive);
            rateMode->setCurrentIndex(index);
            rateModes->actions().at(index)->setChecked(true);
        }
        if (v.contains("motion")) {
            motion->setCurrentIndex(motion->findData(v["motion"].toString()));
            if (auto *choice = motionChoices.value(v["motion"].toString()).action) choice->setChecked(true);
        }
        if (v.contains("motion"))
            generationLabel(motion->currentData().toString());
        if (rifePrecision && v.contains("rife_precision"))
            rifePrecision->setCurrentIndex(v["rife_precision"].toString() == "fp16" ? 1 : 0);
        if (v.contains("scale"))
            updatePictureScale(v["mode"].toString(), v["scale"].toInt());
        if (v.contains("mode") && pictures.contains(v["mode"].toString()))
            pictures[v["mode"].toString()].action->setChecked(true);
        if (v.contains("effects")) updateEffects(v.value("effects").toObject());
        if (v.contains("muted") && mute)
            mute->setText(v["muted"].toBool() ? "恢复声音" : "静音");
        if (v.contains("recording"))
            setRecording(v["recording"].toString(), v["path"].toString(), v["error"].toString());
        if (v.contains("duration") && record->active) {
            auto s = v["duration"].toInteger();
            elapsed->setText(QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0')));
        }
        if (v.contains("debug")) {
            snapshot = v["debug"].toObject();
            updateFpsLabel();
            updateJpegLabel();
            rateStatus->setText(QString("掌机上限 %1 · 实收 %2 MB/s")
                                    .arg(number("device_cap", 2, 1e6), number("bytes_per_s", 2, 1e6)));
            rateStatus->setVisible(snapshot.value("connection").toString() == "live");
            const auto jpeg = jpegStatus();
            jpegSettingsStatus->setText(jpeg);
            jpegSettingsStatus->setVisible(!jpeg.isEmpty());
            if (jpegMenuStatus) {
                jpegMenuStatus->setText(jpeg);
                jpegMenuStatus->setVisible(!jpeg.isEmpty());
            }
            history = v["history"].toArray();
            if (performance) {
                const auto health = snapshot.value("frame_health").toObject();
                const QString label = health.isEmpty() ? QStringLiteral("ⓘ") : health.value("label").toString();
                if (label != performance->text()) {
                    performance->setText(label);
                    performance->setStyleSheet(health.isEmpty() ? QString() : QStringLiteral(
                        "QToolButton { color: #422b08; background: #ffda8b; border-radius: 3px; padding: 1px 3px; }"));
                    performance->setAccessibleName(health.isEmpty() ? QStringLiteral("直播性能，悬停查看，点击固定") :
                        QString("掉帧观察 · %1，悬停查看，点击固定").arg(label));
                }
            }
            refreshCard();
            if (debug)
                debug->receive(snapshot, history);
        }
        arrangeBar(width());
    }

  protected:
    void resizeEvent(QResizeEvent *e) override {
        QWidget::resizeEvent(e);
        arrangeBar(e->size().width());
    }
    bool eventFilter(QObject *obj, QEvent *e) override {
        if (activateAfterFirstPaint && obj == canvas && e->type() == QEvent::Paint) {
            // Windows can call third-party text services synchronously on
            // activation. Finish this backing-store paint before requesting focus.
            activateAfterFirstPaint = false;
            QMetaObject::invokeMethod(this, [this] {
                auto *active = QApplication::activeWindow();
                if (isVisible() && !isMinimized() && !active && !QApplication::activePopupWidget())
                    activateWindow();
            }, Qt::QueuedConnection);
        }
        if (performance && obj == performance) {
            if (e->type() == QEvent::Enter || e->type() == QEvent::FocusIn) showCard(true);
            if (e->type() == QEvent::Leave || e->type() == QEvent::FocusOut || e->type() == QEvent::Hide)
                showCard(performance->isChecked());
        }
        if (obj == canvas && e->type() == QEvent::Resize) refreshCard();
        if (obj == canvas && e->type() == QEvent::MouseButtonDblClick) {
            controls(panel->isHidden());
            return true;
        }
        if (obj == canvas && e->type() == QEvent::Resize && restore)
            restore->move(canvas->width() - 35, canvas->height() - 25);
        return QWidget::eventFilter(obj, e);
    }
    void closeEvent(QCloseEvent *e) override {
        emitAction("close");
        e->accept();
    }
};
} // namespace

namespace live {
void *createView(const QJsonObject &options) {
    return new Viewer(options);
}
void setDebugCapture(void *v, std::function<QJsonObject(bool)> capture) {
    static_cast<Viewer *>(v)->capture = std::move(capture);
}
void destroyView(void *v) {
    delete static_cast<Viewer *>(v);
}
QJsonArray takeEvents(void *v) {
    return static_cast<Viewer *>(v)->pullEvents();
}
void showFrame(void *v, const QImage &im, std::function<void()> onPainted) {
    static_cast<Viewer *>(v)->setImage(im, std::move(onPainted));
}
QJsonObject takePaintStats(void *v) {
    return static_cast<Viewer *>(v)->paintStats();
}
void updateView(void *v, const QJsonObject &o) {
    static_cast<Viewer *>(v)->receive(o);
}
} // namespace live
