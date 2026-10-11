#include "winutil.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <sstream>

namespace winutil {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring localAppData() {
    wchar_t buf[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, buf) == S_OK) {
        return buf;
    }
    const wchar_t* base = _wgetenv(L"LOCALAPPDATA");
    return base ? base : L"C:\\Users\\Public";
}

std::wstring userProfile() {
    wchar_t buf[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, buf) == S_OK) {
        return buf;
    }
    const wchar_t* base = _wgetenv(L"USERPROFILE");
    return base ? base : L"C:\\Users";
}

std::wstring knownFolder(int csidl, const wchar_t* fallbackName) {
    wchar_t buf[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, csidl | CSIDL_FLAG_CREATE, nullptr, 0, buf) == S_OK) {
        return buf;
    }
    std::wstring base = userProfile();
    if (fallbackName) base = joinPath(base, fallbackName);
    return base;
}

std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

bool dirExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool fileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool createDirectory(const std::wstring& path) {
    if (dirExists(path)) return true;
    // Create recursively.
    std::wstring current;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == L'\\' || path[i] == L'/') {
            if (!current.empty()) {
                CreateDirectoryW(current.c_str(), nullptr);
            }
        }
        current.push_back(path[i]);
    }
    if (!current.empty()) {
        CreateDirectoryW(current.c_str(), nullptr);
    }
    return dirExists(path);
}

bool readTextFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.clear();
    char buf[64 * 1024];
    DWORD read = 0;
    while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) {
        out.append(buf, read);
    }
    CloseHandle(h);
    return true;
}

static bool writeFileRaw(const std::wstring& path, const std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t off = 0;
    while (off < content.size()) {
        DWORD written = 0;
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(content.size() - off, 1u << 20));
        if (!WriteFile(h, content.data() + off, chunk, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        off += written;
    }
    CloseHandle(h);
    return ok;
}

bool writeTextFile(const std::wstring& path, const std::string& content, bool atomic) {
    createDirectory(parentDir(path));
    if (!atomic) return writeFileRaw(path, content);
    std::wstring tmp = path + L".tmp";
    if (!writeFileRaw(tmp, content)) return false;
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::wstring parentDir(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return path.substr(0, pos);
}

bool equalsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    return lstrcmpiW(a.c_str(), b.c_str()) == 0;
}

bool startsWithIgnoreCase(const std::wstring& text, const std::wstring& prefix) {
    if (prefix.size() > text.size()) return false;
    if (prefix.empty()) return true;
    return _wcsnicmp(text.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool containsIgnoreCase(const std::wstring& text, const std::wstring& needle) {
    if (needle.empty()) return true;
    return StrStrIW(text.c_str(), needle.c_str()) != nullptr;
}

bool lessIgnoreCase(const std::wstring& a, const std::wstring& b) {
    int r = lstrcmpiW(a.c_str(), b.c_str());
    if (r != 0) return r < 0;
    return a < b;
}

namespace {

// One-time invariant-locale lowercase table for 16-bit code units. The NTFS
// record scan compares names directly inside the memory-mapped name section
// with this table, so a std::wstring is only allocated on an actual match.
const uint16_t* caseFoldTable() {
    static const std::array<uint16_t, 0x10000> table = [] {
        std::array<uint16_t, 0x10000> t{};
        for (uint32_t i = 0; i < 0x10000; i++) t[i] = static_cast<uint16_t>(i);
        for (uint32_t i = 0; i < 0x10000; i++) {
            wchar_t src = static_cast<wchar_t>(i);
            wchar_t out[2] = {};
            int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, &src, 1, out, 2,
                                  nullptr, nullptr, 0);
            if (n == 1) t[i] = static_cast<uint16_t>(out[0]);
        }
        return t;
    }();
    return table.data();
}

} // namespace

wchar_t foldChar(wchar_t c) {
    return static_cast<wchar_t>(caseFoldTable()[static_cast<uint16_t>(c)]);
}

std::wstring foldString(const std::wstring& text) {
    const uint16_t* table = caseFoldTable();
    std::wstring out(text.size(), L'\0');
    for (size_t i = 0; i < text.size(); i++) {
        out[i] = static_cast<wchar_t>(table[static_cast<uint16_t>(text[i])]);
    }
    return out;
}

bool containsFolded(const wchar_t* hay, size_t hayLen, const wchar_t* folded, size_t foldedLen) {
    if (foldedLen == 0) return true;
    if (hayLen < foldedLen) return false;
    const uint16_t* table = caseFoldTable();
    const uint16_t first = static_cast<uint16_t>(folded[0]);
    const size_t last = hayLen - foldedLen;
    for (size_t i = 0; i <= last; i++) {
        if (table[static_cast<uint16_t>(hay[i])] != first) continue;
        size_t j = 1;
        for (; j < foldedLen; j++) {
            if (table[static_cast<uint16_t>(hay[i + j])] != static_cast<uint16_t>(folded[j])) break;
        }
        if (j == foldedLen) return true;
    }
    return false;
}

std::wstring lastErrorText(DWORD code) {
    if (code == 0) code = GetLastError();
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring msg;
    if (n > 0 && buf != nullptr) {
        msg.assign(buf, static_cast<size_t>(n));
        LocalFree(buf);
        while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n')) msg.pop_back();
    }
    if (msg.empty()) {
        wchar_t tmp[32];
        swprintf_s(tmp, L"error %lu", code);
        msg = tmp;
    }
    return msg;
}

void debugLog(const std::wstring& message) {
    std::wstring path = joinPath(localAppData(), L"hotspot");
    createDirectory(path);
    path = joinPath(path, L"debug.log");
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::wstring line = nowIsoLocal() + L" " + message + L"\r\n";
    std::string utf8 = wideToUtf8(line);
    DWORD written = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
}

std::wstring nowIsoLocal() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    TIME_ZONE_INFORMATION tz{};
    DWORD tzResult = GetTimeZoneInformation(&tz);
    int offsetMinutes = 0;
    if (tzResult == TIME_ZONE_ID_STANDARD) offsetMinutes = tz.Bias;
    else if (tzResult == TIME_ZONE_ID_DAYLIGHT) offsetMinutes = tz.Bias + tz.DaylightBias;
    // Windows Bias = minutes to add to local time to get UTC; so local offset = -bias.
    int totalMinutes = -offsetMinutes;
    wchar_t sign = totalMinutes < 0 ? L'-' : L'+';
    if (totalMinutes < 0) totalMinutes = -totalMinutes;
    wchar_t buf[64];
    swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02u.%03u%c%02d:%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
               sign, totalMinutes / 60, totalMinutes % 60);
    return buf;
}

std::wstring formatInt(int64_t value) {
    std::wstring s = std::to_wstring(value < 0 ? -value : value);
    std::wstring out;
    int count = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        out.push_back(*it);
        if (++count % 3 == 0 && (it + 1) != s.rend()) out.push_back(L',');
    }
    std::reverse(out.begin(), out.end());
    if (value < 0) out.insert(out.begin(), L'-');
    return out;
}

std::wstring trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) b++;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) e--;
    return s.substr(b, e - b);
}

} // namespace winutil
