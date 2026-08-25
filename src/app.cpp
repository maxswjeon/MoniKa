// app.cpp
#include "app.hpp"
#include "scanner.hpp"
#include <tlhelp32.h>
#include <algorithm>

namespace kw {

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
        if (dir_exists(cand) && GetFileAttributesExW((cand + L"\\keystore.bin").c_str(),
                                                     GetFileExInfoStandard, &data)) {
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

    wstring appdir = env_var(L"LOCALAPPDATA") + L"\\kakao_watcher";
    CreateDirectoryW(appdir.c_str(), nullptr);
    cacheDbPath_ = appdir + L"\\cache.db";

    if (!oracle_.init()) {
        LOGE("oracle/BCrypt init failed");
        return false;
    }
    oracle_.load_edb_dir(userDir_);
    if (!cache_.init(cacheDbPath_))
        return false;
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
}

void App::request_scan(DWORD pid, int delayMs) {
    if (!pid)
        return;
    auto due = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
    {
        std::lock_guard<std::mutex> lk(jobsMu_);
        if (stopping_)
            return;
        for (auto& job : jobs_)
            if (job.pid == pid) {
                auto gap = (due > job.due) ? due - job.due : job.due - due;
                if (gap < std::chrono::seconds(2)) {
                    if (due < job.due)
                        job.due = due;
                    jobsCv_.notify_all();
                    return;
                }
            }
        jobs_.push_back({pid, due});
    }
    jobsCv_.notify_all();
}

void App::scan_worker_() {
    std::unique_lock<std::mutex> lk(jobsMu_);
    while (!stopping_) {
        if (jobs_.empty()) {
            jobsCv_.wait(lk, [this] { return stopping_ || !jobs_.empty(); });
            continue;
        }
        auto it =
            std::min_element(jobs_.begin(), jobs_.end(),
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
        jobs_.erase(it);
        lk.unlock();
        scan_pid(pid);
        lk.lock();
    }
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
    Scanner::scan_process(pid, oracle_, [&](const DekHit& hit, uintptr_t va) {
        bool isNew = cache_.put_dek(hit);
        if (isNew) {
            ++newly;
            LOGI("  armed DEK: %s (reserved=%d) @0x%llx", hit.rel.c_str(), hit.reserved,
                 (unsigned long long)va);
        }
    });
    LOGI("scan done: %d newly-armed", newly);
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
    else
        LOGI("KakaoTalk not running; nothing to scan");
}

void App::on_file_event(const wstring& fullPath, bool /*added*/) {
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

    cache_.touch_tag(rel, "changed");

    // New/unknown room changed -> its connection is likely open now: try to grab its DEK.
    if (!cache_.has_dek(rel)) {
        // make sure the oracle knows this file (it may be brand new)
        oracle_.add_edb(edbFull, rel);
        DWORD pid = find_kakaotalk_pid();
        if (pid) {
            LOGI("new/unknown edb changed (%s) -> targeted rescan", rel.c_str());
            request_scan(pid);
        }
    }
}

} // namespace kw
