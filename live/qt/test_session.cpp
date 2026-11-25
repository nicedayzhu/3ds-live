#include "client.h"
#include "receiver.h"
#include "transport.h"
#include "test_jpeg_rows.h"
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
    int expectedJpeg = 0;
    int expectedRate = 800000;
    bool expectedAdaptive = true;
    int launchIndex = 0;
    for (const QString &expected : QStringList{"fp32", "fp16", "fp32"}) {
        if (launchIndex) QSettings().setValue("fps", launchIndex == 1 ? 35 : 40);
        ++launchIndex;
        bool loaded = false;
        QTimer::singleShot(0, &app, [&] {
            for (auto *window : app.topLevelWidgets()) {
                auto *dialog = window->findChild<QDialog *>("streamSettings");
                if (!dialog) continue;
                auto *precision = dialog->findChild<QComboBox *>("rifePrecision");
                auto *jpegQuality = dialog->findChild<QComboBox *>("jpegQuality");
                auto *rateMode = dialog->findChild<QComboBox *>("rateMode");
                auto *rate = dialog->findChild<QComboBox *>("sourceRate");
                auto *fps = dialog->findChild<QComboBox *>("sourceFps");
                auto *presets = dialog->findChild<QToolButton *>("streamPresets");
                auto *motion = dialog->findChild<QComboBox *>("motionMethod");
                auto *buttons = dialog->findChild<QDialogButtonBox *>("settingsButtons");
                auto *ip = dialog->findChild<QLineEdit *>("senderIp");
                if (!precision || !jpegQuality || !rateMode || !rate || !fps || !presets || !motion || !buttons || !ip) break;
                loaded = precision->currentData().toString() == expected && ip->text().isEmpty() &&
                         jpegQuality->currentData().toInt() == expectedJpeg &&
                         rate->currentData().toInt() == expectedRate && fps->currentData().toInt() == 30 &&
                         fps->findData(35) == -1 && fps->findData(40) == -1 &&
                         rateMode->currentData().toBool() == expectedAdaptive;
                expectedJpeg = expectedJpeg == 0 ? 60 : 0;
                expectedAdaptive = !expectedAdaptive;
                if (expectedRate == 800000) {
                    for (auto *action : presets->menu()->actions())
                        if (action->data().toString() == "balanced30") action->trigger();
                    expectedRate = 2000000;
                    expectedJpeg = 60;
                    loaded &= jpegQuality->currentData().toInt() == expectedJpeg;
                }
                jpegQuality->setCurrentIndex(jpegQuality->findData(expectedJpeg));
                rateMode->setCurrentIndex(rateMode->findData(expectedAdaptive));
                // Change the saved precision without starting GPU inference on this test host.
                motion->setCurrentIndex(motion->findData("rife"));
                precision->setCurrentIndex(precision->findData(expected == "fp32" ? "fp16" : "fp32"));
                motion->setCurrentIndex(motion->findData("off"));
                buttons->button(QDialogButtonBox::Apply)->click();
                break;
            }
        });
        QTimer::singleShot(200, &app, &QApplication::quit);
        if (runClient(app, 0) != 0 || !loaded || QSettings().value("fps", 30).toInt() != 30 ||
            QSettings().value("jpegQuality", -1).toInt() != expectedJpeg ||
            QSettings().value("rate").toInt() != expectedRate ||
            QSettings().value("adaptiveRate").toBool() != expectedAdaptive) return 3;
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

    // A stalled audio sender keeps the user's cadence, including after reordered
    // status packets and when a new session starts.
    constexpr unsigned supported = (1u << 20) | (1u << 30);
    auto audioStatus = [&](unsigned session, unsigned elapsed, unsigned cap, unsigned sent, QUdpSocket *source = nullptr) {
        auto &socket = source ? *source : first;
        socket.writeDatagram(live::message("R9ST", {session, elapsed, cap, cap - 100000, 30, sent, sent,
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
    if (!selected(82, 3000, 30)) return 13;
    audioStatus(83, 1000, 600000, 50);
    if (!selected(83, 1000, 30)) return 14;
    receiver.configure(30, 800000);
    for (unsigned i = 0; i < 4; ++i)
        audioStatus(83, 2000 + i * 1000, 800000, 50);
    if (!selected(83, 5000, 30)) return 15;

    // Drop the last fragments of every pair until the ordinary feedback loop
    // requests a lower byte rate. Check the actual configuration on the wire.
    const QByteArray fragment(live::Chunk, '\0');
    auto adaptsToLoss = [&](quint32 id, quint32 startMs, quint32 quality) {
        unsigned seq = 0;
        for (unsigned tick = 0; tick < 24; ++tick) {
            audioStatus(id, startMs + 250 * tick, 800000, 50);
            for (int i = 0; i < 8; ++i) {
                ++seq;
                const auto header = live::message("R10P", {id, seq, seq * 33333, 2000, 1500, 0, 30});
                first.writeDatagram(header + fragment, local, port);
            }
            QTest::qWait(250);
            while (first.hasPendingDatagrams()) {
                const auto cmd = first.receiveDatagram().data();
                if (quality ? !cmd.startsWith("R16C") : (!cmd.startsWith("R6CF") && !cmd.startsWith("R10C")))
                    continue;
                if (live::word(cmd, 0) != id || live::word(cmd, 3) > 600000) continue;
                return live::word(cmd, 2) == 30 && (!quality || (cmd.size() == 28 && live::word(cmd, 5) == quality));
            }
        }
        return false;
    };
    if (!adaptsToLoss(83, 5250, 0)) return 17;
    receiver.configure(20, 800000);
    audioStatus(83, 12000, 600000, 50);
    if (!selected(83, 12000, 20)) return 18;

    // A quality-only change is retried until the current endpoint confirms it.
    receiver.configure(30, 800000, 60);
    auto qualityStatus = [&](QUdpSocket &source, unsigned id, unsigned quality, unsigned elapsed) {
        source.writeDatagram(live::message("R16S", {id, 1, quality, elapsed}), local, port);
    };
    audioStatus(84, 1000, 800000, 50);
    qualityStatus(second, 84, 60, 1000);
    qualityStatus(first, 83, 60, 1000);
    qualityStatus(first, 84, 42, 1000);
    if (!selected(84, 1000, 30) || receiver.stats()["jpeg_quality_supported"].toBool() ||
        receiver.stats()["config_confirmed"].toBool()) return 19;

    auto qualityCommand = [&](unsigned requested, unsigned active, unsigned elapsed) {
        audioStatus(84, elapsed, 800000, 50);
        qualityStatus(first, 84, active, elapsed);
        return QTest::qWaitFor([&] {
            while (first.hasPendingDatagrams()) {
                const auto command = first.receiveDatagram().data();
                if (command.size() == 28 && command.startsWith("R16C") && live::word(command, 0) == 84 &&
                    live::word(command, 2) == 30 && live::word(command, 3) == 800000 &&
                    live::word(command, 4) == 1 && live::word(command, 5) == requested) return true;
            }
            return false;
        }, 1200);
    };
    if (!qualityCommand(60, 0, 2000) || !qualityCommand(60, 0, 3000)) return 20;
    qualityStatus(first, 84, 60, 3500);
    qualityStatus(first, 84, 0, 3200);
    if (!QTest::qWaitFor([&] { return receiver.stats()["jpeg_quality_confirmed"].toBool(); }, 1600)) return 21;
    if (!adaptsToLoss(84, 4000, 60)) return 22;
    receiver.configure(30, 800000, 0);
    if (!qualityCommand(0, 60, 11000)) return 23;

    // Same IP and stream session, but the sender rebound its UDP socket.
    QUdpSocket rebound;
    if (!rebound.bind(local, 0)) return 24;
    QTest::qWait(300);
    audioStatus(84, 12000, 800000, 60, &rebound);
    if (!selected(84, 12000, 30) || receiver.stats()["jpeg_quality_supported"].toBool()) return 25;
    qualityStatus(rebound, 84, 0, 12000);
    if (!QTest::qWaitFor([&] { return receiver.stats()["jpeg_quality_confirmed"].toBool(); }, 1600)) return 26;
    audioStatus(85, 1000, 800000, 50, &rebound);
    return selected(85, 1000, 30) && !receiver.stats()["jpeg_quality_supported"].toBool() &&
           receiver.stats()["config_confirmed"].toBool() ? 0 : 27;
}

static int checkLegacyFps(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QHostAddress local(QHostAddress::LocalHost);
    QUdpSocket handheld;
    if (!handheld.bind(local, 0)) return 1;
    live::Timeline timeline(30);
    live::Pipeline pipeline(timeline, {}, 1, "off", "off");
    live::Receiver receiver(timeline, pipeline, local, 0, 30, 2000000, 0, 80, false);
    if (!QTest::qWaitFor([&] { return receiver.stats()["udp_port"].toInt() > 0; }, 1500)) return 2;
    const auto port = quint16(receiver.stats()["udp_port"].toInt());
    constexpr quint32 legacy = (1u<<15)|(1u<<20)|(1u<<24)|(1u<<30);
    quint32 session = 229, ms = 0, hz = 30, mask = legacy;
    QSet<quint32> commands;
    auto status = [&] {
        ms += 50;
        handheld.writeDatagram(live::message("R9ST", {session,ms,800000,800000,hz,0,0,0,0,0,0,0,
            2,3,100,100,2000000,mask,1,32728498u,2,160,0,0,0,0}), local, port);
        handheld.writeDatagram(live::message("R16S", {session,1,80,ms}), local, port);
    };
    auto read = [&] {
        while (handheld.hasPendingDatagrams()) {
            const auto b = handheld.receiveDatagram().data();
            if ((b.startsWith("R16C") || b.startsWith("R10C") || b.startsWith("R6CF")) &&
                b.size() >= 20 && live::word(b,0) == session)
                commands.insert(live::word(b,2));
        }
    };
    auto configured = [&](int requested, int effective) {
        return QTest::qWaitFor([&] {
            status(); read();
            const auto s = receiver.stats();
            return s["session"].toInt() == int(session) && s["capture_fps"].toInt() == int(hz) &&
                   s["requested_fps"].toInt() == requested && s["effective_fps"].toInt() == effective &&
                   commands.contains(quint32(effective));
        }, 2600);
    };
    auto frame = [&](quint32 seq) {
        const live::Screens imgs{row_fixture::compact(400, int(seq)), row_fixture::compact(320)};
        const auto pixels = live::compose({live::decode(imgs[0],400),live::decode(imgs[1],320)},1);
        const auto both = imgs[0]+imgs[1];
        for (int off=0; off<both.size(); off+=live::Chunk)
            handheld.writeDatagram(live::message("R10P",{session,seq,seq*27000,quint32(imgs[0].size()),
                quint32(imgs[1].size()),quint32(off/live::Chunk),hz})+both.mid(off,live::Chunk),local,port);
        return !pixels.isNull() && QTest::qWaitFor([&] {
            auto shown=timeline.present();
            return shown.source.sequence == seq && shown.image == pixels;
        }, 1200);
    };
    if (!configured(30,30)) return 3;
    for (const auto oldFps : {35u,40u}) {
        // Replay actual v0.1.11 packet shapes while an old stored request is migrated.
        // Both the status and the pixels still carry the original capture cadence.
        hz=oldFps; mask=legacy|3; commands.clear();
        receiver.configure(int(oldFps),2000000,80,false);
        status();
        handheld.writeDatagram(live::message("R20F",{session,1,3}),local,port);
        if (!configured(30,30) || receiver.stats()["fps_mask"].toInt() != int(legacy) ||
            commands.contains(35) || commands.contains(40)) return 4;
        if (!frame(oldFps==35 ? 1 : 2)) return 5;
    }
    receiver.configure(24,2000000,80,false);
    return configured(24,24) && !commands.contains(35) && !commands.contains(40) ? 0 : 6;
}

static int checkQuality70(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QHostAddress local(QHostAddress::LocalHost);
    QUdpSocket handheld, other;
    if (!handheld.bind(local, 0) || !other.bind(local, 0)) return 1;
    live::Timeline timeline(30);
    live::Pipeline pipeline(timeline, {}, 1, "off", "off");
    live::Receiver receiver(timeline, pipeline, local, 0, 30, 2000000, 0, 70, false);
    if (!QTest::qWaitFor([&] { return receiver.stats()["udp_port"].toInt() > 0; }, 1500)) return 2;
    const auto port = quint16(receiver.stats()["udp_port"].toInt());
    quint32 session = 101, ms = 0, activeQ = 60;
    auto status = [&] {
        ms += 100;
        handheld.writeDatagram(live::message("R7ST", {session,ms,800000,800000,24,0,0,0,0,0,0,0,2,3,
                                                        1000,1000,2000000,(1u << 24) | (1u << 30)}), local, port);
        handheld.writeDatagram(live::message("R16S", {session,1,activeQ,ms}), local, port);
    };
    auto capability = [&](QUdpSocket &source, quint32 id, quint32 version, quint32 q) {
        source.writeDatagram(live::message("R16Q", {id,version,q}), local, port);
    };
    bool got60 = false, got70 = false;
    auto observe = [&] {
        status();
        while (handheld.hasPendingDatagrams()) {
            const auto packet = handheld.receiveDatagram().data();
            if (packet.size() != 28 || !packet.startsWith("R16C") || live::word(packet,0) != session) continue;
            if (live::word(packet,2) != 30 || live::word(packet,3) != 2000000) continue;
            got60 |= live::word(packet,5) == 60;
            got70 |= live::word(packet,5) == 70;
        }
        return receiver.stats();
    };
    status();
    capability(other, session, 1, 70);
    capability(handheld, session - 1, 1, 70);
    capability(handheld, session, 2, 70);
    capability(handheld, session, 1, 60);
    if (!QTest::qWaitFor([&] {
            const auto s = observe();
            return got60 && s["requested_jpeg_quality"].toInt() == 70 && s["device_jpeg_quality"].toInt() == 60;
        }, 2500) || got70 || receiver.stats()["jpeg_q70_supported"].toBool() ||
        receiver.stats()["jpeg_quality_confirmed"].toBool()) return 3;
    // A reported Q70 alone doesn't grant the capability, even if it matches our target.
    activeQ = 70;
    if (!QTest::qWaitFor([&] { return observe()["device_jpeg_quality"].toInt() == 70; }, 1800) ||
        got70 || receiver.stats()["jpeg_quality_confirmed"].toBool()) return 4;
    activeQ = 60;
    status();
    capability(handheld, session, 1, 70);
    if (!QTest::qWaitFor([&] { observe(); return got70; }, 1500)) return 5;
    activeQ = 70;
    if (!QTest::qWaitFor([&] { return observe()["jpeg_quality_confirmed"].toBool(); }, 1800)) return 6;
    ++session;
    activeQ = 60;
    got60 = got70 = false;
    capability(handheld, session - 1, 1, 70);
    if (!QTest::qWaitFor([&] { return observe()["session"].toInt() == int(session) && got60; }, 2400) ||
        got70 || receiver.stats()["jpeg_q70_supported"].toBool() || receiver.stats()["jpeg_quality_confirmed"].toBool()) return 7;
    return 0;
}

static int checkSelectiveRepair(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QHostAddress local(QHostAddress::LocalHost);
    QUdpSocket handheld, foreign;
    if (!handheld.bind(local, 0) || !foreign.bind(local, 0)) return 1;
    live::Timeline timeline(100);
    live::Pipeline pipeline(timeline, {}, 1, "off", "off");
    live::Receiver receiver(timeline, pipeline, local, 0, 30, 2000000, 0, 80, false);
    if (!QTest::qWaitFor([&] { return receiver.stats()["udp_port"].toInt() > 0; }, 1500)) return 2;
    const auto port = quint16(receiver.stats()["udp_port"].toInt());
    auto send = [&](const QByteArray &packet) { handheld.writeDatagram(packet, local, port); };
    quint32 session = 41;
    auto status = [&] {
        send(live::message("R7ST", {session,1,2000000,1900000,30,0,0,0,0,0,0,0,2,3,1000,1000,2000000,1u<<30}));
    };
    // A JPEG comment increases the packet count without changing the decoded picture.
    auto padded = [](int width, QColor color, int bytes) {
        auto data = jpeg(width, color);
        const auto length = quint16(bytes + 2);
        QByteArray comment = QByteArray::fromHex("fffe");
        comment.append(char(length >> 8)).append(char(length)).append(QByteArray(bytes,'x'));
        data.insert(2, comment);
        return data;
    };
    const auto bottom = jpeg(320, Qt::blue);
    const auto upper = padded(400, Qt::red, 19*live::Chunk);
    const auto nextUpper = padded(400, Qt::green, 19*live::Chunk);
    auto fragment = [&](quint32 seq, int i, const char *tag) {
        const auto &top = seq == 11 ? nextUpper : upper;
        const auto joined = top + bottom;
        QByteArray payload;
        if (QByteArrayView(tag) == "R10F") {
            payload = QByteArray(live::Chunk,0);
            for (int off = i*live::Chunk; off < joined.size(); off += 4*live::Chunk)
                for (int k = 0; k < live::Chunk && off+k < joined.size(); ++k) payload[k] ^= joined[off+k];
        } else payload = joined.mid(i*live::Chunk,live::Chunk);
        return live::message(tag,{session,seq,seq*33333u,quint32(top.size()),quint32(bottom.size()),quint32(i),30})
               + payload;
    };
    auto frame = [&](quint32 seq, bool loseBurst) {
        const auto &top = seq == 11 ? nextUpper : upper;
        for (int i = 0; i*live::Chunk < top.size()+bottom.size(); ++i)
            if (!loseBurst || i < 3 || i > 11) send(fragment(seq,i,"R10P"));
        if (loseBurst) for (int i = 0; i < 4; ++i) send(fragment(seq,i,"R10F"));
    };
    int correctAcks = 0;
    bool badAck = false;
    auto takeRequest = [&](int timeout) {
        QElapsedTimer timer;
        timer.start();
        do {
            while (handheld.hasPendingDatagrams()) {
                auto datagram = handheld.receiveDatagram();
                auto packet = datagram.data();
                if (packet.startsWith("R19A")) {
                    if (packet == live::message("R19A",{session,1}) && datagram.senderPort() == port) ++correctAcks;
                    else badAck = true;
                }
                if (packet.startsWith("R19N")) return packet;
            }
            handheld.waitForReadyRead(std::max(1,timeout-int(timer.elapsed())));
        } while (timer.elapsed() < timeout);
        return QByteArray{};
    };
    status();
    foreign.writeDatagram(live::message("R19S",{session,1,16}),local,port);
    send(live::message("R19S",{session,2,16}));
    send(live::message("R19S",{session,1,17}));
    frame(9,true);
    if (!takeRequest(90).isEmpty() || correctAcks || badAck || foreign.hasPendingDatagrams()) return 3;
    ++session;
    status();
    send(live::message("R19S",{session-1,1,16}));
    send(live::message("R19S",{session,1,16}));
    send(live::message("R19S",{session,1,16}));
    frame(10,true);
    const auto request = takeRequest(500);
    if (request != live::message("R19N",{session,10,333330,5}) + QByteArray::fromHex("0304050607") ||
        correctAcks != 2 || badAck) return 4;
    // A later frame arrives in full before the requested repairs. The other
    // socket also injects a matching reply, which must not complete our frame.
    frame(11,false);
    for (char i : request.mid(20))
        foreign.writeDatagram(fragment(10,uchar(i),"R19P"),local,port);
    for (char i : request.mid(20)) send(fragment(10,uchar(i),"R19P"));
    QList<quint32> shown;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 750 && shown.size() < 2) {
        const auto displayed = timeline.present();
        if (displayed.source.session == session && !displayed.image.isNull() &&
            (shown.isEmpty() || shown.back() != displayed.source.sequence)) {
            const auto color = displayed.image.pixelColor(50,50);
            if (displayed.synthetic || displayed.quality != "original" ||
                (displayed.source.sequence == 10 ? color.red() : color.green()) < 200) return 5;
            shown.append(displayed.source.sequence);
        }
        QTest::qWait(2);
    }
    if (shown != QList<quint32>{10,11}) { qWarning() << "Presented repair sequences" << shown; return 6; }
    if (!QTest::qWaitFor([&] { return receiver.stats()["repair_completed_pairs"].toInt() == 1; }, 1800)) return 7;
    const auto s = receiver.stats();
    if (!s["selective_repair_supported"].toBool() || s["repair_requests"].toInt() != 1 ||
        s["repair_fragments_requested"].toInt() != 5 || s["repair_fragments_received"].toInt() != 5 ||
        s["repair_completed_tops"].toInt() != 1) return 8;
    return 0;
}

static int checkPresetUpdates(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QHostAddress local(QHostAddress::LocalHost);
    QUdpSocket handheld;
    if (!handheld.bind(local, 0)) return 1;
    live::Timeline timeline(30);
    live::Pipeline pipeline(timeline, {}, 1, "off", "off");
    live::Receiver receiver(timeline, pipeline, local, 0, 30, 2000000, 0, 80, false);
    if (!QTest::qWaitFor([&] { return receiver.stats()["udp_port"].toInt() > 0; }, 1500)) return 2;
    const auto port = quint16(receiver.stats()["udp_port"].toInt());
    constexpr quint32 fpsMask = (1u << 24) | (1u << 30);
    bool hd = false, low = false, mixed = false;
    int commands = 0;
    auto collect = [&](quint32 ms) {
        handheld.writeDatagram(live::message("R7ST", {42,ms,800000,800000,30,0,0,0,0,0,0,0,2,3,
                                                       1000,1000,2000000,fpsMask}), local, port);
        handheld.writeDatagram(live::message("R16S", {42,1,0,ms}), local, port);
        while (handheld.hasPendingDatagrams()) {
            const auto packet = handheld.receiveDatagram().data();
            if (packet.size() != 28 || !packet.startsWith("R16C")) continue;
            ++commands;
            const auto fps = live::word(packet,2), rate = live::word(packet,3), quality = live::word(packet,5);
            const bool a = fps == 30 && rate == 2000000 && quality == 80;
            const bool b = fps == 24 && rate == 1000000 && quality == 40;
            hd |= a;
            low |= b;
            mixed |= !a && !b;
        }
    };
    std::atomic<bool> stop{false};
    std::thread toggler([&] {
        while (!stop.load()) {
            receiver.configure(24,1000000,40,false);
            QThread::msleep(3);
            receiver.configure(30,2000000,80,false);
            QThread::msleep(4);
        }
    });
    QElapsedTimer run;
    run.start();
    while (run.elapsed() < 750) {
        collect(quint32(run.elapsed()+1));
        QTest::qWait(2);
    }
    stop = true;
    toggler.join();
    return commands >= 5 && hd && low && !mixed ? 0 : 3;
}

static int checkReceiverRates(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QHostAddress local(QHostAddress::LocalHost);
    QUdpSocket handheld;
    if (!handheld.bind(local, 0)) return 1;
    live::Timeline timeline(100);
    live::Pipeline pipeline(timeline, {}, 1, "off", "off");
    live::Receiver receiver(timeline, pipeline, local, 0, 30, 2000000, 0, 80, false);
    if (!QTest::qWaitFor([&] { return receiver.stats()["udp_port"].toInt() > 0; }, 1500)) return 2;
    const auto port = quint16(receiver.stats()["udp_port"].toInt());
    const auto top = jpeg(400, Qt::blue), bottom = jpeg(320, Qt::green), pair = top + bottom;
    if (top.isEmpty() || bottom.isEmpty() || pair.size() <= live::Chunk) return 3;
    quint32 seq = 0, cap = 800000, quality = 0, deviceMax = 2000000;
    bool drop = true, legacy = false;
    QElapsedTimer clock;
    clock.start();
    qint64 nextStatus = 0;
    auto pump = [&] {
        const auto now = clock.elapsed();
        const quint32 session = legacy ? 43 : 42;
        auto send = [&](const QByteArray &packet) { handheld.writeDatagram(packet, local, port); };
        // Simulate a Q80 sender producing about ten pairs/s at a configured 30 FPS.
        if (now >= nextStatus) {
            send(live::message("R7ST", {session, quint32(now), cap, cap, 30, seq, seq, seq, seq,
                                        0, 0, 0, 2, 3, 1000, 1000, deviceMax, 1u << 30}));
            if (!legacy) send(live::message("R16S", {session, 1, quality, quint32(now)}));
            nextStatus = now + 900;
        }
        ++seq;
        for (int off = 0; off < pair.size(); off += live::Chunk) {
            auto header = live::message("R10P", {session, seq, quint32(now * 1000), quint32(top.size()),
                                                 quint32(bottom.size()), quint32(off / live::Chunk), 30});
            send(header + pair.mid(off, live::Chunk));
            if (drop) break;
        }
        QTest::qWait(95);
        while (handheld.hasPendingDatagrams()) {
            const auto packet = handheld.receiveDatagram().data();
            if (legacy ? !(packet.startsWith("R10C") && packet.size() == 24)
                       : !(packet.startsWith("R16C") && packet.size() == 28)) continue;
            if (live::word(packet, 0) != session) continue;
            cap = live::word(packet, 3);
            if (!legacy) quality = live::word(packet, 5);
            nextStatus = 0;
        }
    };
    auto until = [&](auto condition, int timeoutMs) {
        const qint64 deadline = clock.elapsed() + timeoutMs;
        while (!condition() && clock.elapsed() < deadline) pump();
        if (condition()) return true;
        qWarning() << "Rate test timeout" << cap << quality << receiver.stats();
        return false;
    };
    if (!until([&] { return cap == 2000000 && quality == 80; }, 2500)) return 4;
    const auto fixedStart = clock.elapsed();
    while (clock.elapsed() < fixedStart + 3300) {
        pump();
        if (cap != 2000000) return 5;
    }
    if (receiver.stats()["adaptive_rate"].toBool(true) || receiver.stats()["incomplete"].toInt() < 5) return 6;
    receiver.configure(30, 2000000, 80, true);
    if (!until([&] { return cap < 2000000; }, 5000) || cap != 1600000) return 7;
    drop = false;
    if (!until([&] { return cap == 2000000; }, 9000)) return 8;
    drop = true;
    if (!until([&] { return cap < 2000000; }, 5000)) return 9;
    const auto backedOff = cap;
    drop = false;
    const auto retryStart = clock.elapsed();
    while (clock.elapsed() < retryStart + 6500) {
        pump();
        if (cap > backedOff) return 10;
    }
    receiver.configure(30, 2000000, 80, false);
    if (!until([&] { return cap == 2000000; }, 2000)) return 11;
    // Fixed mode still respects a previous-generation handheld's supported maximum.
    legacy = true;
    cap = 800000;
    deviceMax = 1000000;
    nextStatus = 0;
    return until([&] { return cap == deviceMax; }, 2500) ? 0 : 12;
}

static int checkPacketTiming(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QHostAddress local(QHostAddress::LocalHost);
    QUdpSocket handheld, other;
    if (!handheld.bind(local, 0) || !other.bind(local, 0)) return 1;
    live::Timeline timeline(100);
    live::Pipeline pipeline(timeline, {}, 1, "off", "off");
    live::Receiver receiver(timeline, pipeline, local, 0, 30, 2000000, 0, 80, false, 0);
    if (!QTest::qWaitFor([&] { return receiver.stats()["udp_port"].toInt() > 0; }, 1000)) return 2;
    const auto port = quint16(receiver.stats()["udp_port"].toInt());
    auto send = [&](const QByteArray &b) { handheld.writeDatagram(b, local, port); };
    auto control = [&] {
        while (handheld.hasPendingDatagrams())
            if (handheld.receiveDatagram().data() == live::message("R21C", {42, 1, 0})) return true;
        return false;
    };
    const auto offer = live::message("R21S", {42, 1, 1});
    send(live::message("R9ST", {42,1000,2000000,1900000,30,30,30,29,29,0,0,0,2,3,500,500,
                                2000000,1u << 30,1,32728498,2,160,200,0,66,1}));
    other.writeDatagram(offer, local, port);
    send(live::message("R21S", {41, 1, 1}));
    send(offer.chopped(1));
    QTest::qWait(30);
    if (control()) return 3;
    send(offer);
    if (!QTest::qWaitFor(control, 750)) return 4;
    send(live::message("R21S", {42,1,0}));
    QByteArray sample = live::message("R21T", {42,1000,0,55,66,77,88,99,100,7,
                                                201,202,203,204,205,301,302,303,401,402,403,404,405});
    for (quint32 i = 24; i < 48; ++i) sample += live::message({}, {i});
    send(sample);
    send(sample.chopped(1));
    if (!QTest::qWaitFor([&] {
        auto stats = receiver.stats();
        auto timing = stats["device_packet_timing"].toObject();
        return stats["device_packet_pacing"].toInt(-1) == 0 && timing["elapsed_ms"].toInt() == 1000 &&
               timing["original"].toObject()["submit_us"].toInt() == 204 &&
               timing["nack"].toObject()["received_us"].toInt() == 303 &&
               timing["repair"].toObject()["return_us"].toInt() == 405 &&
               timing["video"].toObject()["calls"].toInt() == 24 &&
               timing["status"].toObject()["start_after_return_under_150us"].toInt() == 47;
    }, 1600)) return 5;
    return control() ? 6 : 0;
}

static int checkRestartRows(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QHostAddress local(QHostAddress::LocalHost);
    QUdpSocket handheld;
    if (!handheld.bind(local, 0)) return 1;
    live::Timeline timeline(100);
    live::Pipeline pipeline(timeline, {}, 1, "off", "off");
    live::Receiver receiver(timeline, pipeline, local, 0, 30, 800000, 0);
    if (!QTest::qWaitFor([&] { return receiver.stats()["udp_port"].toInt() > 0; }, 1000)) return 2;
    const auto port = quint16(receiver.stats()["udp_port"].toInt());
    auto send = [&](const QByteArray &p) { handheld.writeDatagram(p, local, port); };
    auto ack = [&](unsigned flag, unsigned seq) {
        return QTest::qWaitFor([&] {
            while (handheld.hasPendingDatagrams())
                if (handheld.receiveDatagram().data() == live::message("R17A", {42, flag, seq})) return true;
            return false;
        }, 1200);
    };
    auto pair = [&](unsigned seq, unsigned pts, const live::Screens &images) {
        const QByteArray both = images[0] + images[1];
        for (int off = 0; off < both.size(); off += live::Chunk)
            send(live::message("R10P", {42, seq, pts, quint32(images[0].size()), quint32(images[1].size()),
                                        quint32(off/live::Chunk), 30}) + both.mid(off, live::Chunk));
    };
    const auto status = [&](quint32 id, quint32 elapsed) {
        send(live::message("R9ST", {id, elapsed, 800000, 800000, 30, 0, 0, 0, 0, 0, 0, 0, 2, 3, 500, 500,
                                    2000000, 1u << 30, 1, 32728498, 2, 160, 2050, 13, 679, 2}));
    };
    const auto counters = [&](quint32 id, quint32 elapsed, quint32 replaced) {
        return live::message("R19T", {id, elapsed, replaced, 1, 23, 98, 100050, 5, 8, UINT32_MAX});
    };
    constexpr quint32 start = UINT32_MAX - 499;
    status(42, start);
    send(counters(42, start, 0));
    send(live::message("R17T", {42, start, UINT32_MAX - 19, UINT32_MAX - 99}));
    const auto offer = live::message("R17S", {42, 1});
    send(offer);
    if (!ack(0, 0)) return 3;
    const live::Screens original{row_fixture::compact(400), row_fixture::compact(320)};
    const live::Screens edited{row_fixture::compact(400, 0), row_fixture::compact(320, 14)};
    pair(0, 1000, {row_fixture::key(original[0]), row_fixture::key(original[1])});
    if (!ack(1, 0)) return 4;
    const auto repeat = live::message("R10P", {42, 0, 1000, quint32(original[0].size()),
                                               quint32(original[1].size()), 0, 30}) +
                        row_fixture::key(original[0]).left(live::Chunk);
    QTest::qWait(30);
    QUdpSocket other;
    if (!other.bind(local, 0)) return 10;
    other.writeDatagram(repeat, local, port);
    QTest::qWait(15);
    while (handheld.hasPendingDatagrams())
        if (handheld.receiveDatagram().data() == live::message("R17A", {42,1,0})) return 11;
    send(repeat);
    if (!ack(1, 0)) return 12;
    send(offer); // The key ACK can be lost.
    if (!ack(1, 0)) return 5;
    const live::Screens delta{row_fixture::delta(original[0], edited[0], 0),
                              row_fixture::delta(original[1], edited[1], 0)};
    auto dictAck = [&](QUdpSocket &socket, unsigned id) {
        while (socket.hasPendingDatagrams())
            if (socket.receiveDatagram().data() == live::message("R20A", {id,1})) return true;
        return false;
    };
    const auto dictOffer = live::message("R20S", {42,1});
    other.writeDatagram(dictOffer, local, port);
    send(live::message("R20S", {43,1}));
    send(live::message("R20S", {42,2}));
    send(dictOffer.chopped(1));
    QTest::qWait(25);
    if (dictAck(handheld, 42) || dictAck(other, 42)) return 13;
    for (int i = 0; i < 2; ++i) {
        send(dictOffer);
        if (!QTest::qWaitFor([&] { return dictAck(handheld, 42); }, 500)) return 14;
    }
    const auto wrappedBottom = row_fixture::dictionary(original[1], delta[1], 0);
    pair(2, 70000, {delta[0], wrappedBottom});
    const auto expected = live::compose({live::decode(edited[0], 400), live::decode(edited[1], 320)}, 1);
    if (expected.isNull() || !QTest::qWaitFor([&] {
        const auto frame = timeline.present();
        return frame.source.sequence == 2 && frame.image == expected;
    }, 1500)) return 6;
    status(42, 500);
    send(counters(42, 500, 7));
    send(counters(42, 300, 77));       // Reordered.
    send(counters(42, 501, 77).chopped(4));
    send(counters(43, 501, 77));       // Different session.
    other.writeDatagram(counters(42, 501, 77), local, port);
    send(live::message("R17T", {42, 500, 799980, 699900}));
    send(live::message("R17T", {42, 300, 17, 12})); // A reordered sample cannot replace the valid rates.
    if (!QTest::qWaitFor([&] {
        const auto stats = receiver.stats();
        const qint64 wire = original[0].size() + original[1].size() + delta[0].size() + wrappedBottom.size();
        const qint64 savings = edited[0].size() + edited[1].size() - delta[0].size() - delta[1].size();
        const qint64 dictSavings = delta[1].size() - wrappedBottom.size();
        return stats["jpeg_row_key_pairs"].toInt() == 1 && stats["jpeg_row_repeat_acks"].toInt() == 1 &&
               stats["jpeg_row_delta_screens"].toInt() == 2 &&
               stats["jpeg_rows_reused"].toInt() == 28 && stats["jpeg_wire_bytes"].toInteger() == wire &&
               stats["jpeg_row_saved_bytes"].toInteger() == savings &&
               stats["jpeg_dictionary_supported"].toBool() && stats["jpeg_dictionary_screens"].toInt() == 1 &&
               stats["jpeg_dictionary_saved_bytes"].toInteger() == dictSavings && stats.contains("jpeg_dictionary_decode_mean_us") &&
               stats["jpeg_reconstructed_bytes"].toInteger() == wire + savings + dictSavings + 4 * LIVE_JPEG_WIRE_SAVED_BYTES &&
               stats["device_media_bytes_per_s"].toInt() == 800000 && stats["device_video_bytes_per_s"].toInt() == 700000 &&
               stats["device_audio_blocks_queued"].toInt() == 2050 && stats["device_audio_blocks_dropped"].toInt() == 13 &&
               stats["device_audio_packets_sent"].toInt() == 679 && stats["device_audio_queue_depth"].toInt() == 2 &&
               stats["device_counters_elapsed_ms"].toInt() == 500 && stats["device_replaced"].toInt() == 7 &&
               stats["device_row_repeats"].toInt() == 1 && stats["device_repair_requests"].toInt() == 23 &&
               stats["device_repair_packets"].toInt() == 98 && stats["device_repair_ipv4_bytes"].toInt() == 100050 &&
               stats["device_audio_queue_full"].toInt() == 5 && stats["device_audio_expired"].toInt() == 8 &&
               stats["device_send_active_us"].toInteger() == UINT32_MAX;
    }, 1500)) return 7;
    if (!QTest::qWaitFor([&] {
        const auto stats = receiver.stats();
        return !stats.contains("device_media_bytes_per_s") && !stats.contains("device_replaced") &&
               !stats.contains("device_audio_packets_sent");
    }, 4500)) return 8;
    // A periodic reply from the earlier live interval can still be queued
    // at this test's socket. Count only replies to the stale offer below.
    while (handheld.hasPendingDatagrams()) handheld.receiveDatagram();
    send(dictOffer); // Existing endpoint, expired video/status lease.
    QTest::qWait(35);
    if (dictAck(handheld, 42)) return 15;
    status(43, 200);
    send(live::message("R17T", {43, 200, 5000, 4500})); // Only one sample in the new session.
    if (!QTest::qWaitFor([&] {
        auto stats = receiver.stats();
        return stats["session"].toInt() == 43 && !stats["jpeg_dictionary_supported"].toBool() &&
               stats["jpeg_dictionary_screens"].toInt() == 0 && !stats.contains("device_media_bytes_per_s") &&
               !stats.contains("device_replaced") && stats["device_audio_packets_sent"].toInt() == 679;
    }, 1600)) return 9;
    return 0;
}

static int checkCapabilityRenewal(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QHostAddress local(QHostAddress::LocalHost);
    QUdpSocket handheld, other;
    if (!handheld.bind(local,0) || !other.bind(local,0)) return 1;
    live::Timeline timeline(100);
    live::Pipeline pipeline(timeline, {}, 1, "off", "off");
    live::Receiver receiver(timeline,pipeline,local,0,30,800000,0);
    if (!QTest::qWaitFor([&]{ return receiver.stats()["udp_port"].toInt() > 0; },1000)) return 2;
    const auto port = quint16(receiver.stats()["udp_port"].toInt());
    auto send = [&](QUdpSocket &s,const QByteArray &b){ s.writeDatagram(b,local,port); };
    auto status = [](quint32 sid,quint32 elapsed) {
        return live::message("R9ST",{sid,elapsed,800000,700000,30,3,3,3,3,0,0,0,2,3,500,500,
                                    2000000,1u<<30,1,32728498,2,160,0,0,0,0});
    };
    auto sendPair = [&](quint32 seq,const live::Screens &panels) {
        const QByteArray both=panels[0]+panels[1];
        for (int off=0;off<both.size();off+=live::Chunk)
            send(handheld,live::message("R10P",{42,seq,seq*33333u,quint32(panels[0].size()),
                                                quint32(panels[1].size()),quint32(off/live::Chunk),30})
                          +both.mid(off,live::Chunk));
    };
    auto drain = [&](QUdpSocket &s) {
        QList<QByteArray> result;
        while (s.hasPendingDatagrams()) {
            const auto datagram=s.receiveDatagram();
            if (datagram.senderPort()==port) result.push_back(datagram.data());
        }
        return result;
    };
    const auto rows=live::message("R17S",{42,1});
    const auto rowAck=live::message("R17A",{42,1,0});
    const auto noKey=live::message("R17A",{42,0,0});
    const auto repairAck=live::message("R19A",{42,1});
    const auto dictionaryAck=live::message("R20A",{42,1});
    send(handheld,status(42,1));send(handheld,rows);
    send(handheld,live::message("R19S",{42,1,16}));send(handheld,live::message("R20S",{42,1}));
    const live::Screens base{row_fixture::compact(400),row_fixture::compact(320)};
    sendPair(0,{row_fixture::key(base[0]),row_fixture::key(base[1])});
    if (!QTest::qWaitFor([&]{return drain(handheld).contains(rowAck);},1200)) return 3;
    // The firmware stops using a feature after four seconds. Supply only
    // actual delta frames through a longer gap in all three capability offers.
    const live::Screens deltas{row_fixture::delta(base[0],base[0],0),row_fixture::delta(base[1],base[1],0)};
    const QList<QByteArray> replies{rowAck,repairAck,dictionaryAck};
    QElapsedTimer watch;
    watch.start();
    std::array<int,3> counts{};
    std::array<qint64,3> last{};
    for (quint32 seq=1;watch.elapsed()<5150;++seq) {
        sendPair(seq,deltas);
        QTest::qWait(80);
        for (const auto &b:drain(handheld)) {
            if (b==noKey) return 4;
            const auto i=replies.indexOf(b);
            if (i<0) continue;
            if (watch.elapsed()-last[i]>1850) return 5;
            ++counts[i];last[i]=watch.elapsed();
        }
    }
    for (int n:counts) if(n<4 || n>6) return 6;
    // After an outage, the same endpoint/session must renegotiate the
    // reference. An endpoint move also must not inherit any old replies.
    QTest::qWait(2300);drain(handheld);
    QTest::qWait(2100);
    if (!drain(handheld).isEmpty()) return 7;
    send(handheld,status(42,6000));send(handheld,rows);
    if (!QTest::qWaitFor([&]{return drain(handheld).contains(noKey);},1000)) return 8;
    QTest::qWait(300);
    send(other,status(42,6400));
    for (unsigned i=0;i<13;++i) {send(other,status(42,6500+100*i));QTest::qWait(100);}
    for (const auto &b:drain(other))
        if (b.startsWith("R17A") || b.startsWith("R19A") || b.startsWith("R20A")) return 9;
    return 0;
}

static int checkDeliveryWindow(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QHostAddress local(QHostAddress::LocalHost);
    QUdpSocket handheld, unrelated;
    if (!handheld.bind(local, 0) || !unrelated.bind(local, 0)) return 1;
    live::Timeline timeline(30);
    live::Pipeline pipeline(timeline, {}, 1, "off", "off");
    live::Receiver receiver(timeline, pipeline, local, 0, 30, 2000000, 0, 80, false);
    if (!QTest::qWaitFor([&] { return receiver.stats()["udp_port"].toInt() > 0; }, 1500)) return 2;
    const auto port = quint16(receiver.stats()["udp_port"].toInt());
    auto send = [&](const QByteArray &packet) { handheld.writeDatagram(packet, local, port); };
    auto status = [&](quint32 ms, quint32 frames) {
        send(live::message("R9ST", {42, ms, 2000000, 1900000, 30, frames,frames,frames,frames, 0,0,0,2,3,
                                    1000,1000,2000000,1u<<30,1,32728,2,160,0,0,0,0}));
    };
    status(1, 0); // establish the UDP endpoint before a wire report arrives out of order
    send(live::message("R16S", {42,1,80,1}));
    send(live::message("R17T", {42,100,500000,400000}));
    status(100,100);
    const auto top = row_fixture::compact(400), bottom = row_fixture::compact(320), joined = top + bottom;
    qint64 media = 0;
    auto sendMedia = [&](const QByteArray &packet) { send(packet); media += packet.size() + 28; };
    auto fragment = [&](quint32 seq, int off) {
        return live::message("R10P", {42,seq,seq*33333u,quint32(top.size()),quint32(bottom.size()),
                                      quint32(off/live::Chunk),30}) + joined.mid(off, live::Chunk);
    };
    for (int off = 0; off < joined.size(); off += live::Chunk) sendMedia(fragment(1, off));
    sendMedia(fragment(1, 0)); // a duplicate consumes bandwidth, but it isn't another pair
    for (int off = 0; off < top.size(); off += live::Chunk) sendMedia(fragment(2, off));
    sendMedia(live::message("R10F",{42,1,33333,quint32(top.size()),quint32(bottom.size()),0,30}) +
              QByteArray(live::Chunk, 'p'));
    auto audio = live::message("R9A1", {42,0,1,32728498u,(2u<<16)|160u}) + QByteArray(175, 0);
    sendMedia(audio);
    send(live::message("R17S",{42,1})); // all control/status bytes must be excluded
    unrelated.writeDatagram(fragment(3, 0), local, port);
    QTest::qWait(2110);
    status(2210, 160);
    if (!receiver.stats().value("delivery_window").isUndefined()) return 3;
    unrelated.writeDatagram(live::message("R17T", {42,2210,9000000,0}), local, port);
    send(live::message("R17T", {42,2210,2250000,2100000}));
    if (!QTest::qWaitFor([&] { return receiver.stats().contains("delivery_window"); }, 500)) return 4;
    auto window = receiver.stats()["delivery_window"].toObject();
    if (window["device_media_bytes"].toInteger() != 1750000 || window["received_media_bytes"].toInteger() != media ||
        window["device_sent_pairs"].toInt() != 60 || window["received_pairs"].toInt() != 1 ||
        window["received_tops"].toInt() != 2 || window["device_jpeg_quality"].toInt() != 80) return 5;
    send(live::message("R16S", {42,1,60,2210}));
    if (!QTest::qWaitFor([&] { return !receiver.stats().contains("delivery_window"); }, 500)) return 6;
    return 0;
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
    if (testArgc == 3 && QByteArray(testArgv[1]) == "--reject-fps") {
        char fpsArg[] = "--fps", secondsArg[] = "--seconds", duration[] = "1";
        char *args[]{testArgv[0], fpsArg, testArgv[2], secondsArg, duration, nullptr};
        int argc = 5;
        QApplication app(argc, args);
        return runClient(app, 0) == 2 ? 0 : 1;
    }
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
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--receiver-rates")
        return checkReceiverRates(testArgc, testArgv);
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--receiver-presets")
        return checkPresetUpdates(testArgc, testArgv);
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--receiver-legacy-fps")
        return checkLegacyFps(testArgc, testArgv);
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--receiver-quality70")
        return checkQuality70(testArgc, testArgv);
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--receiver-repair")
        return checkSelectiveRepair(testArgc, testArgv);
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--packet-timing")
        return checkPacketTiming(testArgc, testArgv);
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--receiver-rows")
        return checkRestartRows(testArgc, testArgv);
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--capability-renewal")
        return checkCapabilityRenewal(testArgc, testArgv);
    if (testArgc == 2 && QByteArray(testArgv[1]) == "--delivery-window")
        return checkDeliveryWindow(testArgc, testArgv);
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
        trace.toLocal8Bit(),      "--record",  mp4.toLocal8Bit(), "--seconds",         "15",
        "--stream-preset",        "hd30", "--jpeg-quality", "70", "--fixed-rate"};
    QVector<char *> argv;
    for (auto &arg : args)
        argv.append(arg.data());
    int argc = int(argv.size());
    argv.append(nullptr);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv.data());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, work.path());
    QSettings storedQuality(QSettings::IniFormat, QSettings::UserScope, "3ds-live", "3DS Live");
    storedQuality.setValue("jpegQuality", 40);
    storedQuality.setValue("adaptiveRate", true);
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
        settings = trigger("连接与输出…");
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
        auto table = debug->findChild<QTableView *>();
        marked = freeze && freeze->isChecked() && table && table->model()->rowCount() > 4;
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
    if (storedQuality.value("jpegQuality").toInt() != 40 || !storedQuality.value("adaptiveRate").toBool()) {
        qCritical() << "Command-line run overwrote saved JPEG target" << storedQuality.value("jpegQuality");
        return 11;
    }
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
        if (sample.contains("rx_sample_us") &&
            (sample["requested_jpeg_quality"].toInt() != 70 || sample["adaptive_rate"].toBool(true) ||
             sample["requested_fps"].toInt() != 30 || sample["requested_rate"].toInt() != 2000000)) {
            qCritical() << "Unexpected JPEG target in receiver sample" << sample["connection"]
                        << sample["requested_jpeg_quality"];
            return 11;
        }
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
