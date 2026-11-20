#pragma once
#include "pipeline.h"
#include <QHostAddress>
namespace live {
const QList<int> &rates();
class Receiver {
    Timeline &timeline;
    Pipeline &pipeline;
    QHostAddress sender;
    quint16 port, handheldDiscoveryPort;
    std::atomic<bool> stop{false};
    std::atomic<qint64> lastPairUs{0}, lastPacketUs{0}, lastTopUs{0};
    std::thread worker;
    std::mutex mutex;
    QJsonObject snapshot;
    std::atomic<int> requestedFps, requestedRate, revision{0};
    void run();

  public:
    Receiver(Timeline &timeline, Pipeline &pipeline, QHostAddress sender, quint16 port, int fps, int rate,
             quint16 handheldDiscoveryPort);
    ~Receiver();
    void configure(int fps, int rate);
    QJsonObject stats();
};
} // namespace live
