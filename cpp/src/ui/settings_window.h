#pragma once

#include <windows.h>

#include <functional>
#include <string>

#include "ntfs/ntfs_service.h"
#include "settings.h"

class SettingsWindow {
public:
    SettingsWindow(HINSTANCE instance, SettingsService& settings, NtfsService& ntfs,
                   std::function<void()> onSaved, std::function<void()> onClosed);
    ~SettingsWindow();

    bool create(HWND owner);
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void createControls();
    void loadSettings();
    void saveSettings();
    void refreshStatus();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    SettingsService& settings_;
    NtfsService& ntfs_;
    std::function<void()> onSaved_;
    std::function<void()> onClosed_;

    HWND hotkeyEdit_ = nullptr;
    HWND autoStartCheck_ = nullptr;
    HWND showOnStartupCheck_ = nullptr;
    HWND searchCombo_ = nullptr;
    HWND ntfsIndexCheck_ = nullptr;
    HWND liveSearchCheck_ = nullptr;
    HWND plainTextCheck_ = nullptr;
    HWND timeoutEdit_ = nullptr;
    HWND statusText_ = nullptr;
    HWND rebuildButton_ = nullptr;
    HWND fileSearchPluginCheck_ = nullptr;
    HWND historyPluginCheck_ = nullptr;
};
