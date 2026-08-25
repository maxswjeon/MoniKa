// watcher.cpp
#include "watcher.hpp"

namespace kw {

bool DirWatcher::start(const wstring& dir, EventFn onEvent) {
    dir_ = dir;
    cb_ = std::move(onEvent);
    if (!dir_exists(dir_)) {
        LOGE("watch dir missing: %s", wide_to_utf8(dir_).c_str());
        return false;
    }
    hDir_ = CreateFileW(dir_.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (hDir_ == INVALID_HANDLE_VALUE) {
        LOGE("open watch dir failed %lu", GetLastError());
        return false;
    }
    hStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hStop_) {
        LOGE("create stop event failed %lu", GetLastError());
        CloseHandle(hDir_);
        hDir_ = INVALID_HANDLE_VALUE;
        return false;
    }
    running_ = true;
    th_ = std::thread([this] { run_(); });
    LOGI("watching %s", wide_to_utf8(dir_).c_str());
    return true;
}

void DirWatcher::run_() {
    vector<uint8_t> buf(64 * 1024);
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        LOGE("create watch event failed %lu", GetLastError());
        return;
    }
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                         FILE_NOTIFY_CHANGE_CREATION;
    while (running_) {
        ResetEvent(ov.hEvent);
        DWORD ret = 0;
        if (!ReadDirectoryChangesW(hDir_, buf.data(), (DWORD)buf.size(), TRUE, filter, &ret, &ov, nullptr)) {
            LOGE("ReadDirectoryChangesW failed %lu", GetLastError());
            break;
        }
        HANDLE hs[2] = {ov.hEvent, hStop_};
        DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 + 1) {
            CancelIoEx(hDir_, &ov);
            GetOverlappedResult(hDir_, &ov, &ret,
                                TRUE); // keep ov/buffer alive through cancellation
            break;
        }
        if (w != WAIT_OBJECT_0) {
            LOGE("directory wait failed %lu", GetLastError());
            break;
        }
        DWORD bytes = 0;
        if (!GetOverlappedResult(hDir_, &ov, &bytes, FALSE)) {
            if (GetLastError() == ERROR_NOTIFY_ENUM_DIR && cb_)
                cb_(L"", false);
            continue;
        }
        if (bytes == 0) {
            if (cb_)
                cb_(L"", false);
            continue;
        }

        size_t off = 0;
        for (;;) {
            if (off > bytes || bytes - off < offsetof(FILE_NOTIFY_INFORMATION, FileName)) {
                LOGW("truncated directory notification; reconciling");
                if (cb_)
                    cb_(L"", false);
                break;
            }
            auto* fni = (FILE_NOTIFY_INFORMATION*)(buf.data() + off);
            size_t entrySize = offsetof(FILE_NOTIFY_INFORMATION, FileName) + fni->FileNameLength;
            if ((fni->FileNameLength % sizeof(wchar_t)) != 0 || entrySize > bytes - off) {
                LOGW("malformed directory notification; reconciling");
                if (cb_)
                    cb_(L"", false);
                break;
            }
            wstring name(fni->FileName, fni->FileNameLength / sizeof(wchar_t));
            wstring full = dir_ + L"\\" + name;
            bool added = (fni->Action == FILE_ACTION_ADDED || fni->Action == FILE_ACTION_RENAMED_NEW_NAME);
            if (cb_)
                cb_(full, added);
            if (fni->NextEntryOffset == 0)
                break;
            if (fni->NextEntryOffset > bytes - off) {
                LOGW("invalid directory notification offset; reconciling");
                if (cb_)
                    cb_(L"", false);
                break;
            }
            off += fni->NextEntryOffset;
        }
    }
    CloseHandle(ov.hEvent);
}

void DirWatcher::stop() {
    running_ = false;
    if (hStop_)
        SetEvent(hStop_);
    if (th_.joinable())
        th_.join();
    if (hDir_ != INVALID_HANDLE_VALUE) {
        CloseHandle(hDir_);
        hDir_ = INVALID_HANDLE_VALUE;
    }
    if (hStop_) {
        CloseHandle(hStop_);
        hStop_ = nullptr;
    }
}

DirWatcher::~DirWatcher() {
    stop();
}

} // namespace kw
