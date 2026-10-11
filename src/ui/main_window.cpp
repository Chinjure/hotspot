#include "main_window.h"

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <dwrite.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <thread>

#include "hotkey.h"
#include "settings_window.h"
#include "tray.h"
#include "winutil.h"

namespace {

constexpr int kWidth = 720;
constexpr int kHeight = 480;
constexpr int kRowHeight = 64;
constexpr int kEditId = 100;
constexpr int kHotkeyId = 0xB00F;

constexpr float kBg = 0x1F1F23;
constexpr float kInput = 0x2B2B33;
constexpr float kBorder = 0x3D3D46;
constexpr float kFg = 0xF2F2F4;
constexpr float kSecondary = 0xABABB5;
constexpr float kSelected = 0x34343E;
constexpr float kAccent = 0x9A7BFF;

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

D2D1_COLOR_F colorFrom(float value) {
    float r = ((static_cast<int>(value) >> 16) & 0xFF) / 255.0f;
    float g = ((static_cast<int>(value) >> 8) & 0xFF) / 255.0f;
    float b = (static_cast<int>(value) & 0xFF) / 255.0f;
    return D2D1::ColorF(r, g, b, 1.0f);
}

} // namespace

MainWindow::MainWindow(HINSTANCE instance, SettingsService& settings, HistoryService& history,
                       FileSearchService& fileSearch, NtfsService& ntfs)
    : instance_(instance), settings_(settings), history_(history),
      fileSearch_(fileSearch), ntfs_(ntfs) {}

MainWindow::~MainWindow() {
    destroyed_.store(true);
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        if (activeQueryCancel_) activeQueryCancel_->store(true);
        for (auto& t : queryThreads_) {
            if (t.joinable()) t.join();
        }
        queryThreads_.clear();
    }
    destroyD2D();
    if (editHwnd_ && prevEditProc_) {
        SetWindowLongPtrW(editHwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(prevEditProc_));
    }
    if (hotkeyRegistered_) hotkey::unregisterHotKey(hwnd_, kHotkeyId, true);
}

bool MainWindow::create() {
    winutil::debugLog(L"MainWindow::create start");
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &MainWindow::windowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"HotspotLauncherWindow";
        RegisterClassExW(&wc);
        registered = true;
    }

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"HotspotLauncherWindow", L"hotspot",
                            WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight,
                            nullptr, nullptr, instance_, nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    winutil::debugLog(L"window created");

    // Rounded launcher window (matches the WPF version's corner radius).
    HRGN roundRgn = CreateRoundRectRgn(0, 0, kWidth + 1, kHeight + 1, 24, 24);
    if (roundRgn) SetWindowRgn(hwnd_, roundRgn, TRUE);

    DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    const int hideW = 28, gearW = 32;
    const int buttonRight = kWidth - 8;
    const int hideLeft = buttonRight - hideW;
    const int gearLeft = hideLeft - 4 - gearW;
    const int inputLeft = 12;
    const int inputRight = gearLeft - 6;

    editHwnd_ = CreateWindowExW(0, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NOHIDESEL | WS_TABSTOP,
                                inputLeft + 4, 20, inputRight - inputLeft - 8, 24, hwnd_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)),
                                instance_, nullptr);
    if (editHwnd_) {
        HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SendMessageW(editHwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        prevEditProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            editHwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MainWindow::editProc)));
        SetWindowLongPtrW(editHwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }

    createD2D();
    winutil::debugLog(L"d2d created (rt=" + std::to_wstring(renderTarget_ != nullptr) + L")");
    layout();
    reRegisterHotkey();
    winutil::debugLog(L"MainWindow::create done");
    return true;
}

LRESULT CALLBACK MainWindow::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->handleMessage(msg, wParam, lParam);
}

