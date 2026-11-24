#pragma once
#include "pipeline.h"
#include <QHostAddress>
namespace live {
const QList<int> &rates();
bool isSourceJpegQuality(quint32 quality);
class Receiver {
    Timeline &timeline;
    Pipeline &pipeline;
    QHostAddress sender;
    quint16 port, handheldDiscoveryPort;
    std::optional<int> requestedPacing;
    std::atomic<bool> stop{false};
    std::atomic<qint64> lastPairUs{0}, lastPacketUs{0}, lastTopUs{0};
    std::thread worker;
    std::mutex mutex;
    QJsonObject snapshot, latestDelivery;
    struct Settings {
        int fps, rate, quality;
        bool adaptive;
        quint64 revision = 0;
    };
    Settings requested;
    void run();

  public:
    Receiver(Timeline &timeline, Pipeline &pipeline, QHostAddress sender, quint16 port, int fps, int rate,
             quint16 handheldDiscoveryPort, int jpegQuality = 0, bool adaptiveRate = true,
             std::optional<int> packetPacing = {});
    ~Receiver();
    void configure(int fps, int rate, int jpegQuality = 0, bool adaptiveRate = true);
    QJsonObject stats();
};
} // namespace live
