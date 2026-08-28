// main.cpp - wire ETW + scanner + watcher + cache + tray together.
#include "common.hpp"
#include "app.hpp"
#include "etw.hpp"
#include "watcher.hpp"
#include "tray.hpp"
#include <shellapi.h>
#include <tlhelp32.h>

using namespace kw;

static App g_app;
static EtwProcessMonitor g_etw;
static DirWatcher g_watch;
static Tray g_tray;

static bool is_process_elevated(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                    elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

static wstring executable_path() {
    vector<wchar_t> path(32768);
    DWORD len = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    return (len && len < path.size()) ? wstring(path.data(), len) : L"";
}

// Returns true only when an elevated replacement was successfully launched.
// ERROR_CANCELLED deliberately falls through so the current process can continue
// with reduced functionality instead of disappearing.
static bool try_relaunch_elevated() {
    wstring path = executable_path();
    if (path.empty())
        return false;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = path.c_str();
    sei.lpParameters = L"--elevated";
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        DWORD error = GetLastError();
        if (error == ERROR_CANCELLED)
            LOGW("administrator elevation cancelled; continuing non-elevated");
        else
            LOGW("administrator elevation failed (%lu); continuing non-elevated", error);
        return false;
    }
    if (sei.hProcess)
        CloseHandle(sei.hProcess);
    return true;
}

static void stop_non_elevated_copies() {
    const DWORD selfPid = GetCurrentProcessId();
    const wstring selfPath = executable_path();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == selfPid || _wcsicmp(entry.szExeFile, L"MoniKa.exe") != 0)
                continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                         entry.th32ProcessID);
            if (!process)
                continue;
            vector<wchar_t> path(32768);
            DWORD pathLen = (DWORD)path.size();
            bool sameBinary = QueryFullProcessImageNameW(process, 0, path.data(), &pathLen) &&
                              _wcsicmp(wstring(path.data(), pathLen).c_str(), selfPath.c_str()) == 0;
            if (sameBinary && !is_process_elevated(process)) {
                LOGI("stopping non-elevated previous instance pid=%lu", entry.th32ProcessID);
                if (TerminateProcess(process, 0))
                    WaitForSingleObject(process, 5000);
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

// Scan KakaoTalk a few seconds after start so its DBs (and DEKs) are loaded.
static void delayed_scan(DWORD pid, int delayMs) {
    g_app.request_scan(pid, delayMs);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const bool elevated = is_process_elevated(GetCurrentProcess());
    if (!elevated && try_relaunch_elevated())
        return 0;
    if (elevated)
        stop_non_elevated_copies();

    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\MoniKa_single_instance");
    if (!instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (instanceMutex)
            CloseHandle(instanceMutex);
        MessageBoxW(nullptr, L"MoniKa is already running in the notification area.", L"MoniKa",
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    LOGI("MoniKa starting (%s; self-use: your own account/PC)", elevated ? "administrator" : "non-elevated");
    if (!g_app.init()) {
        LOGE("init failed");
        MessageBoxW(nullptr, L"MoniKa could not initialize.\n\nSee %LOCALAPPDATA%\\MoniKa\\watcher.log for details.",
                    L"MoniKa", MB_OK | MB_ICONERROR);
        CloseHandle(instanceMutex);
        return 1;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!g_tray.create(hInst)) {
        MessageBoxW(nullptr,
                    L"MoniKa could not create its tray icon.\n\nSee %LOCALAPPDATA%\\MoniKa\\watcher.log for details.",
                    L"MoniKa", MB_OK | MB_ICONERROR);
        CloseHandle(instanceMutex);
        return 1;
    }
    g_app.onArmedChanged = [](int n) { g_tray.set_count(n); };
    g_tray.set_count(g_app.armed());

    g_tray.onRescan = [] { g_app.rescan_running(); };
    g_tray.onExit = [] { g_tray.destroy(); };

    // 1) ETW trigger: catch KakaoTalk starting AFTER us. We started before it, so
    // run the storm-capture burst to snapshot DEKs during its startup open-storm.
    bool etwOk = g_etw.start([](DWORD pid, const wstring& image) {
        if (iends_with(image, KAKAO_IMAGE)) {
            LOGI("ETW: KakaoTalk started pid=%lu (%s)", pid, wide_to_utf8(image).c_str());
            g_app.start_storm(pid);
        }
    });
    if (!etwOk)
        LOGW("ETW start failed (need admin) - relying on watcher/manual rescan");

    // 3) chat_data watcher (tags + new-room DEK capture)
    if (!g_watch.start(g_app.chat_data(), [](const wstring& full, bool added) { g_app.on_file_event(full, added); })) {
        LOGE("chat_data watcher failed to start");
        g_etw.stop();
        g_app.shutdown();
        g_tray.destroy();
        return 1;
    }

    // Catch an already-running instance (watcher started before or after KakaoTalk).
    DWORD running = App::find_kakaotalk_pid();
    if (running) {
        LOGI("KakaoTalk already running pid=%lu -> scanning", running);
        delayed_scan(running, 500);
    }

    LOGI("running. tray active. right-click for Rescan/Exit.");
    MSG msg{};
    int getMessageResult = 0;
    while ((getMessageResult = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (getMessageResult < 0)
        LOGE("GetMessageW failed: %lu", GetLastError());

    LOGI("shutting down");
    g_watch.stop();
    g_etw.stop();
    g_app.shutdown();
    g_tray.destroy();
    CloseHandle(instanceMutex);
    return 0;
}
