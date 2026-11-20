#pragma once
#include "timeline.h"
#include <thread>
namespace live {
class Recording {
    Timeline &timeline;
    QString path;
    int scale, fps;
    bool allowReplace;
    std::thread worker;
    std::atomic<qint64> stopTime{0}, startTime{0};
    mutable std::mutex mutex;
    QString state = "starting", error;
    void run();

  public:
    Recording(Timeline &timeline, QString path, int scale, int fps, bool allowReplace = false);
    ~Recording();
    void stop();
    QJsonObject stats() const;
};
} // namespace live
