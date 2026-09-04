#include "hotkey.h"

#include <cwctype>

#include "winutil.h"

namespace hotkey {

namespace {
UINT parseKey(const std::wstring& token) {
    if (token.empty()) return 0;
    std::wstring t = token;
    for (auto& c : t) c = static_cast<wchar_t>(std::towlower(c));

    if (t == L"space") return VK_SPACE;
    if (t == L"enter" || t == L"return") return VK_RETURN;
    if (t == L"esc" || t == L"escape") return VK_ESCAPE;
    if (t == L"tab") return VK_TAB;
    if (t == L"backspace") return VK_BACK;
    if (t == L"delete") return VK_DELETE;
    if (t == L"insert") return VK_INSERT;
    if (t == L"up") return VK_UP;
    if (t == L"down") return VK_DOWN;
    if (t == L"left") return VK_LEFT;
    if (t == L"right") return VK_RIGHT;
    if (t == L"home") return VK_HOME;
    if (t == L"end") return VK_END;
    if (t == L"pageup") return VK_PRIOR;
    if (t == L"pagedown") return VK_NEXT;

    if (t.size() == 1 && (std::iswalpha(t[0]) || std::iswdigit(t[0]))) {
        return static_cast<UINT>(std::towupper(t[0]));
    }

    if (t[0] == L'f' && t.size() > 1) {
        int n = _wtoi(t.c_str() + 1);
        if (n >= 1 && n <= 24) return static_cast<UINT>(VK_F1 + n - 1);
    }
    return 0;
}
} // namespace

bool parse(const std::wstring& shortcut, DWORD& modifiers, UINT& vk) {
    modifiers = 0;
    vk = 0;
    std::wstring s = winutil::trim(shortcut);
    if (s.empty()) return false;

    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L'+') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return false;

    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        std::wstring p = parts[i];
        for (auto& c : p) c = static_cast<wchar_t>(std::towlower(c));
        if (p == L"alt") modifiers |= MOD_ALT;
        else if (p == L"ctrl" || p == L"control") modifiers |= MOD_CONTROL;
        else if (p == L"shift") modifiers |= MOD_SHIFT;
        else if (p == L"win" || p == L"windows") modifiers |= MOD_WIN;
        else {
            // "Ctrl+Alt+Space" style: unknown modifier token is invalid.
            return false;
        }
    }

    vk = parseKey(parts.back());
    return vk != 0;
}

bool registerHotKey(HWND hwnd, int id, const std::wstring& shortcut) {
    DWORD mods = 0;
    UINT vk = 0;
    if (!parse(shortcut, mods, vk)) return false;
    return RegisterHotKey(hwnd, id, mods, vk) != FALSE;
}

void unregisterHotKey(HWND hwnd, int id, bool wasRegistered) {
    if (hwnd && wasRegistered) UnregisterHotKey(hwnd, id);
}

} // namespace hotkey