LRESULT CALLBACK MainWindow::editProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    // Consume only the launcher shortcuts (Esc / Up / Down / Enter). Every other
    // key — Left/Right, Home/End, Ctrl+Left/Right word jumps, Shift selection,
    // Delete/Backspace, clipboard and IME — must reach the native EDIT control,
    // which is the only thing that knows how to move the caret.
    if (self && msg == WM_KEYDOWN && self->onKeyDown(static_cast<UINT>(wParam))) {
        return 0;
    }
    // TranslateMessage() already turned that key into its control character
    // before the WM_KEYDOWN above was dispatched (Esc -> 0x1B, Enter -> 0x0D,
    // Ctrl+Enter -> 0x0A), so the queued WM_CHAR still arrives here -- after Esc
    // has already hidden the window. A single-line EDIT cannot insert such a
    // character and answers it with the system default beep, which is the "ding"
    // heard on Esc. Swallow exactly the characters of the shortcuts we consume;
    // Backspace (0x08), Tab (0x09) and every printable character must keep
    // reaching the EDIT.
    if (self && msg == WM_CHAR) {
        wchar_t ch = static_cast<wchar_t>(wParam);
        if (ch == L'\x1B' || ch == L'\r' || ch == L'\n') return 0;
    }
    return CallWindowProcW(self && self->prevEditProc_ ? self->prevEditProc_ : DefWindowProcW,
                           hwnd, msg, wParam, lParam);
}

void MainWindow::layout() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    int hideW = 28, gearW = 32;
    int buttonRight = w - 8;
    int hideLeft = buttonRight - hideW;
    int gearLeft = hideLeft - 4 - gearW;
    hideRect_ = {hideLeft, 12, buttonRight, 12 + 44};
    gearRect_ = {gearLeft, 12, hideLeft - 4, 12 + 44};
    listRect_ = {12, 68, w - 12, h - 44};
    statusRect_ = {12, h - 34, w - 10, h - 10};

    if (editHwnd_) {
        int inputLeft = 12;
        int inputRight = gearRect_.left - 6;
        MoveWindow(editHwnd_, inputLeft + 4, 20, inputRight - inputLeft - 8, 24, TRUE);
    }
    if (renderTarget_) {
        renderTarget_->Resize(D2D1::SizeU(static_cast<UINT32>(w > 0 ? w : kWidth),
                                          static_cast<UINT32>(h > 0 ? h : kHeight)));
    }
}

