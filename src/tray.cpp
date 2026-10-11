#include "tray.h"

#include <cwchar>

#include "resource.h"
#include "winutil.h"

TrayIcon::TrayIcon(HWND host, HINSTANCE instance, Action showLauncher, Action showSettings, Action exitApp)
    : host_(host), instance_(instance), showLauncher_(std::move(showLauncher)),
      showSettings_(std::move(showSettings)), exitApp_(std::move(exitApp)) {}

TrayIcon::~TrayIcon() { remove(); }

bool TrayIcon::add() {
    nid_ = {};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = host_;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uCallbackMessage = callbackMessage_;
    nid_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
    wcscpy_s(nid_.szTip, L"hotspot — 左键显示 · 右键菜单");
    return Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
}

void TrayIcon::remove() {
    if (nid_.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        nid_.hWnd = nullptr;
    }
}

void TrayIcon::handleCallback(LPARAM lParam) {
    switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            showLauncher_();
            break;
        case WM_RBUTTONUP:
            showMenu();
            break;
    }
}

void TrayIcon::showMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1001, L"显示启动器");
    AppendMenuW(menu, MF_STRING, 1002, L"设置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1003, L"退出");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(host_);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, host_, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
        case 1001: showLauncher_(); break;
        case 1002: showSettings_(); break;
        case 1003: exitApp_(); break;
    }
}
