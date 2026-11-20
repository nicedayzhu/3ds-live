#include "client.h"
#include "receiver.h"
#include "transport.h"
#include <QtWidgets>
#include <QtNetwork>
#include <QtTest>

static QByteArray jpeg(int width, QColor color) {
    QImage image(width, 240, QImage::Format_RGB888);
    image.fill(color);
    QByteArray result;
    QBuffer buffer(&result);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 80);
    return result;
}

static int checkRifePreferences() {
    QTemporaryDir folder;
    if (!folder.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, folder.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, folder.path());
    qunsetenv("THREE_DS_LIVE_RIFE_PRECISION");
    char name[] = "test_session";
    char *args[]{name, nullptr};
    int argc = 1;
    QApplication app(argc, args);
    app.setOrganizationName("3ds-recorder");
    app.setApplicationName("3DS Live");
    const auto ini = QDir(folder.path()).relativeFilePath(QSettings().fileName());
    if (ini.startsWith("..") || QDir::isAbsolutePath(ini)) return 1;
    QSettings().setValue("ip", "127.0.0.1");
    QUdpSocket port;
    if (!port.bind(QHostAddress::LocalHost, 0)) return 2;
    QSettings().setValue("port", port.localPort());
    port.close();
    for (const QString &expected : QStringList{"fp32", "fp16", "fp32"}) {
        bool loaded = false;
        QTimer::singleShot(0, &app, [&] {
            for (auto *window : app.topLevelWidgets()) {
                auto *dialog = window->findChild<QDialog *>("streamSettings");
                if (!dialog) continue;
                auto *precision = dialog->findChild<QComboBox *>("rifePrecision");
                auto *motion = dialog->findChild<QComboBox *>("motionMethod");
                auto *buttons = dialog->findChild<QDialogButtonBox *>("settingsButtons");
                auto *ip = dialog->findChild<QLineEdit *>("senderIp");
                if (!precision || !motion || !buttons || !ip) break;
                loaded = precision->currentData().toString() == expected && ip->text().isEmpty();
                // Change the saved precision without starting GPU inference on this test host.
                motion->setCurrentIndex(motion->findData("rife"));
                precision->setCurrentIndex(precision->findData(expected == "fp32" ? "fp16" : "fp32"));
                motion->setCurrentIndex(motion->findData("off"));
                buttons->button(QDialogButtonBox::Apply)->click();
                break;
            }
        });
        QTimer::singleShot(200, &app, &QApplication::quit);
        if (runClient(app, 0) != 0 || !loaded) return 3;
    }
    return 0;
}

