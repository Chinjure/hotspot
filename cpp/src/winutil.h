#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace winutil {

// UTF-8 <-> UTF-16 conversion (no BOM).
std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);

// %LOCALAPPDATA% / user profile / standard shell folders.
std::wstring localAppData();
std::wstring userProfile();
std::wstring knownFolder(int csidlOrKnownFolderId, const wchar_t* fallbackName);

// Path helpers (wide).
std::wstring joinPath(const std::wstring& a, const std::wstring& b);
std::wstring parentDir(const std::wstring& path);
bool dirExists(const std::wstring& path);
bool fileExists(const std::wstring& path);
bool createDirectory(const std::wstring& path);

// Read / write UTF-8 text files; return false on failure.
bool readTextFile(const std::wstring& path, std::string& out);
bool writeTextFile(const std::wstring& path, const std::string& content, bool atomic = true);

// Case-insensitive string helpers (ordinal-ish, C-locale for ASCII + Win32).
bool equalsIgnoreCase(const std::wstring& a, const std::wstring& b);
bool startsWithIgnoreCase(const std::wstring& text, const std::wstring& prefix);
bool containsIgnoreCase(const std::wstring& text, const std::wstring& needle);
bool lessIgnoreCase(const std::wstring& a, const std::wstring& b);

// Human readable Win32 error message.
std::wstring lastErrorText(DWORD code = ::GetLastError());

// Append a line to %LOCALAPPDATA%\hotspot\debug.log (diagnostics).
void debugLog(const std::wstring& message);

// ISO-8601 local time with offset, e.g. 2026-09-03T17:06:24.123+08:00.
std::wstring nowIsoLocal();

// Convert int64 to human readable with thousands separators (simple).
std::wstring formatInt(int64_t value);

// Wide string trim.
std::wstring trim(const std::wstring& s);

// Tokenize "Alt+Space" style shortcuts is handled by hotkey.h.
// Format a number like .NET "{N0}" in zh/current locale is approximated.
} // namespace winutil
