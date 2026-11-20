#pragma once
#include <QtCore>
#include <array>
#include <deque>
#include <optional>
#include <functional>

namespace live {
constexpr int Chunk = 1168;
using Screens = std::array<QByteArray, 2>;
struct Pair {
    quint32 session = 0, seq = 0, pts = 0;
    Screens screens;
    int fps = 0;
    std::array<int, 2> jpegQuality{}; // Per-panel R8J1 encoder Q; zero means unknown.
    std::optional<quint32> bottomPts; // R15 may carry a previously captured lower screen.
};
struct Counts {
    qint64 packets = 0, complete = 0, fullPairs = 0, incomplete = 0, missing = 0, repaired = 0, late = 0, invalid = 0;
};
quint32 word(QByteArrayView bytes, int index);
QByteArray message(QByteArray magic, std::initializer_list<quint32> words);

// Three receiver ticks at one confirmed sender setting. This window is used
// only for upward probing; loss and congestion decisions use the latest tick.
class RecoveryCadence {
    int confirmedFps = 0, confirmedRate = 0;
    qint64 since = 0, lastStatus = 0;
    qint64 lastSampleEnd = 0;
    std::deque<std::pair<int, qint64>> samples;
  public:
    void confirm(int fps, int rate, qint64 now);
    bool healthy(int fps, int rate, qint64 begin, qint64 end, int complete);
};

class Assembler {
    struct Fragments {
        int total = 0, count = 0;
        QVector<QByteArray> data;
        std::array<QByteArray, 4> parity;
        void add(int size, int index, int stripes, bool fec, QByteArrayView payload, Counts &counts);
        QByteArray join() const;
        QByteArray range(int begin, int size) const;
    };
    struct Partial {
        quint32 seq, pts;
        int fps, top, bottom;
        bool combined;
        qint64 born;
        std::array<Fragments, 2> screens;
        Screens ready{};
        std::optional<quint32> bottomPts;
        bool accounted = false;
    };
    std::deque<Partial> pending;
    std::optional<quint32> completed, newest;
    std::array<Fragments, 2> legacy;
    std::array<std::optional<quint32>, 2> legacySeq;
    Screens previous;
    std::array<std::optional<quint32>, 2> published;
    void publish(Pair update);
    void discard();

  public:
    std::function<void(quint32, quint32, qint64, int)> onIncomplete;
    // A nonempty screen is an independently complete JPEG. Both may become ready on the same packet.
    std::function<void(Pair)> onPanels;
    Counts counts;
    bool combined = false;
    void reset();
    void expire(qint64 now);
    std::optional<Pair> feed(QByteArrayView packet, qint64 now);
};
} // namespace live