static int checkReceiverHandover(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QHostAddress local(QHostAddress::LocalHost), moved("127.0.0.2");
    QUdpSocket first, second, reserve;
    if (!first.bind(local, 0) || !second.bind(moved, 0) || !reserve.bind(local, 0))
        return 1;
    const auto port = reserve.localPort();
    reserve.close();
    auto status = [&](QUdpSocket &source, quint32 elapsed) {
        source.writeDatagram(live::message("R3ST", {81, elapsed, 600000, 600000, 30, 0, 0, 0,
                                                   0, 0, 0, 0, 0, 0, 0, 0}), local, port);
    };
    auto gotControl = [](QUdpSocket &source) {
        bool received = false;
        while (source.hasPendingDatagrams()) {
            auto data = source.receiveDatagram().data();
            received |= data.startsWith("R6CF") || data.startsWith("R10C");
        }
        return received;
    };
    auto awaitControl = [&](QUdpSocket &source, quint32 elapsed) {
        for (int i = 0; i < 30; ++i) {
            status(source, elapsed + i);
            QTest::qWait(20);
            if (gotControl(source)) return true;
        }
        return false;
    };
    live::Timeline timeline(100);
    live::Pipeline pipeline(timeline, {}, 1, "off", "off");
    live::Receiver receiver(timeline, pipeline, {}, port, 30, 800000, 0);
    if (!QTest::qWaitFor([&]{ return receiver.stats()["udp_port"].toInt() == port; }, 2000))
        return 2;
    if (!awaitControl(first, 1000)) return 2;
    for (unsigned i = 0; i < 10; ++i) {
        status(first, 2000 + i);
        status(second, 3000 + i);
        QTest::qWait(30);
    }
    if (gotControl(second)) return 3;
    QTest::qWait(260);
    status(second, 1000); // An old status, even after silence, cannot move the peer.
    QTest::qWait(60);
    if (gotControl(second)) return 4;
    if (!awaitControl(second, 4000)) return 5;
    const auto top = jpeg(400, Qt::cyan), bottom = jpeg(320, Qt::green), both = top + bottom;
    if (top.isEmpty() || bottom.isEmpty()) {
        qWarning() << "Failed to encode the handover test frames; Qt plugin paths:" << app.libraryPaths();
        return 6;
    }
    QElapsedTimer sourceClock;
    sourceClock.start();
    auto sendPair = [&](QUdpSocket &source, unsigned seq) {
        const auto pts = quint32(sourceClock.nsecsElapsed() / 1000);
        for (int offset = 0; offset < both.size(); offset += live::Chunk) {
            auto header = live::message("R10P", {81, seq, pts, quint32(top.size()),
                                        quint32(bottom.size()), quint32(offset / live::Chunk), 30});
            source.writeDatagram(header + both.mid(offset, live::Chunk), local, port);
        }
    };
    for (unsigned seq = 0; seq < 10; ++seq) {
        sendPair(second, seq);
        QTest::qWait(20);
    }
    QTest::qWait(300);
    status(first, 2009);
    QTest::qWait(1100); // Include a complete reporting interval after the stale packet.
    auto follows = [&](const QHostAddress &ip, int pairs, int changes) {
        const auto stats = receiver.stats();
        return stats["pairs"].toInt() == pairs && stats["peer_changes"].toInt() == changes &&
               stats["jpeg_full_screens"].toInt() == 2 * pairs && stats["jpeg_header_saving_pct"].toDouble(-1) == 0 &&
               QHostAddress(stats["peer"].toString()).toIPv4Address() == ip.toIPv4Address();
    };
    if (!follows(moved, 10, 1)) {
        qWarning() << "Unexpected statistics after source address changed" << receiver.stats();
        return 6;
    }
    QTest::qWait(1800); // A legacy video-only peer can still rediscover after three seconds.
    sendPair(first, 10);
    sendPair(first, 11);
    QTest::qWait(1100);
    if (!follows(local, 12, 2)) return 7;
    // A previously configured R15 may still send split packets until it
    // receives and acknowledges our request for equal capture rates.
    receiver.configure(30, 800000);
    auto capability = [&](unsigned active) {
        first.writeDatagram(live::message("R15S", {81, 1, 1, active}), local, port);
    };
    auto splitCommand = [&](unsigned desired, unsigned active) {
        for (unsigned i = 0; i < 60; ++i) {
            status(first, 10000 + i);
            capability(active);
            QTest::qWait(20);
            while (first.hasPendingDatagrams()) {
                auto packet = first.receiveDatagram().data();
                if (packet.size() == 24 && packet.startsWith("R15C") && live::word(packet, 0) == 81 &&
                    live::word(packet, 2) == 30 && live::word(packet, 3) == 800000 &&
                    live::word(packet, 4) == desired) return true;
            }
        }
        qWarning() << "R15 control timeout, requested mode" << desired << receiver.stats();
        return false;
    };
    if (!splitCommand(0, 1)) return 8;
    capability(1);
    auto splitSlot = [&](unsigned seq, bool carryBottom, unsigned bottomPts) {
        const auto payload = carryBottom ? both : top;
        const auto pts = quint32(sourceClock.nsecsElapsed() / 1000);
        for (int off = 0; off < payload.size(); off += live::Chunk) {
            auto header = live::message("R15P", {81, seq, pts, quint32(top.size()),
                                          carryBottom ? quint32(bottom.size()) : 0u,
                                          quint32(off / live::Chunk), 30, bottomPts});
            first.writeDatagram(header + payload.mid(off, live::Chunk), local, port);
        }
    };
    splitSlot(12, false, 0);
    const auto bottomPts = quint32(sourceClock.nsecsElapsed() / 1000) - 1000;
    splitSlot(13, true, bottomPts);
    splitSlot(14, false, 0);
    splitSlot(15, true, bottomPts); // A reconnect refresh of the same lower source.
    QTest::qWait(1100);
    auto splitStats = receiver.stats();
    if (!splitStats["split_supported"].toBool() || !splitStats["split_active"].toBool() ||
        splitStats["pairs"].toInt() != 14 || splitStats["top_received"].toInt() != 16 ||
        splitStats["bottom_received"].toInt() != 13 || splitStats["incomplete"].toInt() ||
        splitStats["jpeg_full_screens"].toInt() != 30) {
        qWarning() << "Unexpected R15 statistics" << splitStats;
        return 9;
    }
    if (!splitCommand(0, 1)) return 10;

    // An audio sender at a low cap can fail to fit a single pair. Its status
    // has a monotonic device clock even when UDP reports arrive out of order.
    constexpr unsigned supported = (1u << 20) | (1u << 30);
    auto audioStatus = [&](unsigned session, unsigned elapsed, unsigned cap, unsigned sent) {
        first.writeDatagram(live::message("R9ST", {session, elapsed, cap, cap - 100000, 30, sent, sent,
            sent, sent, 0, 0, 0, 2, 3, 120, 180, 2000000, supported, 1, 32728498, 2, 160, 0, 0, 0, 0}),
            local, port);
    };
    auto selected = [&](unsigned session, unsigned elapsed, int fps) {
        const bool reached = QTest::qWaitFor([&] {
            const auto report = receiver.stats();
            return report["session"].toInteger() == session && report["device_elapsed_ms"].toInteger() == elapsed &&
                   report["effective_fps"].toInt() == fps;
        }, 1800);
        if (!reached) qWarning() << "Capture cadence did not reach" << session << elapsed << fps << receiver.stats();
        return reached;
    };
    receiver.configure(30, 600000);
    audioStatus(82, 1000, 600000, 50);
    if (!selected(82, 1000, 30)) return 11;
    audioStatus(82, 2000, 600000, 50);
    audioStatus(82, 1700, 600000, 50);
    audioStatus(82, 2000, 600000, 50);
    if (!selected(82, 2000, 30)) return 12;
    audioStatus(82, 3000, 600000, 50);
    if (!selected(82, 3000, 20)) return 13;
    audioStatus(83, 1000, 600000, 50);
    if (!selected(83, 1000, 30)) return 14;
    receiver.configure(30, 800000);
    for (unsigned i = 0; i < 4; ++i)
        audioStatus(83, 2000 + i * 1000, 800000, 50);
    return selected(83, 5000, 30) ? 0 : 15;
}

