#pragma once
#include "diagnostics.h"
#include <QImage>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
namespace live {
struct PresentedFrame {
    SourceFrame source;
    QImage image;
    QString quality;
    bool synthetic = false;
    qint64 time = 0, id = 0;
    int updatedPanels = 0, generatedPanels = 0;
};
class Timeline : public QObject {
    Q_OBJECT
    mutable std::mutex mutex;
    quint32 session = 0;
    bool active = false, mapped = false, audioAnchor = false;
    qint64 newest = 0, offset = 0;
    quint32 anchorSeq = 0;
    double anchorPts = 0;
    struct Frame {
        SourceFrame source;
        QImage original, enhanced;
        QString quality;
        bool synthetic = false;
        qint64 displayPts = 0;
        int generatedPanels = 0;
    };
    std::map<qint64, Frame> frames;
    std::deque<PresentedFrame> choices;
    std::array<std::optional<PanelStamp>, 2> lastPanels{};
    std::optional<qint64> lastChoiceKey;
    QImage lastRealCanvas;
    bool holdingGeneratedTop = false;
    qint64 nextChoice = 0, lastDisplayPts = 0;
    std::optional<qint64> lastEnhancedPts;
    double enhancedPeriodUs = 33333.;
    bool newSession = true, motionEnabled = false;
    QString requiredQuality;
    std::optional<qint64> lastRawPts;
    qint64 rawDisplayPts = 0;
    qint64 nextWakeAt = 0;
    void notifyEarlierFrame(qint64 displayPts);
    void paceSource(Frame &frame, int fps);
    std::map<qint64, QByteArray> audio;
    QSet<quint32> seen;
    std::deque<quint32> order;
    qint64 observe(quint32 raw, qint64 wall);

  public:
    std::atomic<qint64> audioBlocks{0}, duplicates{0}, invalidAudio{0};
    Diagnostics diagnostics;
    const int delay;
    explicit Timeline(int delayMs) : delay(delayMs * 1000) {}
    void reset(quint32 id);
    SourceFrame arrival(quint32 id, quint32 seq, quint32 pts, qint64 wall, int panelMask = 3,
                        std::optional<quint32> bottomPts = {});
    void put(SourceFrame source, const QImage &image, const QString &quality = {}, bool synthetic = false,
             int fps = 0, int generatedPanels = 3);
    qint64 deadline(SourceFrame source) const;
    void setPictureMode(const QString &mode, bool interpolate = false, bool keepDisplayed = false);
    void clearSynthetic();
    PresentedFrame present(int *nextWakeMs = nullptr);
    PresentedFrame recorded(qint64 wall) const;
    void addAudio(QByteArrayView packet, qint64 wall);
    QByteArray pcm(qint64 wall, int samples) const;
  signals:
    void earlierFrameReady();
};
class Speaker {
    std::atomic<bool> stop{false};
    std::unique_ptr<QThread> worker;

  public:
    std::atomic<bool> mute{false};
    std::atomic<bool> opened{false};
    explicit Speaker(Timeline &timeline);
    ~Speaker();
};
} // namespace live
