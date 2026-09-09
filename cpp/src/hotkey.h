#pragma once

#include <windows.h>

#include <string>

namespace hotkey {

// Parses "Alt+Space", "Ctrl+Shift+F6", "Win+1" etc.
bool parse(const std::wstring& shortcut, DWORD& modifiers, UINT& vk);

bool registerHotKey(HWND hwnd, int id, const std::wstring& shortcut);
void unregisterHotKey(HWND hwnd, int id, bool wasRegistered);

} // namespace hotkey
