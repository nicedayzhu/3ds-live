#pragma once
#include "transport.h"
#include "timeline.h"
#include "imaging.h"
#include <condition_variable>
#include <thread>
namespace live {
class Pipeline {
    struct Packet {
        Pair pair;
        SourceFrame source;
        int sessionEpoch;
    };
    struct Job {
        SourceFrame source;
        Images images;
        int generation, scale;
        QString mode, motion;
        std::array<int, 2> jpegQuality;
    };
    struct MotionJob {
        SourceFrame source;
        Images before, after;
        int generation, scale;
        QString method, quality;
        Images previousHd;
        DeviceImages beforeGpu, afterGpu;
    };
    Timeline &timeline;
    QString models;
    std::atomic<int> scale;
    std::mutex mutex;
    std::condition_variable decodeReady, enhanceReady, motionReady;
    bool done = false, preparing = false, strictPicture = false;
    QString mode, motion, rifePrecision, error, motionError, fbcnnBackend, fbcnnFallback, rifeBackend, rifeFallback;
    int generation = 0, motionGeneration = 0, sessionEpoch = 0;
    std::deque<Packet> pending;
    // Snapshot both compressed panels in every queued job so coalescing cannot lose
    // an update of the other screen. A missing lower panel stays blank on startup.
    Screens latestWire;
    std::array<int, 2> latestQuality{};
    SourceFrame latestSource;
    std::deque<Job> jobs;
    std::optional<MotionJob> pendingMotion;
    std::thread decoder, enhancer, interpolator;
    std::atomic<qint64> decoded{0}, skipped{0}, invalid{0}, enhanced{0}, overwritten{0}, cache{0}, inferUs{0},
        interpolated{0}, motionReplaced{0}, motionPredictedLate{0}, motionTimeUs{0}, guardedPairs{0}, heldPairs{0},
        motionDeviceInputs{0},
        decodeTotalUs{0}, restoreTotalUs{0}, composeTotalUs{0}, restoreCalls{0}, motionWorkTotalUs{0};
    void decodeLoop();
    void enhanceLoop();
    void motionLoop();
    void clearSessionLocked(quint32 session);

  public:
    Pipeline(Timeline &timeline, QString models, int scale, QString mode, QString motion, QString rifePrecision = {});
    ~Pipeline();
    void resetSession(quint32 session);
    void submit(Pair pair, SourceFrame source);
    void configure(QString mode, QString motion, int outputScale, QString rifePrecision = {});
    QJsonObject stats();
};
} // namespace live
