// app.cpp
#include "app.hpp"
#include "scanner.hpp"
#include <tlhelp32.h>
#include <algorithm>
#include <cstring>

namespace kw {

static string profile_id_from_dir(const wstring& userDir) {
    size_t slash = userDir.find_last_of(L"\\/");
    return wide_to_utf8(userDir.substr(slash == wstring::npos ? 0 : slash + 1));
}

// Find %LOCALAPPDATA%\Kakao\KakaoTalk\users\<40hex> that contains keystore.bin
static wstring discover_user_dir() {
    wstring base = env_var(L"LOCALAPPDATA");
    if (base.empty())
        return L"";
    wstring users = base + L"\\Kakao\\KakaoTalk\\users";
    if (!dir_exists(users))
        return L"";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((users + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return L"";
    wstring best;
    ULONGLONG bestWrite = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        wstring name = fd.cFileName;
        if (name.size() != 40)
            continue; // 40-hex user hash
        bool hex = true;
        for (wchar_t c : name)
            if (!iswxdigit(c)) {
                hex = false;
                break;
            }
        if (!hex)
            continue;
        wstring cand = users + L"\\" + name;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (dir_exists(cand) &&
            GetFileAttributesExW((cand + L"\\keystore.bin").c_str(), GetFileExInfoStandard, &data)) {
            ULARGE_INTEGER t;
            t.LowPart = data.ftLastWriteTime.dwLowDateTime;
            t.HighPart = data.ftLastWriteTime.dwHighDateTime;
            if (best.empty() || t.QuadPart > bestWrite) {
                best = cand;
                bestWrite = t.QuadPart;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

bool App::init() {
    userDir_ = discover_user_dir();
    if (userDir_.empty()) {
        LOGE("could not find KakaoTalk user dir under %%LOCALAPPDATA%%");
        return false;
    }
    chatDataDir_ = userDir_ + L"\\chat_data";
    LOGI("user dir: %s", wide_to_utf8(userDir_).c_str());

    wstring appdir = env_var(L"LOCALAPPDATA") + L"\\MoniKa";
    CreateDirectoryW(appdir.c_str(), nullptr);
    cacheDbPath_ = appdir + L"\\cache.db";

    if (!oracle_.init()) {
        LOGE("oracle/BCrypt init failed");
        return false;
    }
    oracle_.load_edb_dir(userDir_);
    if (!cache_.init(cacheDbPath_))
        return false;
    if (!find_kakaotalk_pid())
        cache_.put_session("not_running", profile_id_from_dir(userDir_), "");
    refresh_armed_();
    scanThread_ = std::thread([this] { scan_worker_(); });
    return true;
}

void App::shutdown() {
    {
        std::lock_guard<std::mutex> lk(jobsMu_);
        stopping_ = true;
        jobs_.clear();
    }
    jobsCv_.notify_all();
    if (scanThread_.joinable())
        scanThread_.join();
    std::lock_guard<std::mutex> lk(poolMu_);
    pool_.clear();
    tried_.clear();
}

void App::requeue_tried_candidates_() {
    std::lock_guard<std::mutex> lk(poolMu_);
    pool_.insert(tried_.begin(), tried_.end());
    tried_.clear();
}

void App::enqueue_(DWORD pid, int delayMs, JobMode mode, bool coalesce) {
    if (!pid)
        return;
    auto due = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
    {
        std::lock_guard<std::mutex> lk(jobsMu_);
        if (stopping_)
            return;
        if (coalesce) {
            for (auto& job : jobs_)
                if (job.pid == pid && job.mode == mode) {
                    auto gap = (due > job.due) ? due - job.due : job.due - due;
                    if (gap < std::chrono::seconds(2)) {
                        if (due < job.due)
                            job.due = due;
                        jobsCv_.notify_all();
                        return;
                    }
                }
        }
        jobs_.push_back({pid, due, mode});
    }
    jobsCv_.notify_all();
}

void App::request_scan(DWORD pid, int delayMs) {
    enqueue_(pid, delayMs, JobMode::Normal, /*coalesce=*/true);
}

// Front-loaded burst of cheap anchor-only collects during KakaoTalk's open-storm,
// interleaved with validation passes so the tray count climbs as we confirm keys.
void App::start_storm(DWORD pid) {
    if (!pid)
        return;
    LOGI("storm capture armed for pid=%lu", pid);
    // KakaoTalk opens its DBs a few seconds in (codec structs appear ~5-20s after
    // launch), so weight the collects across that window rather than the first 1s.
    const int collectMs[] = {2000, 4000, 6000, 8000, 11000, 15000, 20000, 26000, 33000};
    for (int ms : collectMs)
        enqueue_(pid, ms, JobMode::StormCollect, /*coalesce=*/false);
    const int validateMs[] = {5000, 9000, 16000, 27000, 34000};
    for (int ms : validateMs)
        enqueue_(pid, ms, JobMode::Validate, /*coalesce=*/false);
    enqueue_(pid, 30000, JobMode::RefreshEdbList, /*coalesce=*/true);
    enqueue_(pid, 36000, JobMode::Normal, /*coalesce=*/true);
}

// A single cheap collect+validate, coalesced so a burst of file events (e.g. the
// startup write-storm) collapses into one pass instead of many full scans.
void App::request_storm_tick(DWORD pid) {
    enqueue_(pid, 300, JobMode::StormCollect, /*coalesce=*/true);
    enqueue_(pid, 1500, JobMode::Validate, /*coalesce=*/true);
}

void App::scan_worker_() {
    std::unique_lock<std::mutex> lk(jobsMu_);
    while (!stopping_) {
        if (jobs_.empty()) {
            jobsCv_.wait(lk, [this] { return stopping_ || !jobs_.empty(); });
            continue;
        }
        auto it = std::min_element(jobs_.begin(), jobs_.end(),
                                   [](const ScanJob& a, const ScanJob& b) { return a.due < b.due; });
        auto due = it->due;
        if (jobsCv_.wait_until(lk, due, [this, due] {
                if (stopping_)
                    return true;
                for (const auto& j : jobs_)
                    if (j.due < due)
                        return true;
                return false;
            }))
            continue;
        it = std::min_element(jobs_.begin(), jobs_.end(),
                              [](const ScanJob& a, const ScanJob& b) { return a.due < b.due; });
        if (it == jobs_.end() || it->due > std::chrono::steady_clock::now())
            continue;
        DWORD pid = it->pid;
        JobMode mode = it->mode;
        jobs_.erase(it);
        lk.unlock();
        switch (mode) {
        case JobMode::Normal:
            scan_pid(pid);
            break;
        case JobMode::StormCollect:
            storm_capture(pid);
            break;
        case JobMode::Validate:
            validate_pool();
            break;
        case JobMode::RefreshEdbList:
            refresh_edb_listing_();
            break;
        }
        lk.lock();
    }
}

void App::refresh_edb_listing_() {
    size_t before = oracle_.file_count();
    oracle_.load_edb_dir(userDir_);
    size_t after = oracle_.file_count();
    if (after > before)
        requeue_tried_candidates_();

    const string chatPrefix = "chat_data\\";
    size_t chatCount = 0;
    LOGI("chatroom .edb listing after login storm (+30s):");
    for (const auto& rel : oracle_.relative_paths()) {
        if (rel.size() >= chatPrefix.size() && _strnicmp(rel.c_str(), chatPrefix.c_str(), chatPrefix.size()) == 0 &&
            GetFileAttributesW((userDir_ + L"\\" + utf8_to_wide(rel)).c_str()) != INVALID_FILE_ATTRIBUTES) {
            LOGI("  %s", rel.c_str());
            ++chatCount;
        }
    }
    LOGI("chatroom .edb listing complete: %zu files", chatCount);
}

void App::refresh_armed_() {
    int n = cache_.count_armed();
    int prev = armed_.exchange(n);
    if (n != prev && onArmedChanged)
        onArmedChanged(n);
}

void App::scan_pid(DWORD pid) {
    std::lock_guard<std::mutex> lk(scanMu_);
    LOGI("scanning KakaoTalk pid=%lu ...", pid);
    int newly = 0;
    int resident = Scanner::scan_process(pid, oracle_, [&](const DekHit& hit, uintptr_t va) {
        bool isNew = cache_.put_dek(hit);
        if (isNew) {
            ++newly;
            LOGI("  armed DEK: %s (reserved=%d) @0x%llx", hit.rel.c_str(), hit.reserved, (unsigned long long)va);
        }
    });
    SessionInfo session = Scanner::inspect_session(pid);
    if (session.state == "signed_in_candidate" && resident > 0)
        session.state = "signed_in";
    cache_.put_session(session.state, profile_id_from_dir(userDir_), session.hashedTalkUserId);
    LOGI("scan done: %d newly-armed", newly);
    refresh_armed_();
}

// One fast anchor-only pass. Just caches candidate keys in memory (no AES, no
// save). Runs repeatedly during the storm to snapshot transient DEKs before the
// heap reuses their pages.
void App::storm_capture(DWORD pid) {
    size_t added = 0;
    Scanner::collect_candidates(pid, [&](const uint8_t* k) {
        Key32 a;
        memcpy(a.data(), k, 32);
        std::lock_guard<std::mutex> lk(poolMu_);
        if (tried_.find(a) == tried_.end() && pool_.insert(a).second)
            ++added;
    });
    size_t pending;
    {
        std::lock_guard<std::mutex> lk(poolMu_);
        pending = pool_.size();
    }
    LOGI("storm: +%zu new candidates (pending=%zu)", added, pending);
}

// Validate the accumulated candidate pool with the oracle; persist ONLY the ones
// that decrypt an .edb page-1 header. Everything tried is remembered so repeated
// validations stay incremental.
void App::validate_pool() {
    vector<Key32> batch;
    {
        std::lock_guard<std::mutex> lk(poolMu_);
        batch.assign(pool_.begin(), pool_.end());
        pool_.clear();
    }
    if (batch.empty())
        return;
    int newly = 0;
    for (const auto& a : batch) {
        auto hit = oracle_.test_key(a.data());
        if (hit && cache_.put_dek(*hit)) {
            ++newly;
            LOGI("  armed DEK (storm): %s (reserved=%d)", hit->rel.c_str(), hit->reserved);
        }
    }
    {
        std::lock_guard<std::mutex> lk(poolMu_);
        for (const auto& a : batch)
            tried_.insert(a);
    }
    LOGI("validate: %zu candidates -> %d newly-armed", batch.size(), newly);
    refresh_armed_();
}

DWORD App::find_kakaotalk_pid() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (to_lower(pe.szExeFile) == KAKAO_IMAGE) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

void App::rescan_running() {
    DWORD pid = find_kakaotalk_pid();
    if (pid)
        request_scan(pid);
    else {
        cache_.put_session("not_running", profile_id_from_dir(userDir_), "");
        LOGI("KakaoTalk not running; nothing to scan");
    }
}

void App::on_file_event(const wstring& fullPath, bool added) {
    if (fullPath.empty()) {
        LOGW("directory notifications overflowed; reconciling all .edb files");
        oracle_.load_edb_dir(userDir_);
        requeue_tried_candidates_();
        DWORD pid = find_kakaotalk_pid();
        if (pid)
            request_scan(pid);
        return;
    }
    // derive rel path under userDir_
    if (fullPath.size() <= userDir_.size() + 1)
        return;
    wstring low = to_lower(fullPath);
    bool is_edb = iends_with(low, L".edb");
    bool is_wal = iends_with(low, L".edb-wal");
    if (!is_edb && !is_wal)
        return;

    wstring edbFull = is_wal ? fullPath.substr(0, fullPath.size() - 4) : fullPath; // strip -wal
    string rel = wide_to_utf8(edbFull.substr(userDir_.size() + 1));

    LOGI("database file %s: %s%s", added ? "added" : "changed", rel.c_str(), is_wal ? "-wal" : "");

    cache_.touch_tag(rel, "changed");

    // New/unknown room changed -> its connection is likely open now: try to grab
    // its DEK via the cheap coalesced collect+validate (NOT a full scan per file,
    // which would stampede during the startup write-storm).
    if (!cache_.has_dek(rel)) {
        size_t oldFileCount = oracle_.file_count();
        if (oracle_.add_edb(edbFull, rel) && oracle_.file_count() > oldFileCount)
            requeue_tried_candidates_();
        DWORD pid = find_kakaotalk_pid();
        if (pid)
            request_storm_tick(pid);
    }
}

} // namespace kw