static int checkDiscovery(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QHostAddress local("127.0.0.1"), moved("127.0.0.2");
    QUdpSocket first, second, reserve;
    if (!first.bind(local, 0) || !second.bind(moved, 0) || !reserve.bind(local, 0))
        return 1;
    const auto port = reserve.localPort();
    reserve.close();
    live::Timeline timeline(100);
    live::Pipeline pipeline(timeline, {}, 1, "off", "off");
    const auto probe = live::message("3DLS", {81, 0x1234abcd});
    const auto heartbeat = live::message("3DLS", {81, 0x5678fedc});
    auto exchange = [&](QUdpSocket &source, const QByteArray &packet, int wait) {
        if (source.writeDatagram(packet, local, port) != packet.size())
            return QByteArray("send failed");
        if (!source.hasPendingDatagrams() && !source.waitForReadyRead(wait))
            return QByteArray{};
        auto response = source.receiveDatagram();
        if (response.data() != QByteArray("3DLA") + packet.mid(4))
            qWarning() << "Unexpected discovery reply" << response.data().toHex() << "after" << packet.toHex();
        return response.senderPort() == port ? response.data() : QByteArray("wrong port");
    };
    auto listening = [&](live::Receiver &receiver) {
        return QTest::qWaitFor([&]{ return receiver.stats()["udp_port"].toInt() == port; }, 2000);
    };
    auto discover = [&](QUdpSocket &source) {
        for (int i = 0; i < 20; ++i)
            if (exchange(source, probe, 60) == QByteArray("3DLA") + probe.mid(4)) return true;
        return false;
    };
    {
        live::Receiver receiver(timeline, pipeline, {}, port, 30, 800000, 0);
        if (!listening(receiver) || !discover(first)) return 2;
        if (!exchange(first, probe + 'x', 100).isEmpty()) return 3;
        if (!exchange(second, probe, 100).isEmpty()) return 4;
        if (exchange(first, heartbeat, 1000) != QByteArray("3DLA") + heartbeat.mid(4)) return 5;
        if (receiver.stats()["last_packet_us"].toInteger() != 0) return 6;
        // Closing the current console socket elicits ICMP port-unreachable for
        // subsequent feedback. The listener must keep its fixed port for the next console.
        first.writeDatagram(live::message("R3ST", {81, 1000, 600000, 600000, 30, 0, 0, 0,
                                                   0, 0, 0, 0, 0, 0, 0, 0}), local, port);
        if (!QTest::qWaitFor([&]{ return receiver.stats()["last_packet_us"].toInteger() > 0; }, 1500))
            return 10;
        first.close();
        QTest::qWait(4100);
        if (!discover(second)) return 7;
    }
    // Emulate the handheld locator on loopback. No test sends LAN broadcasts.
    QUdpSocket locator;
    if (!locator.bind(local, 0)) return 11;
    live::Receiver pinned(timeline, pipeline, local, port, 30, 800000, locator.localPort());
    auto receivedLocate = [&](int timeout) {
        if (!locator.hasPendingDatagrams() && !locator.waitForReadyRead(timeout)) return false;
        const auto packet = locator.receiveDatagram();
        return packet.senderPort() == port && packet.senderAddress().toIPv4Address() == local.toIPv4Address() &&
               packet.data() == QByteArray::fromHex("33444c5001000000");
    };
    if (!first.bind(local, 0) || !listening(pinned) || !receivedLocate(1500) || !discover(first)) return 8;
    if (locator.waitForReadyRead(1300)) return 12;
    if (!receivedLocate(4000)) return 13;
    return exchange(second, probe, 200).isEmpty() ? 0 : 9;
}

