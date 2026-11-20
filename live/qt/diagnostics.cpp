#include "diagnostics.h"
#include <chrono>
#include <future>
namespace live {
qint64 clockUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

QJsonObject diagnoseFrameDrops(const QJsonObject &current, const QJsonArray &history) {
    const auto now = current.value("wall_us").toInteger();
    const auto state = current.value("connection").toString();
    QJsonArray stages, evidence;
    QString label;
    auto note = [&](QString stage, QString shortLabel, QString detail) {
        stages.append(stage);
        evidence.append(detail);
        label = stages.size() == 1 ? shortLabel : QStringLiteral("多环节");
    };
    auto result = [&]() -> QJsonObject {
        if (stages.isEmpty()) return {};
        return {{"label", label}, {"stages", stages}, {"evidence", evidence}};
    };
    if (state == "interrupted") {
        const auto packet = current.value("last_packet_us").toInteger();
        const auto pair = current.value("last_top_us").toInteger(current.value("last_pair_us").toInteger());
        if (packet > pair && pair > 0 && now - packet < 1200000 && now > pair)
            note("input", "收帧", QString("仍有数据到达，%1 秒未收齐上屏。").arg((now - pair) / 1e6, 0, 'f', 1));
        else if (pair > 0 && packet > 0 && now - packet >= 1200000)
            note("silence", "断流", QString("已 %1 秒未收到掌机数据。掌机停流或链路中断，等待新包。")
                 .arg((now - packet) / 1e6, 0, 'f', 1));
        return result();
    }
    if (state != "live" || current.value("model_preparing").toBool() ||
        !current.value("model_error").toString().isEmpty() || !current.value("session").toInteger())
        return {};

    // Average several complete UI and receiver ticks. Their clocks have different
    // sampling boundaries; subtract totals on each clock instead of comparing two 1 s readings.
    auto first = current;
    double sourceGap = 0, inputGap = 0;
    bool sameGear = true, visible = true;
    for (auto i = history.size(); i > 0 && now - first.value("wall_us").toInteger() < 2500000; --i) {
        const auto previous = history.at(i - 1).toObject();
        const auto step = first.value("wall_us").toInteger() - previous.value("wall_us").toInteger();
        if (step <= 0 || step > 2000000 || previous.value("connection") != "live" ||
            previous.value("session") != current.value("session") || previous.value("mode") != current.value("mode") ||
            previous.value("motion") != current.value("motion") || previous.value("requested_fps") != current.value("requested_fps") ||
            previous.value("model_preparing").toBool()) return {};
        sourceGap = std::max(sourceGap, first.value("source_interval_max_gap_ms").toDouble());
        inputGap = std::max(inputGap, first.value("interval_top_gap_ms").toDouble(first.value("interval_pair_gap_ms").toDouble()));
        visible &= first.value("canvas_visible").toBool() && previous.value("canvas_visible").toBool();
        sameGear &= previous.value("capture_fps") == current.value("capture_fps");
        first = previous;
    }
    const double seconds = (now - first.value("wall_us").toInteger()) / 1e6;
    auto delta = [&](const char *key) -> qint64 {
        const auto a = first.value(key), b = current.value(key);
        return a.isDouble() && b.isDouble() ? b.toInteger() - a.toInteger() : -1;
    };
    const double rxSeconds = delta("rx_sample_us") / 1e6;
    const auto pairs = delta(current.contains("top_received") ? "top_received" : "pairs");
    const auto real = delta("presented_frames"), generated = delta("synthetic_at_presentation");
    if (seconds < 2.5 || rxSeconds < 2 || rxSeconds > 5 || pairs < 0 || generated < 0 || real < generated)
        return {};
    const double received = pairs / rxSeconds, shown = (real - generated) / seconds;
    const int requested = current.value("requested_fps").toInt();
    auto freshStatus = [](const QJsonObject &row) {
        const auto stamp = row.value("device_status_us").toInteger();
        const auto age = row.value("wall_us").toInteger() - stamp;
        return stamp > 0 && age >= 0 && age < 2500000;
    };
    const int gear = freshStatus(current) ? current.value("capture_fps").toInt() : 0;
    const auto elapsed = delta("device_elapsed_ms"), sentCount = delta("device_sent_top");
    const double sent = freshStatus(first) && gear > 0 && elapsed >= 1500 && elapsed <= 5000 && sentCount >= 0
        ? sentCount * 1000. / elapsed : -1;
    auto slower = [](double expected, double actual) { return expected - actual > std::max(2., expected * .10); };
    if (gear > 0 && gear < requested)
        note("rate", "帧率档位", QString("掌机报告 %1 FPS，所选目标为 %2 FPS。").arg(gear).arg(requested));
    const bool senderSlow = sameGear && sent >= 0 && slower(gear, sent);
    if (senderSlow)
        note("sender", "发送", QString("掌机报告发送 %1 FPS，当前档位 %2 FPS。").arg(sent, 0, 'f', 1).arg(gear));
    const bool inputSlow = slower(sent >= 0 ? sent : (gear > 0 ? gear : requested), received);
    const auto incomplete = delta("incomplete");
    if (incomplete > 0 && (inputSlow || (sourceGap > 100 && !current.contains("top_received"))))
        note("input", "缺片", QString("PC 有 %1 对双屏缺片未收齐。需对照掌机和路由器记录。").arg(incomplete));
    else if (inputSlow || (inputGap > 100 && sourceGap > 100 && !senderSlow))
        note("input", "收帧", inputSlow && sent >= 0 ?
             QString("掌机报告发送 %1、PC 收齐 %2 FPS。传输中的具体位置待查。").arg(sent, 0, 'f', 1).arg(received, 0, 'f', 1) :
             inputSlow ? QString("PC 收齐 %1 FPS，输入不足，具体原因待查。").arg(received, 0, 'f', 1) :
                         QString("视频到达间隔最长 %1 ms，具体原因待查。").arg(qRound(inputGap)));
    const auto decode = delta("decode_overwrites"), enhance = delta("enhancement_overwrites");
    if (slower(received, shown)) {
        if (decode > 0 || enhance > 0)
            note("processing", decode > 0 ? "PC处理" : "PC增强",
                 QString("PC 收齐 %1、显示 %2 个源帧/秒，%3队列跳过 %4 帧。")
                     .arg(received, 0, 'f', 1).arg(shown, 0, 'f', 1)
                     .arg(decode > 0 ? QStringLiteral("解码") : QStringLiteral("增强")).arg(decode > 0 ? decode : enhance));
        else
            note("playout", "呈现", QString("PC 收齐 %1、显示 %2 个源帧/秒。调度原因待查。")
                 .arg(received, 0, 'f', 1).arg(shown, 0, 'f', 1));
    }
    const auto coalesced = delta("canvas_coalesced_updates");
    if (visible && coalesced >= 2)
        note("paint", "PC绘制", QString("PC 有 %1 次画面更新在绘制前被替换。").arg(coalesced));
    if (stages.isEmpty() && sourceGap > 100)
        note("unknown", "卡顿", QString("源帧显示间隔最长 %1 ms，现有数据不足以定位。").arg(qRound(sourceGap)));
    return result();
}

QJsonObject TraceEvent::json() const {
    QJsonObject fields{{"event", kind},
            {"wall_us", wall},
            {"target_us", target},
            {"session", qint64(source.session)},
            {"sequence", qint64(source.sequence)},
            {"pts_us", source.pts},
            {"arrival_us", source.arrival},
            {"missing_fragments", missingFragments},
            {"choice", choice},
            {"image_key", QString::number(imageKey)},
            {"quality", quality}};
    for (int i = 0; i < 2; ++i) {
        if (const auto &p = source.panels[i]) {
            fields[i ? "bottom_sequence" : "top_sequence"] = qint64(p->sequence);
            fields[i ? "bottom_pts_us" : "top_pts_us"] = p->pts;
        }
    }
    if (updatedPanels || generatedPanels) {
        fields["updated_panels"] = updatedPanels;
        fields["generated_panels"] = generatedPanels;
    }
    if (generatedBottom) {
        fields["bottom_generated_pts_us"] = generatedBottom->pts;
        fields["bottom_reference_sequence"] = qint64(generatedBottom->sequence);
    }
    return fields;
}
void Diagnostics::add(TraceEvent event) {
    if (!event.wall)
        event.wall = clockUs();
    std::lock_guard lock(mutex);
    event.number = ++next;
    events.push_back(std::move(event));
    while (!events.empty() && (events.front().wall < events.back().wall - 60000000 || events.size() > 65536))
        events.pop_front();
}
QVector<TraceEvent> Diagnostics::since(quint64 &cursor, qint64 now) const {
    std::lock_guard lock(mutex);
    QVector<TraceEvent> out;
    if (!events.empty() && cursor < next) {
        auto first = std::max(cursor + 1, events.front().number) - events.front().number;
        for (size_t i = first; i < events.size(); ++i)
            if (events[i].wall >= now - 60000000)
                out.append(events[i]);
        if (!out.isEmpty())
            cursor = out.back().number;
    }
    return out;
}
QJsonArray Diagnostics::window(qint64 now) const {
    quint64 cursor = 0;
    QJsonArray result;
    for (const auto &event : since(cursor, now))
        result.append(event.json());
    return result;
}
bool TraceWriter::start(const QString &path, const Diagnostics &events) {
    std::promise<bool> opened;
    auto ready = opened.get_future();
    writer = std::thread([this, path, &events, opened = std::move(opened)]() mutable {
        QFile file(path);
        const bool ok = file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
        opened.set_value(ok);
        if (!ok) return;
        quint64 cursor = 0;
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait_for(lock, std::chrono::milliseconds(250), [this] { return stop; });
            const bool final = stop;
            lock.unlock();
            QByteArray batch;
            for (const auto &event : events.since(cursor, clockUs()))
                batch += QJsonDocument(event.json()).toJson(QJsonDocument::Compact) + '\n';
            if (!batch.isEmpty()) {
                file.write(batch);
                file.flush();
            }
            if (final) break;
            lock.lock();
        }
    });
    return ready.get();
}
TraceWriter::~TraceWriter() {
    if (!writer.joinable()) return;
    {
        std::lock_guard lock(mutex);
        stop = true;
    }
    wake.notify_one();
    writer.join();
}
} // namespace live
