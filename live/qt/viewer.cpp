#include "viewer_native.h"
#include "debug_window.h"
#include <QtWidgets>
#include <algorithm>
#include <utility>
namespace {
class Canvas final : public QWidget {
    QSize sourceSize;
    QImage image;
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
    void setSourceSize(QSize size) {
        sourceSize = size;
        updateGeometry();
    }
    void setImage(const QImage &frame) {
        if (submitClock.isValid())
            ++coalesced;
        submitClock.start();
        image = frame;
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
            return;
        }
        QSize sz = image.size().scaled(size(), Qt::KeepAspectRatio);
        QRect target(QPoint((width() - sz.width()) / 2, (height() - sz.height()) / 2), sz);
        if (target != rect())
            p.fillRect(rect(), Qt::black);
        p.setRenderHint(QPainter::SmoothPixmapTransform, sz != image.size());
        p.drawImage(target, image);
        if (submitClock.isValid()) {
            ++paintedChanges;
            maxPaintNs = std::max(maxPaintNs, submitClock.nsecsElapsed());
            submitClock.invalidate();
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
    QToolButton *quickFps = nullptr, *quickRate = nullptr, *generation = nullptr, *performance = nullptr;
    QLabel *performanceCard = nullptr;
    QToolButton *restore = nullptr, *more = nullptr;
    RecordButton *record;
    QMenu *menu, *limitsMenu = nullptr;
    QAction *recordAction, *mute = nullptr, *openFile, *openFolder;
    QDialog *settings;
    QTabWidget *settingsTabs;
    QComboBox *fps, *rate, *motion, *scale, *rifePrecision = nullptr;
    QLineEdit *ip, *rtsp;
    QSpinBox *port;
    QMap<QString, QAction *> pictures, motionChoices;
    QPointer<QFileDialog> saveDialog;
    live::DebugWindow *debug = nullptr;
    QJsonObject snapshot;
    QJsonArray history, queue;
    QString lastFile;
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
        choices->clear();
        for (int i = 0; i < combo->count(); ++i) {
            auto *a = choices->addAction(combo->itemText(i));
            a->setData(combo->itemData(i));
            a->setCheckable(true);
            connect(a, &QAction::triggered, this, [this, a, field] {
                queue.append(QJsonObject{{"type", "limit"}, {field, a->data().toInt()}});
            });
        }
    }
    void selectQuick(QToolButton *button, const QString &text, int value) {
        button->setText(text);
        button->menu()->setTitle((button == quickFps ? QStringLiteral("目标帧率　") : QStringLiteral("码率上限　")) + text);
        for (auto *a : button->menu()->actions())
            a->setChecked(a->data().toInt() == value);
    }
    void generationLabel(const QString &method) {
        QString text = method == "flow" ? "补帧 DIS" : method == "rife" ? "补帧 RIFE" : "补帧 关";
        generation->setText(text);
        generation->setAccessibleName(generation->text());
        generation->setToolTip(text + QStringLiteral(" · F 切换运动补帧"));
    }
    void refreshCard(bool showHint = true) {
        if (!performanceCard || performanceCard->isHidden())
            return;
        auto number = [this](QString key, int decimals = 0, double divisor = 1.) {
            if (snapshot.contains("rx_sample_us") &&
                (key == "device_sent_fps" || key == "device_cap" || key.startsWith("captured_")) &&
                snapshot.value("wall_us").toInteger() - snapshot.value("device_status_us").toInteger() >= 2500000)
                return QStringLiteral("—");
            const auto v = snapshot.value(key);
            return v.isDouble() ? QString::number(v.toDouble() / divisor, 'f', decimals) : QStringLiteral("—");
        };
        const auto method = snapshot["motion"].toString();
        const QString motionName = method == "flow" ? "DIS 光流" : method == "rife" ? "RIFE AI" : "关闭";
        QStringList rows{"直播性能   " + snapshot["time"].toString()};
        const auto health = snapshot.value("frame_health").toObject();
        if (!health.isEmpty()) {
            rows << (snapshot.value("connection") == "interrupted" ? QStringLiteral("收帧状态") :
                                                                     QStringLiteral("掉帧观察 · 最近约 3 秒"));
            for (const auto &line : health.value("evidence").toArray())
                rows << line.toString();
        }
        if (snapshot["connection"].toString() == "live") {
            rows << QString("源帧 %1   ·   生成 %2   ·   合计 %3 FPS")
                        .arg(number("fps_source_presented", 1), number("fps_generated", 1), number("fps_presented", 1))
                 << QString("运动补帧 %1   ·   缓冲 %2 ms").arg(motionName, number("playout_delay_ms"))
                 << QString("掌机发送 %1   ·   双屏收齐 %2 FPS").arg(number("device_sent_fps", 1), number("fps_received", 1))
                 << QString("实际接收 %1   ·   设备上限 %2 MB/s")
                        .arg(number("bytes_per_s", 2, 1e6), number("device_cap", 2, 1e6))
                 << QString("上 / 下屏 JPEG Q  %1 / %2").arg(number("jpeg_top_q"), number("jpeg_bottom_q"))
                 << QString("%1   ·   %2   ·   %3 ms").arg(quality->text(), snapshot["backend"].toString(), number("processing_ms", 1))
                 << QString("本秒最长源帧间隔  %1 ms").arg(number("source_interval_max_gap_ms", 1));
            if (snapshot.contains("jpeg_header_saving_pct"))
                rows << QString("JPEG 表头精简累计省 %1 MB（%2%）")
                            .arg(number("jpeg_header_saved_bytes", 1, 1000000.), number("jpeg_header_saving_pct", 1));
            if (snapshot.contains("fps_top_presented")) {
                rows << QString("真实更新 上 %1 · 下 %2 FPS").arg(number("fps_top_presented", 1), number("fps_bottom_presented", 1));
                rows << QString("上屏 >100 ms %1 次 · 最长 %2 ms").arg(number("top_gaps_over_100ms"), number("top_max_gap_ms", 1));
                rows << QString("下屏 >100 ms %1 次 · 最长 %2 ms").arg(number("bottom_gaps_over_100ms"), number("bottom_max_gap_ms", 1));
            }
            if (snapshot.contains("captured_top_fps"))
                rows << QString("掌机采集 上 / 下  %1 / %2 FPS").arg(number("captured_top_fps", 1), number("captured_bottom_fps", 1));
            if (method == "flow")
                rows << QString("光流保护累计跳过 %1 对").arg(number("motion_quality_holds"));
        } else {
            rows << status->text();
        }
        if (method == "rife") {
            const QString requested = snapshot["rife_precision"].toString() == "fp16" ? "FP16" : "FP32";
            QString backend = snapshot["motion_backend"].toString();
            if (backend == "DirectML") backend = QStringLiteral("DirectML FP32");
            QString state = QStringLiteral("等待推理");
            if (!snapshot["motion_error"].toString().isEmpty())
                state = QStringLiteral("推理失败   ·   F12 查看原因");
            else if (!backend.isEmpty())
                state = QString("实际 %1   ·   %2 ms").arg(backend, number("motion_processing_ms", 1));
            rows << QString("RIFE 设置 %1   ·   %2").arg(requested, state);
            if (!backend.isEmpty() && !snapshot["motion_backend_fallback"].toString().isEmpty())
                rows << QStringLiteral("RIFE 已回退   ·   F12 查看原因");
        }
        if (showHint) rows << QStringLiteral("点击性能按钮固定   ·   右键复制截图");
        performanceCard->setText(rows.join('\n'));
        int natural = 0;
        for (const auto &row : rows)
            natural = std::max(natural, performanceCard->fontMetrics().horizontalAdvance(row));
        const int width = std::min(natural + 22, std::max(1, canvas->width() - 12));
        performanceCard->setFixedSize(width, performanceCard->heightForWidth(width));
        performanceCard->move(canvas->width()-performanceCard->width()-6, std::max(0,canvas->height()-performanceCard->height()-6));
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
        fit(quickRate);
        if (limitsMenu) limitsMenu->menuAction()->setVisible(panel->isHidden() || quickFps->isHidden() || quickRate->isHidden());
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
    void openSettings() {
        settings->show();
        settings->raise();
        settings->activateWindow();
    }
    void makeSettings(const QJsonObject &o) {
        settings = new QDialog(this);
        settings->setWindowTitle("投屏设置");
        settings->setObjectName("streamSettings");
        auto *layout = new QVBoxLayout(settings);
        settingsTabs = new QTabWidget(settings);
        settingsTabs->setObjectName("streamSettingsTabs");
        layout->addWidget(settingsTabs);
        auto *connectionPage = new QWidget(settingsTabs);
        auto *picturePage = new QWidget(settingsTabs);
        settingsTabs->addTab(connectionPage, "投屏连接");
        settingsTabs->addTab(picturePage, "画面与补帧");
        auto *connection = new QFormLayout(connectionPage);
        auto *picture = new QFormLayout(picturePage);
        connection->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        picture->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
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
        connection->addRow("目标帧率上限", fps);
        connection->addRow("目标码率上限", rate);
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
        motion->setCurrentIndex(std::max(0, motion->findData(o["interpolate"].toString())));
        picture->addRow("运动补帧", motion);
        rifePrecision = new QComboBox(settings);
        rifePrecision->setObjectName("rifePrecision");
        rifePrecision->addItem("FP32 · 全精度（默认）", "fp32");
        rifePrecision->addItem("FP16 · 半精度", "fp16");
        rifePrecision->setCurrentIndex(o["rife_precision"].toString() == "fp16" ? 1 : 0);
        rifePrecision->setToolTip("选择 RIFE 的推理精度。应用后可悬停 ⓘ 查看实际后端与耗时。");
        picture->addRow("RIFE 推理精度", rifePrecision);
        rifePrecision->setEnabled(motion->currentData().toString() == "rife");
        connect(motion, &QComboBox::currentIndexChanged, this, [this] {
            rifePrecision->setEnabled(motion->currentData().toString() == "rife");
        });
        rtsp = new QLineEdit(o["rtsp"].toString(), settings);
        rtsp->setPlaceholderText("留空关闭发布");
        connection->addRow("RTSP 发布地址", rtsp);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, settings);
        buttons->button(QDialogButtonBox::Apply)->setText("应用");
        buttons->button(QDialogButtonBox::Close)->setText("关闭");
        layout->addWidget(buttons);
        connect(buttons->button(QDialogButtonBox::Close), &QPushButton::clicked, settings, &QDialog::hide);
        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] {
            QJsonObject changes{{"type", "apply"},
                                     {"fps", fps->currentData().toInt()},
                                     {"rate", rate->currentData().toInt()},
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
        canvas = new Canvas(size, this);
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
        quickRate = quickControl(bar, "Rate", "修改掌机码率上限 (MB/s)");
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
        updatePictureScale(o["mode"].toString(), o["scale"].toInt(1));
        const auto modes = o["modes"].toObject();
        if (!modes.isEmpty()) {
            auto *pictureMenu = submenu("画质", "pictureMenu");
            pictureMenu->setToolTipsVisible(true);
            auto *group = new QActionGroup(pictureMenu);
            for (auto id : {"off", "fast-detail", "fbcnn", "fbcnn-detail", "fbcnn-sr", "clean", "gpu", "sr"}) {
                if (QLatin1String(id) == "clean") pictureMenu->addSeparator();
                if (!modes.contains(id)) continue;
                auto *a = pictureMenu->addAction(modes[id].toString());
                if (QLatin1String(id) == "fbcnn")
                    a->setToolTip("修复 JPEG 压缩产生的色块、噪点，保留原生分辨率。");
                if (QLatin1String(id) == "fbcnn-detail")
                    a->setToolTip("先修复 JPEG，再提升原尺寸清晰度。高压缩时按双屏各自的 JPEG Q 保护文字和锐利轮廓。");
                a->setData(id);
                a->setCheckable(true);
                a->setChecked(o["mode"].toString() == id);
                group->addAction(a);
                pictures[id] = a;
                connect(a, &QAction::triggered, this,
                        [this, id] { queue.append(QJsonObject{{"type", "mode"}, {"value", id}}); });
            }
            pictureMenu->addSeparator();
            action(pictureMenu, "开关画质增强", Qt::Key_E, [this] { emitAction("enhance"); });
        }
        if (o["motion"].toBool()) {
            auto *motionMenu = submenu("补帧", "motionMenu");
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
                motionChoices[method] = a;
            }
            motionMenu->addSeparator();
            action(motionMenu, "开关补帧", Qt::Key_F, [this] { emitAction("motion"); });
            action(motionMenu, "补帧设置…", {}, [this] {
                settingsTabs->setCurrentIndex(1);
                openSettings();
            });
        }
        limitsMenu = submenu("帧率与码率", "limitsMenu");
        limitsMenu->addMenu(quickFps->menu());
        limitsMenu->addMenu(quickRate->menu());
        menu->addSeparator();
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
        action(menu, "投屏设置…", QKeySequence("Ctrl+,"), [this] { openSettings(); });
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
        if (!o["record"].toBool()) {
            record->setDisabled(true);
            recordAction->setDisabled(true);
        }
        setMinimumSize(240, 180);
        fitCanvas(size);
        show();
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
        scale->setEnabled(mode != "sr" && mode != "fast-detail" && mode != "fbcnn-detail" && mode != "fbcnn-sr");
        QSize target(400 * value, 480 * value);
        if (target == canvas->sizeHint()) return;
        canvas->setSourceSize(target);
        if (!isMaximized() && !isFullScreen()) fitCanvas(target);
    }
    void setImage(const QImage &im) { canvas->setImage(im); }
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
            fps->setCurrentIndex(std::max(0, fps->findData(v["fps"].toInt())));
            selectQuick(quickFps, QString("%1 FPS").arg(v["fps"].toInt()), v["fps"].toInt());
        }
        if (v.contains("rate")) {
            rate->setCurrentIndex(std::max(0, rate->findData(v["rate"].toInt())));
            selectQuick(quickRate, QString("%1 MB/s").arg(QString::number(v["rate"].toDouble()/1e6)), v["rate"].toInt());
        }
        if (v.contains("motion")) {
            motion->setCurrentIndex(motion->findData(v["motion"].toString()));
            if (auto *choice = motionChoices.value(v["motion"].toString())) choice->setChecked(true);
        }
        if (v.contains("motion"))
            generationLabel(motion->currentData().toString());
        if (rifePrecision && v.contains("rife_precision"))
            rifePrecision->setCurrentIndex(v["rife_precision"].toString() == "fp16" ? 1 : 0);
        if (v.contains("scale"))
            updatePictureScale(v["mode"].toString(), v["scale"].toInt());
        if (v.contains("mode") && pictures.contains(v["mode"].toString()))
            pictures[v["mode"].toString()]->setChecked(true);
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
void showFrame(void *v, const QImage &im) {
    static_cast<Viewer *>(v)->setImage(im);
}
QJsonObject takePaintStats(void *v) {
    return static_cast<Viewer *>(v)->paintStats();
}
void updateView(void *v, const QJsonObject &o) {
    static_cast<Viewer *>(v)->receive(o);
}
} // namespace live