static int checkPipelineCapabilities(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir emptyModels;
    if (!emptyModels.isValid()) return 1;
    live::Timeline timeline(0);
    live::Pipeline pipeline(timeline, emptyModels.path(), 1, "off", "off");
    pipeline.configure("fbcnn", "rife", 1, "fp16");
    auto failedWithoutStream = [&] {
        auto s = pipeline.stats();
        return !s["model_error"].toString().isEmpty() && !s["motion_error"].toString().isEmpty() &&
               !s["model_preparing"].toBool() && !s["motion_preparing"].toBool();
    };
    if (!QTest::qWaitFor(failedWithoutStream, 5000)) return 2;
    const auto firstFallback = pipeline.stats()["motion_backend_fallback"].toString();
    if (!firstFallback.contains("DirectML FP16:") || !firstFallback.contains("DirectML:")) return 3;
    // A stale saved preference or a driver failure must leave real video visible.
    pipeline.resetSession(7);
    live::Pair pair;
    pair.screens = {jpeg(400, Qt::red), jpeg(320, Qt::blue)};
    pipeline.submit(pair, timeline.arrival(7, 1, 0, live::clockUs()));
    if (!QTest::qWaitFor([&] { return timeline.present().quality == "original"; }, 2000)) return 4;
    pipeline.configure("clean", "off", 1, "fp16");
    auto s = pipeline.stats();
    if (!s["model_error"].toString().isEmpty() || !s["motion_error"].toString().isEmpty()) return 5;
    pipeline.configure("fbcnn", "rife", 1, "fp16");
    if (!QTest::qWaitFor(failedWithoutStream, 5000)) return 6;
    s = pipeline.stats();
    if (s["motion_backend_fallback"].toString() != firstFallback) return 7;
    if (s["restored_pairs"].toInteger() || s["restore_total_us"].toInteger() ||
        s["interpolated"].toInteger() || s["motion_work_total_us"].toInteger()) return 8;
    pipeline.configure("span", "off", 2);
    if (!QTest::qWaitFor([&] {
        const auto state = pipeline.stats();
        const auto error = state["model_error"].toString();
        return error.contains("span-fp16.onnx") && error.contains("span.onnx") &&
               state["backend"].toString().isEmpty() && state["motion_error"].toString().isEmpty();
    }, 5000)) return 9;
    return 0;
}

