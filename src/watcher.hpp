// watcher.hpp - ReadDirectoryChangesW watcher over chat_data (recursive).
#pragma once
#include "common.hpp"
#include <functional>
#include <thread>
#include <atomic>

namespace kw {

class DirWatcher {
public:
    using EventFn = std::function<void(const wstring& fullPath, bool added)>;
    bool start(const wstring& dir, EventFn onEvent);
    void stop();
    ~DirWatcher();
private:
    void run_();
    wstring dir_;
    EventFn cb_;
    std::thread th_;
    HANDLE hDir_ = INVALID_HANDLE_VALUE;
    HANDLE hStop_ = nullptr;
    std::atomic<bool> running_{false};
};

} // namespace kw
