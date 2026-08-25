// etw.hpp - real-time Microsoft-Windows-Kernel-Process consumer.
// Fires callback(pid, imageName) when any process starts; caller filters.
#pragma once
#include "common.hpp"
#include <functional>
#include <thread>
#include <atomic>

namespace kw {

class EtwProcessMonitor {
  public:
    using StartFn = std::function<void(DWORD pid, const wstring& image)>;
    bool start(StartFn onStart); // needs admin (kernel provider)
    void stop();
    ~EtwProcessMonitor();

    // internal (called from ETW callback)
    void handle_record(void* eventRecord);

  private:
    StartFn cb_;
    std::thread th_;
    unsigned long long traceHandle_ = 0; // TRACEHANDLE
    unsigned long long session_ = 0;     // TRACEHANDLE for control
    std::atomic<bool> running_{false};
    wstring sessionName_ = L"KakaoWatcherKP";
    vector<uint8_t> propsBuf_;
};

} // namespace kw