LRESULT MainWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            paint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            layout();
            return 0;
        case WM_HOTKEY:
            if (wParam == kHotkeyId) toggleLauncher();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && autoHide_ && visible_ && !settingsOpen_) hide();
            return 0;
        case MainWindow::WM_TRAY:
            if (tray_) tray_->handleCallback(lParam);
            return 0;
        case MainWindow::WM_QUERY_RESULT: {
            // Apply a result only when it belongs to the current query; stale
            // generations must never overwrite the freshest results.
            if (static_cast<uint64_t>(wParam) == queryGeneration_.load()) {
                auto* payload = reinterpret_cast<std::vector<ResultItem>*>(lParam);
                if (payload) {
                    results_ = std::move(*payload);
                    delete payload;
                }
                selectedIndex_ = results_.empty() ? -1 : 0;
                scrollOffset_ = 0;
                wchar_t buf[1024] = {};
                GetWindowTextW(editHwnd_, buf, 1023);
                if (buf[0] == L'\0') {
                    statusText_ = L"Type to search · Alt+Space to toggle";
                } else if (results_.empty()) {
                    statusText_ = L"No results";
                } else if (results_.size() == 1) {
                    statusText_ = L"1 result";
                } else {
                    statusText_ = std::to_wstring(results_.size()) + L" results";
                }
                InvalidateRect(hwnd_, nullptr, TRUE);
            } else {
                delete reinterpret_cast<std::vector<ResultItem>*>(lParam);
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (id == kEditId && code == EN_CHANGE) {
                wchar_t buf[1024] = {};
                GetWindowTextW(editHwnd_, buf, 1023);
                startQuery(buf);
                // The native edit paints no background; ask the parent to redraw
                // the rounded input field behind it.
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            return 0;
        }
        case WM_CTLCOLOREDIT: {
            static bool logged = false;
            if (!logged) {
                logged = true;
                winutil::debugLog(L"WM_CTLCOLOREDIT fired");
            }
            static HBRUSH bg = CreateSolidBrush(RGB(43, 43, 51));
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, RGB(242, 242, 244));
            SetBkColor(hdc, RGB(43, 43, 51));
            SetBkMode(hdc, OPAQUE);
            return reinterpret_cast<LRESULT>(bg);
        }
        case WM_SETFOCUS:
            // Every launcher shortcut (Esc/Enter/arrows) lives in the input
            // field's subclass, so the frame must never keep the focus. This is
            // what makes the keyboard survive a window that hands the frame back
            // (a destroyed settings window activating its owner, a menu, a
            // stray SetFocus): the focus is bounced straight to the search box.
            // GetActiveWindow() keeps another window's focus (the settings
            // window shares this thread's input queue) untouched.
            if (editHwnd_ && GetActiveWindow() == hwnd_) SetFocus(editHwnd_);
            return 0;
        case WM_MOUSEWHEEL:
            onMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        case WM_MOUSEMOVE:
            onMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            onMouseLeftClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_RBUTTONUP:
            onMouseRightClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_CLOSE:
            hide();
            return 0;
        case WM_DESTROY:
            if (hotkeyRegistered_) hotkey::unregisterHotKey(hwnd_, kHotkeyId, true);
            hotkeyRegistered_ = false;
            destroyD2D();
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}

void MainWindow::centerWindow() {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int x = std::max(0, (sw - kWidth) / 2);
    int y = std::max(40, static_cast<int>(sh * 0.12));
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

void MainWindow::showLauncher() {
    centerWindow();
    ShowWindow(hwnd_, SW_SHOW);
    visible_ = true;
    SetForegroundWindow(hwnd_);
    if (editHwnd_) {
        SetFocus(editHwnd_);
        SendMessageW(editHwnd_, EM_SETSEL, 0, -1);
    }
}

void MainWindow::hide() {
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
}

void MainWindow::toggleLauncher() {
    if (visible_ && GetForegroundWindow() == hwnd_) hide();
    else showLauncher();
}

void MainWindow::reRegisterHotkey() {
    if (hwnd_) {
        hotkey::unregisterHotKey(hwnd_, kHotkeyId, hotkeyRegistered_);
        hotkeyRegistered_ = hotkey::registerHotKey(hwnd_, kHotkeyId, settings_.current.hotkey);
        if (!hotkeyRegistered_) {
            statusText_ = L"Global hotkey " + settings_.current.hotkey + L" is unavailable (in use?)";
        }
    }
}

void MainWindow::openSettingsWindow() {
    if (settingsWindow_) {
        SetForegroundWindow(settingsWindow_->hwnd());
        return;
    }
    settingsOpen_ = true;
    settingsWindow_ = new SettingsWindow(instance_, settings_, ntfs_,
                                         [this]() { reRegisterHotkey(); },
                                         [this]() { onSettingsWindowClosed(); });
    settingsWindow_->create(hwnd_);
}

void MainWindow::startQuery(const std::wstring& text) {
    queryGeneration_.fetch_add(1);
    uint64_t gen = queryGeneration_.load();

    std::shared_ptr<std::atomic<bool>> cancel;
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        if (activeQueryCancel_) activeQueryCancel_->store(true);
        activeQueryCancel_ = std::make_shared<std::atomic<bool>>(false);
        cancel = activeQueryCancel_;
    }

    std::thread worker([this, text, gen, cancel]() {
        std::vector<ResultItem> results;
        bool isHistory = winutil::equalsIgnoreCase(text, L"history") ||
                         winutil::equalsIgnoreCase(text, L"hist") ||
                         winutil::startsWithIgnoreCase(text, L"!!");
        if (isHistory) fileSearch_.queryHistory(text, results);
        else fileSearch_.query(text, settings_.current.fileSearchMaxResults, results, cancel.get());

        if (destroyed_) return;
        if (gen == queryGeneration_.load()) {
            auto* payload = new std::vector<ResultItem>(std::move(results));
            PostMessageW(hwnd_, MainWindow::WM_QUERY_RESULT, static_cast<WPARAM>(gen),
                         reinterpret_cast<LPARAM>(payload));
        }
    });
    {
        std::lock_guard<std::mutex> lock(threadsMutex_);
        queryThreads_.push_back(std::move(worker));
    }
}

