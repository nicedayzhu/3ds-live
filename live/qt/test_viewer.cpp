#include "viewer_native.h"
#include <QtTest>
#include <QtWidgets>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    void *view = live::createView({{"title", "QA"}, {"width", 400}, {"height", 480},
                                   {"controls", true}, {"record", true}});
    auto update = [&](const char *json) {
        live::updateView(view, QJsonDocument::fromJson(json).object());
    };
    auto actions = [&] {
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return live::takeEvents(view);
    };
    QWidget *root = QApplication::activeWindow();
    for (QWidget *widget : QApplication::topLevelWidgets())
        if (widget->windowTitle() == "QA")
            root = widget;
    if (!root)
        return 1;
    if (!root->findChild<QComboBox *>("rifePrecision"))
        return 31;
    update(R"({"fpsOptions":[24,30],"rateOptions":[800000],"fps":30,"rate":800000})");
    QImage frame(400, 480, QImage::Format_RGB888);
    frame.fill(QColor(222, 0, 0));
    live::showFrame(view, frame);
    frame.fill(Qt::black);
    QTest::qWait(50);
    auto *canvas = root->findChild<QWidget *>("canvas");
    auto *button = root->findChild<QToolButton *>("record");
    if (!canvas || !button || canvas->size() != QSize(400, 480) || root->width() != 400)
        return 2;
    auto *settings = root->findChild<QToolButton *>("settings");
    QTest::mouseClick(settings, Qt::LeftButton);
    auto *menu = root->findChild<QMenu *>("viewerMenu");
    if (!menu->isVisible())
        return 14;
    menu->close();
    if (canvas->grab().toImage().pixelColor(50, 50) != QColor(222, 0, 0))
        return 3;
    QTest::mouseClick(button, Qt::LeftButton);
    QApplication::processEvents();
    auto *file = root->findChild<QFileDialog *>();
    if (!file || file->acceptMode() != QFileDialog::AcceptSave)
        return 4;
    file->reject();
    if (!actions().isEmpty())
        return 5;
    QTest::mouseClick(button, Qt::LeftButton);
    QApplication::processEvents();
    file = root->findChild<QFileDialog *>();
    if (!file)
        return 6;
    QTemporaryDir directory;
    if (!directory.isValid())
        return 12;
    file->setDirectory(directory.path());
    auto *name = file->findChild<QLineEdit *>("fileNameEdit");
    if (!name)
        return 13;
    name->setText("sample.mp4");
    QTest::qWait(40);
    QMetaObject::invokeMethod(file, "accept");
    const auto saved = actions();
    if (saved.size() != 1 || saved.first().toObject()["type"] != "record" ||
        QFileInfo(saved.first().toObject()["path"].toString()).fileName() != "sample.mp4")
        return 7;
    update(R"({"recording":"recording","path":"sample.mp4","debug":{"pairs":51}})");
    auto dot = button->grab().toImage();
    if (dot.pixelColor(dot.rect().center()) != QColor("#df3543"))
        return 8;
    QTest::mouseClick(button, Qt::LeftButton);
    if (!actions().contains(QJsonObject{{"type", "stop-record"}}))
        return 9;
    root->activateWindow();
    QTest::qWait(30);
    QTest::keyClick(root, Qt::Key_F12);
    QApplication::processEvents();
    auto *detail = root->findChild<QPlainTextEdit *>();
    auto *history = root->findChild<QTableWidget *>();
    if (!history || !history->isVisible() || !detail || !detail->toPlainText().contains("51"))
        return 10;
    history->window()->hide();
    root->activateWindow();
    QTest::qWait(30);
    QTest::keyClick(root, Qt::Key_H);
    QApplication::processEvents();
    if (settings->isVisible() || !canvas->isVisible())
        return 11;
    live::destroyView(view);
    view = live::createView(QJsonDocument::fromJson(R"({"title":"Live QA","width":400,"height":480,"interpolate":"flow","motion":true,"mode":"off","modes":{"off":"原始","fbcnn":"FBCNN 高画质","gpu":"DnCNN 降噪"}})").object());
    root = nullptr;
    for (auto *widget : QApplication::topLevelWidgets())
        if (widget->windowTitle() == "Live QA") root = widget;
    if (!root) return 15;
    update(R"({"fpsOptions":[24,30],"rateOptions":[600000,800000],"fps":30,"rate":800000,"status":"显示 57.5 · 源 29.5 FPS","quality":"FBCNN","debug":{"connection":"live","fps_source_presented":29.5,"fps_presented":57.5,"fps_generated":28,"motion":"flow"}})");
    auto *quick = root->findChild<QToolButton *>("quickFps");
    auto *cap = root->findChild<QToolButton *>("quickRate");
    auto *gen = root->findChild<QToolButton *>("frameGeneration");
    auto *info = root->findChild<QToolButton *>("performance");
    auto *card = root->findChild<QLabel *>("performanceCard");
    if (!quick || !cap || !gen || !info || !card || !card->isHidden() || !gen->text().contains("DIS"))
        return 16;
    auto *status = root->findChild<QLabel *>("connectionStatus");
    auto *quality = root->findChild<QLabel *>("qualityStatus");
    settings = root->findChild<QToolButton *>("settings");
    button = root->findChild<QToolButton *>("record");
    auto *timer = root->findChild<QLabel *>("recordDuration");
    auto *mainMenu = root->findChild<QMenu *>("viewerMenu");
    auto *limits = root->findChild<QMenu *>("limitsMenu");
    auto *pictureMenu = root->findChild<QMenu *>("pictureMenu");
    auto *motionMenu = root->findChild<QMenu *>("motionMenu");
    if (!mainMenu || !limits || !pictureMenu || !motionMenu) return 41;
    auto checkSingleRow = [&] {
        QApplication::processEvents();
        auto *panel = status->parentWidget();
        int right = -1;
        for (QWidget *widget : QList<QWidget *>{status, quality, quick, cap, gen, info, timer, button, settings}) {
            if (!widget->isVisible()) continue;
            const auto rect = QRect(widget->mapTo(panel, QPoint()), widget->size());
            if (rect.left() <= right || !panel->rect().contains(rect) ||
                !rect.contains(QPoint(rect.center().x(), panel->rect().center().y()))) return false;
            right = rect.right();
        }
        return status->width() >= status->fontMetrics().horizontalAdvance(status->text());
    };
    auto hasLimitControls = [&] {
        if (quick->isVisible() && cap->isVisible()) return true;
        return settings->isVisible() && mainMenu->actions().contains(limits->menuAction()) &&
               limits->menuAction()->isVisible() && limits->actions().contains(quick->menu()->menuAction()) &&
               limits->actions().contains(cap->menu()->menuAction());
    };
    if (!checkSingleRow() || root->width() != 400 || !hasLimitControls() || !gen->isVisible()) {
        qWarning() << "Default toolbar geometry" << root->width() << status->geometry()
                   << quick->geometry() << cap->geometry() << cap->isVisible() << gen->geometry();
        return 23;
    }
    QTest::mouseClick(settings,Qt::LeftButton);
    if (QApplication::activePopupWidget() != mainMenu) return 42;
    int visibleActions = 0;
    for (auto *action : mainMenu->actions()) {
        if (!action->isVisible() || action->isSeparator()) continue;
        if (!mainMenu->rect().contains(mainMenu->actionGeometry(action))) return 43;
        ++visibleActions;
    }
    if (visibleActions > 7 || !mainMenu->actions().contains(pictureMenu->menuAction()) ||
        !mainMenu->actions().contains(motionMenu->menuAction())) return 44;
    mainMenu->setActiveAction(motionMenu->menuAction());
    QTest::keyClick(mainMenu, Qt::Key_Right);
    QApplication::processEvents();
    if (!motionMenu->isVisible()) return 45;
    auto actionFor = [](QMenu *menu, const QString &value) -> QAction * {
        for (auto *action : menu->actions())
            if (action->data().toString() == value) return action;
        return nullptr;
    };
    auto *flowAction = actionFor(motionMenu, "flow");
    auto *rifeAction = actionFor(motionMenu, "rife");
    auto *fbcnnAction = actionFor(pictureMenu, "fbcnn");
    if (!flowAction || !flowAction->isChecked() || !rifeAction || !fbcnnAction) return 46;
    QTest::mouseClick(motionMenu, Qt::LeftButton, Qt::NoModifier, motionMenu->actionGeometry(rifeAction).center());
    fbcnnAction->trigger();
    const auto selected = actions();
    if (!selected.contains(QJsonObject{{"type","motion"},{"value","rife"}}) ||
        !selected.contains(QJsonObject{{"type","mode"},{"value","fbcnn"}})) return 47;
    update(R"({"motion":"rife","mode":"fbcnn","scale":1})");
    if (!rifeAction->isChecked() || flowAction->isChecked() || !fbcnnAction->isChecked()) return 48;
    root->activateWindow();
    QTest::qWait(30);
    QTest::keyClick(root, Qt::Key_F);
    QTest::keyClick(root, Qt::Key_E);
    const auto shortcuts = actions();
    if (!shortcuts.contains(QJsonObject{{"type","motion"}}) ||
        !shortcuts.contains(QJsonObject{{"type","enhance"}})) return 49;
    update(R"({"mode":"fbcnn-sr","scale":2})");
    canvas = root->findChild<QWidget *>("canvas");
    if (!canvas || canvas->sizeHint() != QSize(800,960)) return 29;
    update(R"({"mode":"fbcnn-detail","scale":1})");
    if (canvas->sizeHint() != QSize(400,480) || root->width() != 400) return 30;
    root->resize(240, 400);
    if (!checkSingleRow() || cap->isVisible()) return 24;
    QTest::mouseClick(settings, Qt::LeftButton);
    auto *overflow = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (overflow != mainMenu || !hasLimitControls()) return 25;
    overflow->close();
    root->resize(800, 600);
    if (!checkSingleRow() || !quality->isVisible() || !cap->isVisible() || limits->menuAction()->isVisible())
        return 26;
    root->activateWindow();
    QTest::qWait(30);
    QTest::keyClick(root, Qt::Key_H);
    if (settings->isVisible() || !limits->menuAction()->isVisible()) return 50;
    const QPoint position = root->rect().center();
    QContextMenuEvent context(QContextMenuEvent::Mouse, position, root->mapToGlobal(position));
    QApplication::sendEvent(root, &context);
    if (QApplication::activePopupWidget() != mainMenu || !mainMenu->actions().contains(limits->menuAction()))
        return 52;
    mainMenu->close();
    root->activateWindow();
    QTest::qWait(30);
    QTest::keyClick(root, Qt::Key_H);
    if (!settings->isVisible() || limits->menuAction()->isVisible()) return 51;
    root->resize(400, 510);
    update(R"({"recording":"recording","duration":3984,"motion":"rife","quality":"FBCNN · 保持增强画质"})");
    if (!checkSingleRow() || !timer->isVisible() || !gen->isVisible()) return 27;
    update(R"({"recording":"idle","motion":"flow","quality":"FBCNN"})");
    if (!checkSingleRow() || !hasLimitControls()) return 28;
    quick->menu()->actions().first()->trigger();
    cap->menu()->actions().first()->trigger();
    gen->click();
    auto changes = QJsonDocument(actions()).toJson(QJsonDocument::Compact);
    if (!changes.contains("\"fps\":24") || !changes.contains("\"rate\":600000") || !changes.contains("\"type\":\"motion\""))
        return 17;
    QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(info, &enter);
    if (!card->isVisible() || !card->text().contains("29.5") || !card->text().contains("28.0")
        || !card->parentWidget()->rect().contains(card->geometry())) return 18;
    update(R"({"status":"源 19.0 FPS","quality":"FBCNN","motion":"off","debug":{"time":"测试示例","connection":"live","motion":"off","fps_source_presented":19,"fps_presented":19,"fps_generated":0,"fps_received":24,"device_sent_fps":30,"bytes_per_s":620000,"device_cap":800000,"jpeg_top_q":27,"jpeg_bottom_q":27,"processing_ms":42,"backend":"DirectML","playout_delay_ms":160,"source_interval_max_gap_ms":143,"frame_health":{"label":"多环节","evidence":["PC 有 6 对双屏缺片未收齐。需对照掌机和路由器记录。","PC 收齐 24.0、显示 19.0 个源帧/秒，增强队列跳过 4 帧。"]}}})");
    if (info->text() != "多环节" || !checkSingleRow() || !card->isVisible() ||
        !card->text().contains("6 对") || !card->text().contains("4 帧") ||
        !card->parentWidget()->rect().contains(card->geometry()) ||
        info->width() < info->fontMetrics().horizontalAdvance(info->text())) return 53;
    if (argc == 5 && QByteArray(argv[1]) == "--frame-health-preview") {
        QImage upper(QString::fromLocal8Bit(argv[2])), lower(QString::fromLocal8Bit(argv[3]));
        if (upper.size() != QSize(400,240) || lower.size() != QSize(320,240)) return 55;
        QImage frame(400,480,QImage::Format_RGB888);
        frame.fill(Qt::black);
        QPainter painter(&frame);
        painter.drawImage(0,0,upper);
        painter.drawImage(40,240,lower);
        painter.end();
        live::showFrame(view, frame);
        if (!root->grab().save(QString::fromLocal8Bit(argv[4]))) return 56;
    }
    info->click();
    QApplication::sendEvent(info, &leave);
    if (!card->isVisible()) return 19;
    auto *copyShot = root->findChild<QAction *>("copyPerformanceShot");
    if (!copyShot) return 21;
    root->activateWindow();
    QTest::qWait(30);
    QTest::keyClick(root, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    const auto screenshot = QApplication::clipboard()->pixmap();
    if (screenshot.isNull() || screenshot.deviceIndependentSize().toSize() != card->parentWidget()->size()) return 22;
    update(R"({"debug":{"connection":"live","frame_health":{}}})");
    if (info->text() != "ⓘ" || card->text().contains("缺片") || !checkSingleRow()) return 54;
    info->click();
    QApplication::sendEvent(info, &leave);
    update(R"({"fps":24,"rate":600000,"motion":"off"})");
    if (!card->isHidden() || !quick->menu()->actions().first()->isChecked() ||
        !cap->menu()->actions().first()->isChecked() || gen->text().contains("DIS")) return 20;
    auto *dialog = root->findChild<QDialog *>("streamSettings");
    auto *motion = root->findChild<QComboBox *>("motionMethod");
    auto *precision = root->findChild<QComboBox *>("rifePrecision");
    if (root->findChild<QComboBox *>("screenRateMode") || root->findChild<QCheckBox *>("bottomInterpolate"))
        return 57;
    if (!dialog || !motion || !precision || precision->currentData().toString() != "fp32" || precision->isEnabled())
        return 32;
    root->activateWindow();
    QTest::qWait(30);
    QTest::keyClick(root, Qt::Key_Comma, Qt::ControlModifier);
    auto *tabs = dialog->findChild<QTabWidget *>("streamSettingsTabs");
    if (!dialog->isVisible() || !tabs || tabs->count() != 2 || tabs->currentIndex() != 0) return 33;
    dialog->hide();
    for (auto *action : motionMenu->actions())
        if (action->text() == "补帧设置…") action->trigger();
    if (!dialog->isVisible() || tabs->currentIndex() != 1 || !motion->isVisible()) return 53;
    motion->setCurrentIndex(motion->findData("rife"));
    if (!precision->isEnabled()) return 34;
    precision->setCurrentIndex(precision->findData("fp16"));
    dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
    auto applied = actions();
    if (applied.size() != 1 || applied[0].toObject()["type"] != "apply" ||
        applied[0].toObject()["motion"] != "rife" || applied[0].toObject()["rife_precision"] != "fp16" ||
        applied[0].toObject().contains("top_priority") || applied[0].toObject().contains("bottom_interpolate"))
        return 35;
    update(R"({"motion":"rife","rife_precision":"fp16","debug":{"motion":"rife","rife_precision":"fp16"}})");
    info->click();
    if (!card->text().contains("设置 FP16") || !card->text().contains("等待推理")) return 36;
    update(R"({"debug":{"motion":"rife","rife_precision":"fp16","motion_backend":"TensorRT FP16","motion_processing_ms":6.4}})");
    if (!card->text().contains("实际 TensorRT FP16") || !card->text().contains("6.4 ms") || card->text().contains("回退"))
        return 37;
    update(R"({"debug":{"motion":"rife","rife_precision":"fp16","motion_backend":"DirectML","motion_backend_fallback":"Missing FP16 model"}})");
    if (!card->text().contains("设置 FP16") || !card->text().contains("实际 DirectML FP32") ||
        !card->text().contains("已回退") || !checkSingleRow()) return 38;
    update(R"({"debug":{"motion":"rife","rife_precision":"fp16","motion_error":"Missing models"}})");
    if (!card->text().contains("推理失败") || card->text().contains("等待推理")) return 40;
    update(R"({"motion":"flow","rife_precision":"fp32"})");
    if (precision->isEnabled() || precision->currentData().toString() != "fp32") return 39;
    live::destroyView(view);
    return 0;
}
