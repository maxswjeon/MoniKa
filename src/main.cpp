// main.cpp - wire ETW + scanner + watcher + cache + tray together.
#include "common.hpp"
#include "app.hpp"
#include "etw.hpp"
#include "watcher.hpp"
#include "tray.hpp"

using namespace kw;

static App g_app;
static EtwProcessMonitor g_etw;
static DirWatcher g_watch;
static Tray g_tray;

// Scan KakaoTalk a few seconds after start so its DBs (and DEKs) are loaded.
static void delayed_scan(DWORD pid, int delayMs) {
    g_app.request_scan(pid, delayMs);
}

int main() {
    // Paths are logged as UTF-8; make Korean and other non-ASCII names readable
    // when attached to a Windows console. Redirected output remains UTF-8 bytes.
    SetConsoleOutputCP(CP_UTF8);
    LOGI("kakao_watcher starting (self-use: your own account/PC)");
    if (!g_app.init()) {
        LOGE("init failed");
        return 1;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!g_tray.create(hInst)) {
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
    return 0;
}
