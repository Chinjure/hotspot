#include "settings_window.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <iterator>

#include "hotkey.h"
#include "winutil.h"

namespace {
constexpr int ID_HOTKEY = 200;
constexpr int ID_AUTOSTART = 201;
constexpr int ID_SHOWSTARTUP = 202;
constexpr int ID_SEARCHCOMBO = 203;
constexpr int ID_NTFS = 204;
constexpr int ID_LIVE = 205;
constexpr int ID_PLAIN = 206;
constexpr int ID_TIMEOUT = 207;
constexpr int ID_REBUILD = 208;
constexpr int ID_SAVE = 209;
constexpr int ID_CANCEL = 210;
constexpr int ID_FILESEARCH_PLUGIN = 211;
constexpr int ID_HISTORY_PLUGIN = 212;

const wchar_t* kSearchEngines[] = {
    L"Bing",
    L"Google",
    L"DuckDuckGo",
};
const wchar_t* kSearchUrls[] = {
    L"https://www.bing.com/search?q={0}",
    L"https://www.google.com/search?q={0}",
    L"https://duckduckgo.com/?q={0}",
};
} // namespace

SettingsWindow::SettingsWindow(HINSTANCE instance, SettingsService& settings, NtfsService& ntfs,
                               std::function<void()> onSaved, std::function<void()> onClosed)
    : instance_(instance), settings_(settings), ntfs_(ntfs),
      onSaved_(std::move(onSaved)), onClosed_(std::move(onClosed)) {}

SettingsWindow::~SettingsWindow() {}

bool SettingsWindow::create(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &SettingsWindow::windowProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(COLOR_WINDOW));
        wc.lpszClassName = L"HotspotSettingsWindow";
        RegisterClassExW(&wc);
        registered = true;
    }

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 560, h = 640;
    int x = sw > w ? (sw - w) / 2 : 0;
    int y = sh > h ? (sh - h) / 2 : 0;

    hwnd_ = CreateWindowExW(0, L"HotspotSettingsWindow", L"hotspot — Settings",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                            x, y, w, h, owner, nullptr, instance_, nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    createControls();
    loadSettings();
    refreshStatus();
    ShowWindow(hwnd_, SW_SHOW);
    return true;
}

LRESULT CALLBACK SettingsWindow::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsWindow* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->handleMessage(msg, wParam, lParam);
}

