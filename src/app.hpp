// app.hpp - orchestration: user-dir discovery, scan-on-start, file-event handling.
#pragma once
#include "common.hpp"
#include "oracle.hpp"
#include "dek_cache.hpp"
#include <functional>
#include <atomic>

namespace kw {

class App {
public:
    // onArmedChanged(count) is called whenever the armed-DEK count changes (tray).
    std::function<void(int)> onArmedChanged;

    bool init();                        // discover dirs, load oracle probe, open cache
    void scan_pid(DWORD pid);           // best-effort: capture all resident DEKs
    void rescan_running();              // find KakaoTalk.exe via Toolhelp and scan it
    void on_file_event(const wstring& fullPath, bool added); // from the dir watcher

    const wstring& user_dir()  const { return userDir_; }
    const wstring& chat_data() const { return chatDataDir_; }
    int armed() const { return armed_.load(); }

    static DWORD find_kakaotalk_pid();  // 0 if not running

private:
    void refresh_armed_();
    wstring   userDir_, chatDataDir_, cacheDbPath_;
    Oracle    oracle_;
    DekCache  cache_;
    std::atomic<int> armed_{0};
    std::mutex scanMu_;
};

} // namespace kw
