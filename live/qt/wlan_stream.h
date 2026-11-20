#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <QtGlobal>

namespace live {
// The Windows WLAN service may block. Keep driver requests off the receive
// and presentation threads, and release our request when live traffic stops.
class WlanStream {
    std::mutex mutex;
    std::condition_variable wake;
    quint32 peer = 0;
    bool stop = false;
    std::thread worker;
    void run();
  public:
    std::atomic<bool> enabled{false};
    std::atomic<unsigned long> error{0};
    WlanStream();
    ~WlanStream();
    void setPeer(quint32 address);
};
}
