// tray.cpp
#include "tray.hpp"
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace kw {

static const UINT WM_TRAY = WM_APP + 1;
static const UINT WM_SETCNT = WM_APP + 2;
static const UINT IDM_RESCAN = 1001;
static const UINT IDM_EXIT = 1002;
static const wchar_t* CLS = L"KakaoWatcherTrayWnd";

static LRESULT CALLBACK wnd_thunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    Tray* self = (Tray*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)l;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }
    if (self)
        return self->wndproc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

bool Tray::create(HINSTANCE hInst) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_thunk;
    wc.hInstance = hInst;
    wc.lpszClassName = CLS;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LOGE("tray class registration failed %lu", GetLastError());
        return false;
    }
    hwnd_ = CreateWindowExW(0, CLS, L"kakao_watcher", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst,
                            this);
    if (!hwnd_) {
        LOGE("tray window create failed %lu", GetLastError());
        return false;
    }

    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAY;
    nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid_.szTip, L"kakao_watcher: starting...");
    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
        LOGE("tray icon add failed %lu", GetLastError());
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    return true;
}

void Tray::update_tip_(int armed) {
    swprintf_s(nid_.szTip, L"kakao_watcher: %d DEKs armed", armed);
    nid_.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    lastCount_ = armed;
}

void Tray::set_count(int armed) {
    if (hwnd_)
        PostMessageW(hwnd_, WM_SETCNT, (WPARAM)armed, 0);
}

LRESULT Tray::wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SETCNT:
        update_tip_((int)w);
        return 0;
    case WM_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            wchar_t item[64];
            swprintf_s(item, L"Armed DEKs: %d", lastCount_ < 0 ? 0 : lastCount_);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, item);
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_RESCAN, L"Rescan now");
            AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
            SetForegroundWindow(h);
            UINT cmd =
                TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (cmd == IDM_RESCAN && onRescan)
                onRescan();
            else if (cmd == IDM_EXIT && onExit)
                onExit();
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void Tray::destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

} // namespace kw