static int checkEffectsFallback(const QByteArray &pictureMode) {
    QTemporaryDir work;
    if (!work.isValid()) return 1;
    const QString stats = work.filePath("effects.jsonl");
    QUdpSocket reserve;
    if (!reserve.bind(QHostAddress::LocalHost, 0)) return 1;
    const auto port = QByteArray::number(reserve.localPort());
    reserve.close();
    QList<QByteArray> args{"test_session", "--preview", "--sender", "127.0.0.1", "--port", port, "--mute",
        "--enhance", pictureMode, "--interpolate", "rife", "--models", work.path().toLocal8Bit(),
        "--stats", stats.toLocal8Bit(), "--seconds", "12"};
    QVector<char *> argv;
    for (auto &arg : args) argv.append(arg.data());
    int argc = int(argv.size());
    argv.append(nullptr);
    QApplication app(argc, argv.data());
    auto choice = [](QMenu *menu, const QString &id) -> QAction * {
        if (menu)
            for (auto *a : menu->actions())
                if (a->data().toString() == id) return a;
        return nullptr;
    };
    bool retried = false, reenabled = false, fellBackAgain = false;
    QTimer observe;
    QObject::connect(&observe, &QTimer::timeout, &app, [&] {
        for (QWidget *view : app.topLevelWidgets()) {
            if (view->windowTitle() != "3DS Live") continue;
            auto *pictures = view->findChild<QMenu *>("pictureMenu");
            auto *methods = view->findChild<QMenu *>("motionMenu");
            auto *picture = choice(pictures, QString::fromLatin1(pictureMode)), *clean = choice(pictures, "clean");
            auto *rife = choice(methods, "rife"), *flow = choice(methods, "flow");
            if (!picture || picture->isEnabled() || !rife || rife->isEnabled() ||
                !clean || !clean->isChecked() || !flow || !flow->isChecked()) return;
            if (!retried) {
                auto *open = view->findChild<QAction *>("effectSettings");
                auto *retry = view->findChild<QPushButton *>("retryEffects");
                if (!open || !open->isVisible() || !retry) return;
                open->trigger();
                if (!retry->isVisible()) return;
                QObject::connect(picture, &QAction::changed, &app, [&, picture] {
                    if (picture->isEnabled()) reenabled = true;
                });
                retried = true;
                retry->click();
            } else if (reenabled) {
                fellBackAgain = true;
                observe.stop();
                // Let the next regular per-second stats record the selected and effective modes.
                QTimer::singleShot(1200, &app, &QApplication::quit);
            }
            return;
        }
    });
    observe.start(25);
    if (runClient(app, 0) || !fellBackAgain) {
        qWarning() << "Effect fallback retry" << retried << reenabled << fellBackAgain;
        return 2;
    }
    QFile reports(stats);
    if (!reports.open(QIODevice::ReadOnly)) return 3;
    while (!reports.atEnd()) {
        const auto report = QJsonDocument::fromJson(reports.readLine()).object();
        const auto id = QString::fromLatin1(pictureMode);
        if (report["requested_mode"] == id && report["requested_motion"] == "rife" &&
            report["mode"] == "clean" && report["motion"] == "flow" &&
            report["picture_failures"].toObject().contains(id) &&
            report["motion_failures"].toObject().contains("rife")) return 0;
    }
    return 4;
}

