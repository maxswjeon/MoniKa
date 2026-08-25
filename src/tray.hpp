// tray.hpp - system tray icon; tooltip shows armed-DEK count; menu Rescan/Exit.
#pragma once
#include "common.hpp"
#include <shellapi.h>
#include <functional>

namespace kw {

class Tray {
public:
    std::function<void()> onRescan;
    std::function<void()> onExit;

    bool create(HINSTANCE hInst);
    void set_count(int armed);      // thread-safe (posts to UI thread)
    void destroy();

    // internal
    LRESULT wndproc(HWND, UINT, WPARAM, LPARAM);
private:
    void update_tip_(int armed);
    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW nid_{};
    int lastCount_ = -1;
};

} // namespace kw
