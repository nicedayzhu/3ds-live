#include "wlan_stream.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wlanapi.h>
#include <chrono>

namespace live {
WlanStream::WlanStream() { worker = std::thread(&WlanStream::run, this); }
WlanStream::~WlanStream() {
    {
        std::lock_guard lock(mutex);
        stop = true;
    }
    wake.notify_one();
    worker.join();
}
void WlanStream::setPeer(quint32 address) {
    std::lock_guard lock(mutex);
    if (address != peer) {
        peer = address;
        wake.notify_one();
    }
}
void WlanStream::run() {
    HANDLE client = nullptr;
    GUID active{};
    auto release = [&] {
        if (client) {
            BOOL off = FALSE;
            WlanSetInterface(client, &active, wlan_intf_opcode_media_streaming_mode, sizeof(off), &off, nullptr);
            WlanCloseHandle(client, nullptr);
            client = nullptr;
        }
        enabled = false;
    };
    quint32 previous = 0;
    std::unique_lock lock(mutex);
    while (!stop) {
        wake.wait_for(lock, std::chrono::seconds(30), [&] { return stop || peer != previous; });
        if (stop) break;
        previous = peer;
        lock.unlock();
        MIB_IF_ROW2 row{};
        const bool wlan = previous && (previous >> 24) != 127 &&
                          GetBestInterface(htonl(previous), &row.InterfaceIndex) == NO_ERROR &&
                          GetIfEntry2(&row) == NO_ERROR &&
                          row.Type == IF_TYPE_IEEE80211 && row.OperStatus == IfOperStatusUp;
        if (!wlan || (client && !IsEqualGUID(active, row.InterfaceGuid)))
            release();
        if (wlan && !client) {
            DWORD version = 0;
            HANDLE session = nullptr;
            DWORD rc = WlanOpenHandle(2, nullptr, &version, &session);
            if (rc == ERROR_SUCCESS) {
                BOOL on = TRUE;
                rc = WlanSetInterface(session, &row.InterfaceGuid, wlan_intf_opcode_media_streaming_mode,
                                      sizeof(on), &on, nullptr);
                if (rc == ERROR_SUCCESS) {
                    active = row.InterfaceGuid;
                    client = session;
                    enabled = true;
                } else WlanCloseHandle(session, nullptr);
            }
            error = rc;
        } else if (!wlan) error = 0;
        lock.lock();
    }
    lock.unlock();
    release();
}
}
