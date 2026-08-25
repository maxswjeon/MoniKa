// app.hpp - orchestration: user-dir discovery, scan-on-start, file-event handling.
#pragma once
#include "common.hpp"
#include "oracle.hpp"
#include "dek_cache.hpp"
#include <functional>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <array>
#include <set>

namespace kw {

class App {
  public:
    // onArmedChanged(count) is called whenever the armed-DEK count changes (tray).
    std::function<void(int)> onArmedChanged;

    bool init();              // discover dirs, load oracle probe, open cache
    void shutdown();          // cancel and join queued scans
    void scan_pid(DWORD pid); // best-effort: validate + save all resident DEKs
    void request_scan(DWORD pid, int delayMs = 0);
    void rescan_running();                                   // find KakaoTalk.exe via Toolhelp and scan it
    void on_file_event(const wstring& fullPath, bool added); // from the dir watcher

    // Storm capture: on KakaoTalk start, run a front-loaded burst of cheap
    // anchor-only collects (no AES) into an in-memory pool, then validate the
    // accumulated candidates and persist ONLY those that pass the oracle.
    void start_storm(DWORD pid);        // schedule the burst + trailing validations
    void storm_capture(DWORD pid);      // one fast collect pass into the pool
    void validate_pool();               // validate un-tried candidates; save hits only
    void request_storm_tick(DWORD pid); // coalesced cheap collect+validate (for file events)

    const wstring& user_dir() const { return userDir_; }
    const wstring& chat_data() const { return chatDataDir_; }
    int armed() const { return armed_.load(); }

    static DWORD find_kakaotalk_pid(); // 0 if not running

  private:
    void refresh_armed_();
    wstring userDir_, chatDataDir_, cacheDbPath_;
    Oracle oracle_;
    DekCache cache_;
    std::atomic<int> armed_{0};
    std::mutex scanMu_;

    enum class JobMode { Normal, StormCollect, Validate, RefreshEdbList };
    struct ScanJob {
        DWORD pid;
        std::chrono::steady_clock::time_point due;
        JobMode mode = JobMode::Normal;
    };
    void enqueue_(DWORD pid, int delayMs, JobMode mode, bool coalesce);
    std::mutex jobsMu_;
    std::condition_variable jobsCv_;
    vector<ScanJob> jobs_;
    std::thread scanThread_;
    bool stopping_ = false;
    void scan_worker_();
    void requeue_tried_candidates_();
    void refresh_edb_listing_();

    // storm candidate pool (32-byte keys); tried_ = already validated (hit or miss).
    struct Key32 {
        std::array<uint8_t, 32> bytes{};
        Key32() = default;
        Key32(const Key32&) = default;
        Key32& operator=(const Key32&) = default;
        ~Key32() { SecureZeroMemory(bytes.data(), bytes.size()); }
        uint8_t* data() { return bytes.data(); }
        const uint8_t* data() const { return bytes.data(); }
        bool operator<(const Key32& other) const { return bytes < other.bytes; }
    };
    std::mutex poolMu_;
    std::set<Key32> pool_;  // pending validation
    std::set<Key32> tried_; // already validated, skip
};

} // namespace kw
