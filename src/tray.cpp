// tray.cpp
#include "tray.hpp"
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace kw {

static const UINT WM_TRAY = WM_APP + 1;
static const UINT WM_SETCNT = WM_APP + 2;
static const UINT IDM_RESCAN = 1001;
static const UINT IDM_EXIT = 1002;
static const UINT IDM_JUST_MONIKA = 1003;
static const wchar_t* CLS = L"MoniKaTrayWnd";
static const wchar_t JUST_MONIKA[] = L"Just MoniKa";

static HFONT create_italic_menu_font() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        return nullptr;
    metrics.lfMenuFont.lfItalic = 1;
    return CreateFontIndirectW(&metrics.lfMenuFont);
}

static bool measure_just_monika(MEASUREITEMSTRUCT* measure) {
    if (measure->CtlType != ODT_MENU || measure->itemID != IDM_JUST_MONIKA)
        return false;
    auto* text = reinterpret_cast<const wchar_t*>(measure->itemData);
    if (!text)
        return false;

    HDC dc = GetDC(nullptr);
    HFONT font = create_italic_menu_font();
    HGDIOBJ oldFont = dc && font ? SelectObject(dc, font) : nullptr;
    SIZE textSize{};
    if (dc)
        GetTextExtentPoint32W(dc, text, lstrlenW(text), &textSize);

    int menuHeight = GetSystemMetrics(SM_CYMENU);
    int textHeight = textSize.cy + 6;
    measure->itemWidth = static_cast<UINT>(textSize.cx + GetSystemMetrics(SM_CXMENUCHECK) + 12);
    measure->itemHeight = static_cast<UINT>(textHeight > menuHeight ? textHeight : menuHeight);

    if (oldFont && oldFont != HGDI_ERROR)
        SelectObject(dc, oldFont);
    if (font)
        DeleteObject(font);
    if (dc)
        ReleaseDC(nullptr, dc);
    return true;
}

static bool draw_just_monika(const DRAWITEMSTRUCT* draw) {
    if (draw->CtlType != ODT_MENU || draw->itemID != IDM_JUST_MONIKA)
        return false;
    auto* text = reinterpret_cast<const wchar_t*>(draw->itemData);
    if (!text)
        return false;
    if ((draw->itemAction & (ODA_DRAWENTIRE | ODA_SELECT | ODA_FOCUS)) == 0)
        return true;

    bool selected = (draw->itemState & ODS_SELECTED) != 0;
    bool disabled = (draw->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    int background = selected ? COLOR_HIGHLIGHT : COLOR_MENU;
    int foreground = disabled ? COLOR_GRAYTEXT : selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT;
    FillRect(draw->hDC, &draw->rcItem, GetSysColorBrush(background));

    HFONT font = create_italic_menu_font();
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    int oldBackgroundMode = SetBkMode(draw->hDC, TRANSPARENT);
    COLORREF oldTextColor = SetTextColor(draw->hDC, GetSysColor(foreground));
    RECT textRect = draw->rcItem;
    textRect.left += GetSystemMetrics(SM_CXMENUCHECK) + 6;
    textRect.right -= 6;
    DrawTextW(draw->hDC, text, -1, &textRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SetTextColor(draw->hDC, oldTextColor);
    SetBkMode(draw->hDC, oldBackgroundMode);
    if (oldFont && oldFont != HGDI_ERROR)
        SelectObject(draw->hDC, oldFont);
    if (font)
        DeleteObject(font);
    return true;
}

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
    hwnd_ = CreateWindowExW(0, CLS, L"MoniKa", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, this);
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
    wcscpy_s(nid_.szTip, L"MoniKa: starting...");
    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
        LOGE("tray icon add failed %lu", GetLastError());
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    nid_.uFlags = NIF_INFO;
    wcscpy_s(nid_.szInfoTitle, L"MoniKa is running");
    wcscpy_s(nid_.szInfo, L"Monitoring KakaoTalk database changes. Right-click the tray icon for options.");
    nid_.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    return true;
}

void Tray::update_tip_(int armed) {
    swprintf_s(nid_.szTip, L"MoniKa: %d DEKs armed", armed);
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
    case WM_MEASUREITEM:
        if (measure_just_monika(reinterpret_cast<MEASUREITEMSTRUCT*>(l)))
            return TRUE;
        break;
    case WM_DRAWITEM:
        if (draw_just_monika(reinterpret_cast<DRAWITEMSTRUCT*>(l)))
            return TRUE;
        break;
    case WM_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) {
            bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            POINT pt;
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            wchar_t item[64];
            swprintf_s(item, L"Armed DEKs: %d", lastCount_ < 0 ? 0 : lastCount_);
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, item);
            if (shiftHeld)
                AppendMenuW(menu, MF_OWNERDRAW | MF_GRAYED, IDM_JUST_MONIKA, JUST_MONIKA);
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_RESCAN, L"Rescan now");
            AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
            SetForegroundWindow(h);
            UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, h, nullptr);
            PostMessageW(h, WM_NULL, 0, 0);
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
