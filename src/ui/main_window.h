#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "file_search.h"
#include "history.h"
#include "icon_cache.h"
#include "ntfs/ntfs_service.h"
#include "settings.h"

class SettingsWindow;
class TrayIcon;

class MainWindow {
public:
    MainWindow(HINSTANCE instance, SettingsService& settings, HistoryService& history,
               FileSearchService& fileSearch, NtfsService& ntfs);
    ~MainWindow();

    bool create();
    HWND hwnd() const { return hwnd_; }

    void showLauncher();
    void hide();
    void toggleLauncher();
    void setAutoHide(bool value) { autoHide_ = value; }
    void reRegisterHotkey();
    void openSettingsWindow();
    void setTray(TrayIcon* tray) { tray_ = tray; }
    void onSettingsWindowClosed() { settingsWindow_ = nullptr; settingsOpen_ = false; }

    static constexpr UINT WM_TRAY = WM_APP + 1;
    static constexpr UINT WM_QUERY_RESULT = WM_APP + 2;

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK editProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool createD2D();
    void destroyD2D();
    void paint();
    void layout();
    void centerWindow();
    void updateResults(const std::vector<ResultItem>& results, const std::wstring& status);
    void startQuery(const std::wstring& text);
    void executeResult(const ResultItem& item, bool copyOnly = false);
    void openDirectly(const ResultItem& item);
    void openContainingPath(const ResultItem& item);
    void copyPath(const ResultItem& item);

    // Returns true when the launcher consumed the key. Unconsumed keys must be
    // forwarded to the native EDIT control, otherwise caret editing dies.
    bool onKeyDown(UINT vk);
    void moveSelection(int delta);
    void ensureSelectedVisible();
    void onMouseLeftClick(int x, int y);
    void onMouseRightClick(int x, int y);
    void onMouseMove(int x, int y);
    void onMouseWheel(int delta);
    int hitTestRow(int y) const;
    bool pointIn(const RECT& r, int x, int y) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND editHwnd_ = nullptr;
    WNDPROC prevEditProc_ = nullptr;

    SettingsService& settings_;
    HistoryService& history_;
    FileSearchService& fileSearch_;
    NtfsService& ntfs_;

    bool autoHide_ = true;
    bool hotkeyRegistered_ = false;
    bool settingsOpen_ = false;
    bool visible_ = false;
    SettingsWindow* settingsWindow_ = nullptr;
    TrayIcon* tray_ = nullptr;

    std::vector<ResultItem> results_;
    int selectedIndex_ = -1;
    int hoverIndex_ = -1;
    int scrollOffset_ = 0;
    bool gearHover_ = false;
    bool hideHover_ = false;

    std::atomic<uint64_t> queryGeneration_{0};
    std::atomic<bool> destroyed_{false};
    std::mutex threadsMutex_;
    std::vector<std::thread> queryThreads_;
    // Cancel flag for the in-flight query; a new query cancels the previous
    // one so stale searches stop scanning as soon as the text changes.
    std::shared_ptr<std::atomic<bool>> activeQueryCancel_;

    RECT gearRect_{};
    RECT hideRect_{};
    RECT listRect_{};
    RECT statusRect_{};

    ID2D1Factory* d2dFactory_ = nullptr;
    ID2D1HwndRenderTarget* renderTarget_ = nullptr;
    IDWriteFactory* dwriteFactory_ = nullptr;
    std::unique_ptr<IconCache> iconCache_;
    IDWriteTextFormat* titleFormat_ = nullptr;
    IDWriteTextFormat* subtitleFormat_ = nullptr;
    IDWriteTextFormat* iconFormat_ = nullptr;
    IDWriteTextFormat* statusFormat_ = nullptr;
    IDWriteTextFormat* pluginFormat_ = nullptr;
    IDWriteTextFormat* statusRightFormat_ = nullptr;
    IDWriteTextFormat* buttonFormat_ = nullptr;

    ID2D1SolidColorBrush* bgBrush_ = nullptr;
    ID2D1SolidColorBrush* inputBrush_ = nullptr;
    ID2D1SolidColorBrush* borderBrush_ = nullptr;
    ID2D1SolidColorBrush* fgBrush_ = nullptr;
    ID2D1SolidColorBrush* secondaryBrush_ = nullptr;
    ID2D1SolidColorBrush* selectedBrush_ = nullptr;
    ID2D1SolidColorBrush* accentBrush_ = nullptr;

    std::wstring statusText_ = L"Type to search · Alt+Space to toggle";
};
