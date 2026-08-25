// main.cpp - wire ETW + scanner + watcher + cache + tray together.
#include "common.hpp"
#include "app.hpp"
#include "etw.hpp"
#include "watcher.hpp"
#include "tray.hpp"
#include <thread>
#include <chrono>

using namespace kw;

static App          g_app;
static EtwProcessMonitor g_etw;
static DirWatcher   g_watch;
static Tray         g_tray;

// Scan KakaoTalk a few seconds after start so its DBs (and DEKs) are loaded.
static void delayed_scan(DWORD pid, int delayMs){
    std::thread([pid, delayMs]{
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        g_app.scan_pid(pid);
    }).detach();
}

int main(){
    LOGI("kakao_watcher starting (self-use: your own account/PC)");
    if(!g_app.init()){ LOGE("init failed"); return 1; }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if(!g_tray.create(hInst)){ return 1; }
    g_app.onArmedChanged = [](int n){ g_tray.set_count(n); };
    g_tray.set_count(g_app.armed());

    g_tray.onRescan = []{ std::thread([]{ g_app.rescan_running(); }).detach(); };
    g_tray.onExit   = []{ PostQuitMessage(0); };

    // 1) ETW trigger: catch KakaoTalk starting AFTER us.
    bool etwOk = g_etw.start([](DWORD pid, const wstring& image){
        if(iends_with(image, KAKAO_IMAGE)){
            LOGI("ETW: KakaoTalk started pid=%lu (%s)", pid, wide_to_utf8(image).c_str());
            delayed_scan(pid, 5000);      // let it open its DBs first
            delayed_scan(pid, 15000);     // second pass for late-opened rooms
        }
    });
    if(!etwOk) LOGW("ETW start failed (need admin) - relying on watcher/manual rescan");

    // 3) chat_data watcher (tags + new-room DEK capture)
    g_watch.start(g_app.chat_data(), [](const wstring& full, bool added){
        g_app.on_file_event(full, added);
    });

    // Catch an already-running instance (watcher started before or after KakaoTalk).
    DWORD running = App::find_kakaotalk_pid();
    if(running){ LOGI("KakaoTalk already running pid=%lu -> scanning", running); delayed_scan(running, 500); }

    LOGI("running. tray active. right-click for Rescan/Exit.");
    MSG msg;
    while(GetMessageW(&msg, nullptr, 0, 0)){
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LOGI("shutting down");
    g_watch.stop();
    g_etw.stop();
    g_tray.destroy();
    return 0;
}