void SettingsWindow::createControls() {
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    auto create = [&](const wchar_t* cls, const wchar_t* text, int id, int x, int y, int w, int h,
                      DWORD style, DWORD ex = 0) -> HWND {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 x, y, w, h, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };

    create(L"STATIC", L"Global hotkey", 0, 16, 12, 300, 20, 0);
    hotkeyEdit_ = create(L"EDIT", nullptr, ID_HOTKEY, 16, 34, 300, 26, WS_BORDER | ES_AUTOHSCROLL);
    autoStartCheck_ = create(L"BUTTON", L"Start with Windows", ID_AUTOSTART, 16, 70, 300, 24,
                             BS_AUTOCHECKBOX);
    showOnStartupCheck_ = create(L"BUTTON", L"Show the launcher window when the app starts",
                                 ID_SHOWSTARTUP, 16, 98, 320, 24, BS_AUTOCHECKBOX);

    create(L"STATIC", L"Web search engine", 0, 16, 134, 300, 20, 0);
    searchCombo_ = create(L"COMBOBOX", nullptr, ID_SEARCHCOMBO, 16, 156, 220, 300,
                          CBS_DROPDOWNLIST | WS_VSCROLL);
    for (auto* name : kSearchEngines) {
        SendMessageW(searchCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    }

    create(L"STATIC", L"NTFS 文件索引 ($MFT)", 0, 16, 200, 320, 20, 0);
    ntfsIndexCheck_ = create(L"BUTTON", L"启用全盘文件搜索 (fs / ?) — 直接重建 NTFS MFT 记录",
                             ID_NTFS, 16, 224, 400, 24, BS_AUTOCHECKBOX);
    liveSearchCheck_ = create(L"BUTTON", L"索引外实时搜索（新建/未入索引文件，限时扫描）",
                              ID_LIVE, 16, 250, 420, 24, BS_AUTOCHECKBOX);
    plainTextCheck_ = create(L"BUTTON", L"无前缀直接搜索全盘文件（类似 Everything，无需 fs / ?）",
                             ID_PLAIN, 16, 276, 420, 24, BS_AUTOCHECKBOX);
    create(L"STATIC", L"实时搜索时限", 0, 16, 306, 100, 20, 0);
    timeoutEdit_ = create(L"EDIT", nullptr, ID_TIMEOUT, 120, 302, 60, 24, WS_BORDER | ES_NUMBER);
    create(L"STATIC", L"ms (300–5000)", 0, 188, 306, 140, 20, 0);

    fileSearchPluginCheck_ = create(L"BUTTON", L"启用 File Search 插件", ID_FILESEARCH_PLUGIN,
                                    16, 338, 300, 24, BS_AUTOCHECKBOX);
    historyPluginCheck_ = create(L"BUTTON", L"启用 History 插件", ID_HISTORY_PLUGIN,
                                 16, 364, 300, 24, BS_AUTOCHECKBOX);

    statusText_ = create(L"STATIC", L"", 0, 16, 396, 520, 120,
                         SS_LEFT | SS_NOPREFIX | SS_LEFTNOWORDWRAP);
    rebuildButton_ = create(L"BUTTON", L"立即重建索引", ID_REBUILD, 16, 524, 120, 30, BS_PUSHBUTTON);

    create(L"BUTTON", L"Cancel", ID_CANCEL, 340, 580, 100, 32, BS_PUSHBUTTON);
    create(L"BUTTON", L"Save", ID_SAVE, 460, 580, 80, 32, BS_DEFPUSHBUTTON);
}

void SettingsWindow::loadSettings() {
    SetWindowTextW(hotkeyEdit_, settings_.current.hotkey.c_str());
    SendMessageW(autoStartCheck_, BM_SETCHECK, settings_.current.autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(showOnStartupCheck_, BM_SETCHECK, settings_.current.showOnStartup ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(ntfsIndexCheck_, BM_SETCHECK, settings_.current.ntfsIndexEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(liveSearchCheck_, BM_SETCHECK, settings_.current.ntfsLiveSearchEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(plainTextCheck_, BM_SETCHECK, settings_.current.plainTextFileSearch ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(fileSearchPluginCheck_, BM_SETCHECK, settings_.isPluginEnabled(L"File Search") ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(historyPluginCheck_, BM_SETCHECK, settings_.isPluginEnabled(L"History") ? BST_CHECKED : BST_UNCHECKED, 0);

    wchar_t buf[32] = {};
    swprintf_s(buf, L"%d", settings_.current.ntfsLiveSearchTimeoutMs);
    SetWindowTextW(timeoutEdit_, buf);

    int selected = 0;
    for (size_t i = 0; i < std::size(kSearchUrls); i++) {
        if (winutil::equalsIgnoreCase(settings_.current.webSearchUrl, kSearchUrls[i])) selected = static_cast<int>(i);
    }
    SendMessageW(searchCombo_, CB_SETCURSEL, selected, 0);
}

void SettingsWindow::refreshStatus() {
    auto states = ntfs_.driveStates();
    std::wstring text;
    if (states.empty()) {
        text = L"未检测到固定 NTFS 卷。";
    } else {
        for (const auto& s : states) {
            std::wstring line(1, s.drive);
            line += L": ";
            if (s.ready) {
                line += winutil::formatInt(s.recordCount) + L" 项 (" + s.source + L")";
            } else if (s.building) {
                line += L"构建中 " + winutil::formatInt(s.processed) + L" 条记录";
            } else if (s.failed) {
                line += L"失败";
            } else {
                line += L"等待";
            }
            if (!s.message.empty()) line += L" — " + s.message;
            line += L"\r\n";
            text += line;
        }
    }
    SetWindowTextW(statusText_, text.c_str());
}

void SettingsWindow::saveSettings() {
    wchar_t hotkeyText[128] = {};
    GetWindowTextW(hotkeyEdit_, hotkeyText, 127);
    DWORD mods = 0;
    UINT vk = 0;
    if (!hotkey::parse(hotkeyText, mods, vk)) {
        MessageBoxW(hwnd_, L"Invalid hotkey. Use a format like Alt+Space or Ctrl+Shift+Space.",
                    L"hotspot", MB_OK | MB_ICONWARNING);
        return;
    }

    settings_.current.hotkey = hotkeyText;
    settings_.current.autoStart = SendMessageW(autoStartCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings_.current.showOnStartup = SendMessageW(showOnStartupCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings_.current.ntfsIndexEnabled = SendMessageW(ntfsIndexCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings_.current.ntfsLiveSearchEnabled = SendMessageW(liveSearchCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings_.current.plainTextFileSearch = SendMessageW(plainTextCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;

    wchar_t timeoutText[32] = {};
    GetWindowTextW(timeoutEdit_, timeoutText, 31);
    int timeout = _wtoi(timeoutText);
    if (timeout <= 0) timeout = 800;
    settings_.current.ntfsLiveSearchTimeoutMs = std::clamp(timeout, 300, 5000);

    int combo = static_cast<int>(SendMessageW(searchCombo_, CB_GETCURSEL, 0, 0));
    if (combo >= 0 && combo < static_cast<int>(std::size(kSearchUrls))) {
        settings_.current.webSearchUrl = kSearchUrls[combo];
    }

    settings_.setPluginEnabled(L"File Search",
                               SendMessageW(fileSearchPluginCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    settings_.setPluginEnabled(L"History",
                               SendMessageW(historyPluginCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED);

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    applyAutoStart(settings_.current.autoStart, exePath);

    settings_.save();
    if (onSaved_) onSaved_();
    DestroyWindow(hwnd_);
}

LRESULT SettingsWindow::handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == ID_SAVE) {
                saveSettings();
            } else if (id == ID_CANCEL) {
                DestroyWindow(hwnd_);
            } else if (id == ID_REBUILD) {
                EnableWindow(rebuildButton_, FALSE);
                ntfs_.startRebuildAll();
                refreshStatus();
                SetTimer(hwnd_, 1, 1500, nullptr);
            }
            return 0;
        }
        case WM_TIMER:
            if (wParam == 1) {
                refreshStatus();
                auto states = ntfs_.driveStates();
                bool anyBuilding = false;
                for (const auto& s : states) if (s.building) anyBuilding = true;
                if (!anyBuilding) {
                    KillTimer(hwnd_, 1);
                    EnableWindow(rebuildButton_, TRUE);
                }
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY: {
            if (onClosed_) onClosed_();
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            delete this;
            return 0;
        }
        default:
            return DefWindowProcW(hwnd_, msg, wParam, lParam);
    }
}