int main(int testArgc, char **testArgv) {
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--effects-fallback")
        return checkEffectsFallback("fbcnn");
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--span-fallback")
        return checkEffectsFallback("span");
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--pipeline-capabilities")
        return checkPipelineCapabilities(testArgc, testArgv);
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--discovery")
        return checkDiscovery(testArgc, testArgv);
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--receiver-handover")
        return checkReceiverHandover(testArgc, testArgv);
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--rife-preferences")
        return checkRifePreferences();
    QTemporaryDir work;
    if (!work.isValid())
        return 1;
    QUdpSocket occupied;
    if (!occupied.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::DontShareAddress))
        return 2;
    const auto port = occupied.localPort();
    const auto stats = work.filePath("session.jsonl"), trace = work.filePath("choices.jsonl"),
               mp4 = work.filePath("session.mp4");
    QList<QByteArray> args = {
        "test_session",           "--preview", "--sender",        "127.0.0.1",         "--port",
        QByteArray::number(port), "--mute",    "--stats",         stats.toLocal8Bit(), "--trace",
        trace.toLocal8Bit(),      "--record",  mp4.toLocal8Bit(), "--seconds",         "15"};
    QVector<char *> argv;
    for (auto &arg : args)
        argv.append(arg.data());
    int argc = int(argv.size());
    argv.append(nullptr);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv.data());
    QPointer<QWidget> view;
    bool retry = false, settings = false, marked = false, hidden = false, restored = false, minimized = false,
         quality = false, explicitFlow = false;
    QTimer::singleShot(100, [&] {
        for (auto w : app.topLevelWidgets())
            if (w->windowTitle() == "3DS Live")
                view = w;
    });
    auto trigger = [&](const QString &label) {
        if (!view)
            return false;
        for (auto a : view->findChildren<QAction *>())
            if (a->text() == label) {
                a->trigger();
                return true;
            }
        return false;
    };
    auto triggerShortcut = [&](const QKeySequence &shortcut) {
        if (!view) return;
        for (auto *action : view->findChildren<QAction *>())
            if (action->shortcut() == shortcut) {
                action->trigger();
                return;
            }
    };
    QTimer::singleShot(1150, [&] {
        occupied.close();
        retry = trigger("重新监听");
        settings = trigger("投屏设置…");
        if (view)
            if (auto d = view->findChild<QDialog *>("streamSettings")) {
                settings &= d->isVisible();
                d->hide();
            }
    });
    const QByteArray top = jpeg(400, Qt::cyan), bottom = jpeg(320, Qt::green), both = top + bottom;
    QUdpSocket sender;
    bool singleRate = false, dualRate = false, gotCaptureRates = false;
    QString lastStatus;
    QTimer cadenceObserver;
    QObject::connect(&cadenceObserver, &QTimer::timeout, [&] {
        auto *label = view ? view->findChild<QLabel *>("connectionStatus") : nullptr;
        if (!label || !label->isVisible()) return;
        lastStatus = label->text();
        singleRate |= lastStatus.startsWith("源 ");
        dualRate |= lastStatus.startsWith("显示 ") && lastStatus.contains("源 ");
    });
    cadenceObserver.start(100);
    auto send = [&](quint32 session, quint32 seq, bool drop) {
        for (int offset = 0; offset < both.size(); offset += live::Chunk) {
            if (drop && offset > 0)
                break;
            QByteArray packet =
                live::message("R10P", {session, seq, seq * 33333, quint32(top.size()), quint32(bottom.size()),
                                       quint32(offset / live::Chunk), 30}) +
                both.mid(offset, live::Chunk);
            sender.writeDatagram(packet, QHostAddress::LocalHost, port);
        }
        if (seq == 3 || seq == 35) {
            auto extra = live::message("R13S", {session,seq*33,seq,seq,seq,seq,0,0,0,0,0,0,0,0,seq,seq});
            sender.writeDatagram(extra,QHostAddress::LocalHost,port);
        }
    };
    for (int i = 0; i < 65; ++i) {
        QTimer::singleShot(2350 + i * 33, [&, i] { send(13, i, i == 10); });
        QTimer::singleShot(7250 + i * 33, [&, i] { send(29, i, false); });
    }
    QTimer::singleShot(2800, [&] { quality = trigger("轻度增强"); });
    auto selectFlow = [&] {
        auto *menu = view ? view->findChild<QMenu *>("motionMenu") : nullptr;
        if (menu)
            for (auto *action : menu->actions())
                if (action->data().toString() == "flow") action->trigger();
    };
    QTimer::singleShot(4200, selectFlow);
    QTimer::singleShot(4450, selectFlow);
    QTimer::singleShot(4800, [&] {
        auto *choice = view ? view->findChild<QComboBox *>("motionMethod") : nullptr;
        explicitFlow = choice && choice->currentData().toString() == "flow";
    });
    QTimer::singleShot(3150, [&] {
        if (!view)
            return;
        triggerShortcut(Qt::Key_H);
        auto button = view->findChild<QToolButton *>("record");
        hidden = button && !button->isVisible();
        triggerShortcut(Qt::Key_H);
        restored = button && button->isVisible();
        triggerShortcut(Qt::Key_F11);
        restored &= view->isFullScreen();
        triggerShortcut(Qt::Key_F11);
        restored &= !view->isFullScreen();
        view->showMinimized();
        minimized = view->isMinimized();
    });
    QTimer::singleShot(8100, [&] {
        if (!view)
            return;
        view->showNormal();
        triggerShortcut(Qt::Key_F12);
        auto *button = view->findChild<QPushButton *>("markStall");
        if (!button) return;
        button->click();
        auto *debug = button->window();
        auto freeze = debug->findChild<QCheckBox *>();
        auto table = debug->findChild<QTableWidget *>();
        marked = freeze && freeze->isChecked() && table && table->rowCount() > 4;
        for (auto button : view->findChildren<QPushButton *>())
            if (button->text() == "复制 JSON") {
                button->click();
                auto archived = QJsonDocument::fromJson(app.clipboard()->text().toUtf8()).object();
                const auto archiveEvents = archived["events"].toArray();
                marked &= archived["version"].isString() &&
                          std::any_of(archiveEvents.begin(), archiveEvents.end(),
                                      [](const QJsonValue &e) { return e.toObject()["event"] == "mark"; });
            }
    });
    QTimer::singleShot(8450, [&] {
        if (!view) return;
        if (auto save = view->findChild<QPushButton *>("exportDebug")) save->click();
        if (auto dialog = view->findChild<QFileDialog *>()) {
            dialog->selectFile(work.filePath("frozen.json"));
            QMetaObject::invokeMethod(dialog,"accept");
        }
    });
    QTimer::singleShot(10300, [&] { if (view) view->close(); });
    if (runClient(app, 0))
        return 3;
    QFile saved(work.filePath("frozen.json"));
    if (!saved.open(QIODevice::ReadOnly)) return 6;
    const auto exported=QJsonDocument::fromJson(saved.readAll()).object();
    if (exported["events"].toArray().isEmpty() || exported["samples"].toArray().isEmpty() || !exported.contains("version")) return 7;
    QFile report(stats), ledger(trace);
    if (!report.open(QIODevice::ReadOnly) || !ledger.open(QIODevice::ReadOnly))
        return 4;
    QStringList states;
    while (!report.atEnd()) {
        const auto sample = QJsonDocument::fromJson(report.readLine()).object();
        gotCaptureRates |= sample["device_protocol"] == "R13" && sample["captured_top_fps"].toDouble() > 29;
        auto s = sample["connection"].toString();
        if (!s.isEmpty() && (states.isEmpty() || s != states.last()))
            states.append(s);
    }
    auto interrupted = states.indexOf("interrupted");
    bool reconnected = interrupted >= 0 && states.indexOf("live", interrupted + 1) > interrupted;
    QMap<qint64, QJsonObject> frames;
    QList<QJsonObject> encoded;
    QSet<qint64> sessions;
    QSet<QString> qualities;
    bool sawLoss = false;
    while (!ledger.atEnd()) {
        auto e = QJsonDocument::fromJson(ledger.readLine()).object();
        auto kind = e["event"].toString();
        if (kind == "select" || kind == "select_bottom" || kind == "select_revision" ||
            kind == "synthetic" || kind == "synthetic_bottom" || kind == "clear") {
            frames[e["wall_us"].toInteger()] = e;
            sessions.insert(e["session"].toInteger());
            qualities.insert(e["quality"].toString());
        }
        if (kind == "incomplete" && e["sequence"] == 10 && e["missing_fragments"].toInt() > 0)
            sawLoss = true;
        if (kind == "encode")
            encoded.append(e);
    }
    int mismatches = 0, held = 0;
    qint64 last = 0;
    for (const auto &e : encoded) {
        auto it = frames.upperBound(e["target_us"].toInteger());
        if (it == frames.begin()) {
            mismatches += e["choice"].toInteger() != 0;
            continue;
        }
        --it;
        for (auto key : {"choice", "session", "sequence", "quality", "image_key"})
            mismatches += e[key] != it.value()[key];
        if (e["choice"].toInteger() == last && e["session"] == 13 && e["sequence"] == 64)
            ++held;
        last = e["choice"].toInteger();
    }
    bool ok = retry && settings && marked && hidden && restored && minimized && quality && reconnected &&
              sawLoss && states.contains("bind_error") && states.contains("waiting") && sessions.contains(13) &&
              sessions.contains(29) && qualities.contains("original") && qualities.contains("clean") &&
              !mismatches && held > 30 && QFileInfo(work.filePath("session.mp4")).size() > 1024;
    if (!ok)
        qCritical() << states << retry << settings << marked << hidden << restored << minimized << quality
                    << sessions << qualities << sawLoss << mismatches << held;
    if (!singleRate || !dualRate || !gotCaptureRates || !explicitFlow) {
        qCritical() << "FPS display / device counters / motion selection" << singleRate << dualRate << gotCaptureRates << explicitFlow << lastStatus;
        return 8;
    }
    return ok ? 0 : 5;
}
