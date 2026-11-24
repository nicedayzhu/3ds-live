#pragma once
#include <QtCore>
#include <array>
#include <deque>
#include <optional>
#include <functional>
#include "live_jpeg_rows.h"

namespace live {
constexpr int Chunk = 1168;
constexpr int MaxRepairFragments = 16;
using Screens = std::array<QByteArray, 2>;
struct Pair {
    quint32 session = 0, seq = 0, pts = 0;
    Screens screens;
    int fps = 0;
    std::array<int, 2> jpegQuality{}; // Per-panel R8J1 encoder Q; zero means unknown.
    std::optional<quint32> bottomPts; // R15 may carry a previously captured lower screen.
    std::array<int, 2> deltaBytes{}; // R17D payload length before restoring this screen.
    std::array<int, 2> reusedRows{};
    int dictionaryWireBytes = 0; // R20D replaces a lower R17D on the wire.
};
struct Counts {
    qint64 packets = 0, complete = 0, fullPairs = 0, incomplete = 0, missing = 0, repaired = 0, late = 0, invalid = 0;
};
quint32 word(QByteArrayView bytes, int index);
QByteArray message(QByteArray magic, std::initializer_list<quint32> words);
int supportedCaptureFps(int requested, int supported, int current);

class DeliveryWindow {
  public:
    struct Status {
        quint32 elapsed, sent;
        int fps, cap;
        qint64 wall, mediaBytes, pairs, tops;
    };
    static constexpr qint64 freshUs = 1500000;
    void status(Status next);
    void wire(quint32 elapsed, quint32 mediaBytes, qint64 now);
    QJsonObject report(qint64 now) const;
  private:
    struct Wire { quint32 elapsed, bytes; qint64 wall; };
    std::deque<Status> statuses;
    std::deque<Wire> wires;
    std::optional<quint32> bytesAt(const Status &status) const;
};

// Upward probes use receiver cadence and paired delivery between successive sender
// status reports. Loss and congestion decisions still use the latest receiver tick.
class RecoveryCadence {
  public:
    struct Delivery {
        quint32 elapsedMs, sentPairs, sendFailures;
        qint64 receivedPairs;
    };
  private:
    int confirmedFps = 0, confirmedRate = 0;
    qint64 since = 0, lastStatus = 0;
    qint64 lastSampleEnd = 0;
    std::deque<std::pair<int, qint64>> samples;
    std::deque<Delivery> reports;
  public:
    void confirm(int fps, int rate, qint64 now, std::optional<Delivery> delivery = {});
    bool healthy(int fps, int rate, qint64 begin, qint64 end, int complete);
    bool delivered(int fps, int rate, qint64 now, bool qualityLimited = false) const;
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
        std::array<bool, 2> unusable{};
        std::array<std::optional<LiveJpegRows>, 2> keyRows{};
        quint32 session = 0;
        qint64 lastProgress = 0, repairUntil = 0, holdUntil = 0;
        QByteArray requestedFragments;
        bool deferred = false, usedRepair = false, repairedTopData = false;
    };
    struct Reference {
        quint32 session, seq, pts;
        int fps;
        Screens screens;
        std::array<LiveJpegRows, 2> rows;
    };
    std::deque<Reference> references;
    std::optional<quint32> activeReference;
    qint64 lastKeyAckUs = 0;
    bool awaitingKey = true;
    std::deque<Partial> pending;
    std::optional<quint32> completed, newest;
    std::array<Fragments, 2> legacy;
    std::array<std::optional<quint32>, 2> legacySeq;
    Screens previous;
    std::array<std::optional<quint32>, 2> published;
    const int playoutDelayUs;
    void publish(Pair update);
    void discard();
    bool restorePanel(Partial &partial, int screen, QByteArray &panel, Pair &update);
    void requestRepair(Partial &partial, qint64 now);
    bool heldBefore(quint32 seq, qint64 now) const;
    void releaseDeferred(qint64 now);
    std::optional<Pair> finish(quint32 seq, qint64 now);
    Pair complete(Pair pair);

  public:
    explicit Assembler(int delayUs = 100000) : playoutDelayUs(delayUs) {}
    std::function<void(quint32, quint32, qint64, int)> onIncomplete;
    // A nonempty screen is an independently complete JPEG. Both may become ready on the same packet.
    std::function<void(Pair)> onPanels;
    std::function<void(const Pair &)> onComplete;
    std::function<bool(quint32, quint32, const QByteArray &)> onRepair;
    int repairLimit = 0;
    qint64 repairRequests = 0, requestedFragments = 0, repairFragments = 0, repairedPairs = 0, repairedTops = 0;
    qint64 repairWireFragments = 0, expiredRepairs = 0, retiredRepairs = 0, duplicateRepairs = 0;
    void pollRepairs(qint64 now);
    int socketWaitMs(qint64 now) const;
    std::function<void(std::optional<quint32>)> onReference;
    std::optional<quint32> acknowledgedReference() const;
    bool rowReuse = false;
    bool dictionarySupported = false;
    qint64 dictionaryDecodeNs = 0, dictionaryMaxDecodeNs = 0, dictionaryDecoded = 0;
    qint64 rowKeyPairs = 0, rowReferenceMisses = 0, rowInvalid = 0, rowRepeatAcks = 0;
    Counts counts;
    bool combined = false;
    void reset();
    void expire(qint64 now);
    std::optional<Pair> feed(QByteArrayView packet, qint64 now);
};
} // namespace live