void MainWindow::executeResult(const ResultItem& item, bool copyOnly) {
    wchar_t buf[1024] = {};
    GetWindowTextW(editHwnd_, buf, 1023);
    history_.add(buf, item.title, item.pluginName, item.path);

    if (copyOnly || item.copyToClipboard) {
        copyPath(item);
    }
    if (!copyOnly && !item.path.empty()) {
        openDirectly(item);
    }
    if (item.closeAfterExecute) hide();
    else if (editHwnd_) SendMessageW(editHwnd_, EM_SETSEL, 0, -1);
}

void MainWindow::openDirectly(const ResultItem& item) {
    if (item.path.empty()) return;
    if (item.isDirectory) {
        ShellExecuteW(hwnd_, L"open", item.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(hwnd_, L"open", item.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void MainWindow::openContainingPath(const ResultItem& item) {
    if (item.path.empty()) return;
    if (item.isDirectory) {
        ShellExecuteW(hwnd_, L"open", item.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        std::wstring args = L"/select,\"" + item.path + L"\"";
        ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

void MainWindow::copyPath(const ResultItem& item) {
    if (item.clipboardText.empty()) return;
    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        size_t bytes = (item.clipboardText.size() + 1) * sizeof(wchar_t);
        HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hmem) {
            void* p = GlobalLock(hmem);
            if (p) {
                memcpy(p, item.clipboardText.c_str(), bytes);
                GlobalUnlock(hmem);
                SetClipboardData(CF_UNICODETEXT, hmem);
            }
        }
        CloseClipboard();
        statusText_ = L"完整路径已复制到剪贴板";
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

bool MainWindow::onKeyDown(UINT vk) {
    switch (vk) {
        case VK_ESCAPE:
            hide();
            return true;
        case VK_DOWN:
            moveSelection(1);
            return true;
        case VK_UP:
            moveSelection(-1);
            return true;
        case VK_RETURN:
            if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(results_.size())) {
                executeResult(results_[selectedIndex_]);
            }
            return true;
        default:
            // Not a launcher shortcut: let the edit control handle it (caret
            // movement, selection, editing, IME composition).
            return false;
    }
}

void MainWindow::moveSelection(int delta) {
    if (results_.empty()) return;
    int index = selectedIndex_ < 0 ? 0 : selectedIndex_ + delta;
    index = std::clamp(index, 0, static_cast<int>(results_.size()) - 1);
    selectedIndex_ = index;
    ensureSelectedVisible();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void MainWindow::ensureSelectedVisible() {
    int visibleCount = std::max(1, static_cast<int>(listRect_.bottom - listRect_.top) / kRowHeight);
    if (selectedIndex_ < scrollOffset_) scrollOffset_ = selectedIndex_;
    if (selectedIndex_ >= scrollOffset_ + visibleCount) {
        scrollOffset_ = selectedIndex_ - visibleCount + 1;
    }
    scrollOffset_ = std::clamp(scrollOffset_, 0, std::max(0, static_cast<int>(results_.size()) - 1));
}

void MainWindow::onMouseLeftClick(int x, int y) {
    if (pointIn(gearRect_, x, y)) {
        openSettingsWindow();
        return;
    }
    if (pointIn(hideRect_, x, y)) {
        hide();
        return;
    }
    int row = hitTestRow(y);
    if (row >= 0 && row < static_cast<int>(results_.size())) {
        selectedIndex_ = row;
        executeResult(results_[row]);
    }
}

void MainWindow::onMouseRightClick(int x, int y) {
    (void)x;
    int row = hitTestRow(y);
    if (row < 0 || row >= static_cast<int>(results_.size())) return;
    selectedIndex_ = row;
    InvalidateRect(hwnd_, nullptr, TRUE);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1004, L"打开文件");
    AppendMenuW(menu, MF_STRING, 1005, L"打开所在路径");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1006, L"复制完整路径");

    POINT pt{};
    GetCursorPos(&pt);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);

    if (cmd == 1004) openDirectly(results_[row]);
    else if (cmd == 1005) openContainingPath(results_[row]);
    else if (cmd == 1006) copyPath(results_[row]);
}

void MainWindow::onMouseMove(int x, int y) {
    int row = pointIn(listRect_, x, y) ? hitTestRow(y) : -1;
    if (row != hoverIndex_) {
        hoverIndex_ = row;
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    bool g = pointIn(gearRect_, x, y);
    bool h = pointIn(hideRect_, x, y);
    if (g != gearHover_ || h != hideHover_) {
        gearHover_ = g;
        hideHover_ = h;
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void MainWindow::onMouseWheel(int delta) {
    if (results_.empty()) return;
    int visibleCount = std::max(1, static_cast<int>(listRect_.bottom - listRect_.top) / kRowHeight);
    int maxScroll = std::max(0, static_cast<int>(results_.size()) - visibleCount);
    scrollOffset_ = std::clamp(scrollOffset_ - (delta / WHEEL_DELTA) * 3, 0, maxScroll);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

int MainWindow::hitTestRow(int y) const {
    if (y < listRect_.top || y >= listRect_.bottom) return -1;
    int index = scrollOffset_ + (y - listRect_.top) / kRowHeight;
    return index < static_cast<int>(results_.size()) ? index : -1;
}

bool MainWindow::pointIn(const RECT& r, int x, int y) const {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool MainWindow::createD2D() {
    if (d2dFactory_) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dwriteFactory_)))) {
        return false;
    }
    HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(kWidth, kHeight)),
        &renderTarget_);
    if (FAILED(hr)) return false;

    renderTarget_->CreateSolidColorBrush(colorFrom(kBg), &bgBrush_);
    renderTarget_->CreateSolidColorBrush(colorFrom(kInput), &inputBrush_);
    renderTarget_->CreateSolidColorBrush(colorFrom(kBorder), &borderBrush_);
    renderTarget_->CreateSolidColorBrush(colorFrom(kFg), &fgBrush_);
    renderTarget_->CreateSolidColorBrush(colorFrom(kSecondary), &secondaryBrush_);
    renderTarget_->CreateSolidColorBrush(colorFrom(kSelected), &selectedBrush_);
    renderTarget_->CreateSolidColorBrush(colorFrom(kAccent), &accentBrush_);

    // Real shell icons (exe/lnk/folder/file type) for result rows.
    iconCache_ = std::make_unique<IconCache>(renderTarget_);

    auto makeFormat = [&](const wchar_t* family, float size, IDWriteTextFormat** out) {
        dwriteFactory_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         size, L"zh-cn", out);
    };
    makeFormat(L"Segoe UI", 16.0f, &titleFormat_);
    makeFormat(L"Segoe UI", 12.0f, &subtitleFormat_);
    makeFormat(L"Segoe UI Emoji", 24.0f, &iconFormat_);
    makeFormat(L"Segoe UI", 12.0f, &statusFormat_);
    if (statusFormat_) statusFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    makeFormat(L"Segoe UI", 12.0f, &pluginFormat_);
    if (pluginFormat_) {
        pluginFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        pluginFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    makeFormat(L"Segoe UI", 12.0f, &statusRightFormat_);
    if (statusRightFormat_) {
        statusRightFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        statusRightFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    makeFormat(L"Segoe UI", 24.0f, &buttonFormat_);
    if (buttonFormat_) {
        buttonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        buttonFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    return true;
}

void MainWindow::destroyD2D() {
    // Icons are device-dependent bitmaps: drop them before the target dies.
    iconCache_.reset();
    if (titleFormat_) titleFormat_->Release();
    if (subtitleFormat_) subtitleFormat_->Release();
    if (iconFormat_) iconFormat_->Release();
    if (statusFormat_) statusFormat_->Release();
    if (pluginFormat_) pluginFormat_->Release();
    if (statusRightFormat_) statusRightFormat_->Release();
    if (buttonFormat_) buttonFormat_->Release();
    if (bgBrush_) bgBrush_->Release();
    if (inputBrush_) inputBrush_->Release();
    if (borderBrush_) borderBrush_->Release();
    if (fgBrush_) fgBrush_->Release();
    if (secondaryBrush_) secondaryBrush_->Release();
    if (selectedBrush_) selectedBrush_->Release();
    if (accentBrush_) accentBrush_->Release();
    if (renderTarget_) renderTarget_->Release();
    if (dwriteFactory_) dwriteFactory_->Release();
    if (d2dFactory_) d2dFactory_->Release();
    titleFormat_ = nullptr;
    subtitleFormat_ = nullptr;
    iconFormat_ = nullptr;
    statusFormat_ = nullptr;
    pluginFormat_ = nullptr;
    statusRightFormat_ = nullptr;
    buttonFormat_ = nullptr;
    bgBrush_ = nullptr;
    inputBrush_ = nullptr;
    borderBrush_ = nullptr;
    fgBrush_ = nullptr;
    secondaryBrush_ = nullptr;
    selectedBrush_ = nullptr;
    accentBrush_ = nullptr;
    renderTarget_ = nullptr;
    dwriteFactory_ = nullptr;
    d2dFactory_ = nullptr;
}

void MainWindow::paint() {
    if (!renderTarget_) return;
    renderTarget_->BeginDraw();
    renderTarget_->Clear(D2D1::ColorF(0x1F1F23, 1.0f));

    D2D1_RECT_F full = D2D1::RectF(0.5f, 0.5f, kWidth - 0.5f, kHeight - 0.5f);
    renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(full, 12.0f, 12.0f), borderBrush_, 1.0f);

    // Rounded search-input field (drawn by us; the flat EDIT control is
    // transparent so this shows through).
    D2D1_RECT_F inputRect = D2D1::RectF(12.0f, 12.0f, static_cast<float>(gearRect_.left - 6), 56.0f);
    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(inputRect, 8.0f, 8.0f), inputBrush_);
    renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(inputRect, 8.0f, 8.0f), borderBrush_, 1.0f);

    // Gear and hide buttons (hover-highlighted, right-aligned glyphs).
    auto drawButton = [&](const RECT& r, const wchar_t* text, bool hover) {
        if (hover) {
            D2D1_RECT_F row = D2D1::RectF(static_cast<float>(r.left),
                                          static_cast<float>(r.top),
                                          static_cast<float>(r.right),
                                          static_cast<float>(r.bottom));
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(row, 8.0f, 8.0f), selectedBrush_);
        }
        D2D1_RECT_F rect = D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                                       static_cast<float>(r.right), static_cast<float>(r.bottom));
        renderTarget_->DrawTextW(text, static_cast<UINT32>(wcslen(text)), buttonFormat_, rect,
                                 hover ? fgBrush_ : secondaryBrush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    drawButton(gearRect_, L"⚙", gearHover_);
    drawButton(hideRect_, L"✕", hideHover_);

    // Results.
    int visibleCount = std::max(0, static_cast<int>(listRect_.bottom - listRect_.top) / kRowHeight);
    int end = std::min(static_cast<int>(results_.size()), scrollOffset_ + visibleCount);
    for (int i = scrollOffset_; i < end; ++i) {
        int rowY = listRect_.top + (i - scrollOffset_) * kRowHeight;
        const auto& item = results_[static_cast<size_t>(i)];

        if (i == selectedIndex_ || i == hoverIndex_) {
            D2D1_RECT_F row = D2D1::RectF(static_cast<float>(listRect_.left),
                                          static_cast<float>(rowY),
                                          static_cast<float>(listRect_.right),
                                          static_cast<float>(rowY + kRowHeight - 4));
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(row, 8.0f, 8.0f), selectedBrush_);
        }

        // Icon column: real shell icon when the row maps to a file/folder,
        // otherwise the emoji fallback glyph.
        ID2D1Bitmap* rowIcon = iconCache_ && !item.iconPath.empty()
                                   ? iconCache_->get(item.iconPath)
                                   : nullptr;
        if (rowIcon) {
            const float iconSize = 28.0f;
            float iconLeft = 20.0f + (44.0f - iconSize) / 2.0f;
            float iconTop = static_cast<float>(rowY) + (static_cast<float>(kRowHeight) - iconSize) / 2.0f - 2.0f;
            renderTarget_->DrawBitmap(rowIcon,
                                      D2D1::RectF(iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize),
                                      1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            renderTarget_->DrawTextW(item.icon.c_str(), static_cast<UINT32>(item.icon.size()),
                                     iconFormat_,
                                     D2D1::RectF(20.0f, static_cast<float>(rowY + 14),
                                                 64.0f, static_cast<float>(rowY + 46)),
                                     secondaryBrush_);
        }

        renderTarget_->DrawTextW(item.title.c_str(), static_cast<UINT32>(item.title.size()),
                                 titleFormat_,
                                 D2D1::RectF(68.0f, static_cast<float>(rowY + 8),
                                             static_cast<float>(listRect_.right - 170),
                                             static_cast<float>(rowY + 32)),
                                 fgBrush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        renderTarget_->DrawTextW(item.subtitle.c_str(), static_cast<UINT32>(item.subtitle.size()),
                                 subtitleFormat_,
                                 D2D1::RectF(68.0f, static_cast<float>(rowY + 32),
                                             static_cast<float>(listRect_.right - 170),
                                             static_cast<float>(rowY + 58)),
                                 secondaryBrush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        renderTarget_->DrawTextW(item.pluginName.c_str(), static_cast<UINT32>(item.pluginName.size()),
                                 pluginFormat_,
                                 D2D1::RectF(static_cast<float>(listRect_.right - 170),
                                             static_cast<float>(rowY + 22),
                                             static_cast<float>(listRect_.right - 8),
                                             static_cast<float>(rowY + 42)),
                                 secondaryBrush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // Status bar.
    renderTarget_->DrawTextW(statusText_.c_str(), static_cast<UINT32>(statusText_.size()),
                             statusFormat_,
                             D2D1::RectF(static_cast<float>(statusRect_.left),
                                         static_cast<float>(statusRect_.top),
                                         static_cast<float>(statusRect_.right - 260),
                                         static_cast<float>(statusRect_.bottom)),
                             secondaryBrush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    const wchar_t* hint = L"Enter 执行 · Esc 隐藏 · ↑↓ 选择";
    renderTarget_->DrawTextW(hint, static_cast<UINT32>(wcslen(hint)), statusRightFormat_,
                             D2D1::RectF(static_cast<float>(statusRect_.right - 260),
                                         static_cast<float>(statusRect_.top),
                                         static_cast<float>(statusRect_.right - 8),
                                         static_cast<float>(statusRect_.bottom)),
                             secondaryBrush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);

    renderTarget_->EndDraw();
}
