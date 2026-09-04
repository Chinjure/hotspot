#pragma once

#include <windows.h>
#include <shellapi.h>

#include <functional>

class TrayIcon {
public:
    using Action = std::function<void()>;

    TrayIcon(HWND host, HINSTANCE instance, Action showLauncher, Action showSettings, Action exitApp);
    ~TrayIcon();

    bool add();
    void remove();
    void handleCallback(LPARAM lParam);

private:
    void showMenu();

    HWND host_ = nullptr;
    HINSTANCE instance_ = nullptr;
    UINT callbackMessage_ = WM_APP + 1;
    NOTIFYICONDATAW nid_{};
    Action showLauncher_;
    Action showSettings_;
    Action exitApp_;
};
