#pragma once
#include <QtCore>
#include <array>
#include <optional>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace live {
qint64 clockUs();
QJsonObject diagnoseFrameDrops(const QJsonObject &current, const QJsonArray &history);
struct PanelStamp {
    quint32 sequence = 0;
    qint64 pts = 0;
};
struct SourceFrame {
    quint32 session = 0, sequence = 0;
    qint64 pts = 0, arrival = 0;
    std::array<std::optional<PanelStamp>, 2> panels{};
};
struct TraceEvent {
    const char *kind;
    SourceFrame source{};
    qint64 wall = 0, target = 0, choice = 0, imageKey = 0;
    QString quality;
    quint64 number = 0;
    int missingFragments = 0;
    int updatedPanels = 0, generatedPanels = 0;
    std::optional<PanelStamp> generatedBottom;
    QJsonObject json() const;
};
class Diagnostics {
    mutable std::mutex mutex;
    std::deque<TraceEvent> events;
    quint64 next = 0;

  public:
    void add(TraceEvent event);
    QVector<TraceEvent> since(quint64 &cursor, qint64 now) const;
    QJsonArray window(qint64 now) const;
};
class TraceWriter {
    std::thread writer;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
  public:
    bool start(const QString &path, const Diagnostics &events);
    ~TraceWriter();
};
} // namespace live
