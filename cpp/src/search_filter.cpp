#include "search_filter.h"

#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>

namespace search_filter {

namespace {

constexpr size_t kMaxCachedPaths = 65536;

// Each entry means "this path and every ancestor above it are visible" (false)
// or "this path or an ancestor is hidden" (true), so a hit can short-circuit
// the rest of the ancestor walk.
std::mutex& cacheMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::wstring, bool>& cache() {
    static std::unordered_map<std::wstring, bool> entries;
    return entries;
}

bool cachedLookup(const std::wstring& path, bool& value) {
    std::lock_guard<std::mutex> lock(cacheMutex());
    auto it = cache().find(path);
    if (it == cache().end()) return false;
    value = it->second;
    return true;
}

void cacheStore(const std::wstring& path, bool value) {
    std::lock_guard<std::mutex> lock(cacheMutex());
    if (cache().size() >= kMaxCachedPaths) cache().clear();
    cache()[path] = value;
}

bool componentIsAppData(const wchar_t* text, size_t length) {
    static constexpr wchar_t kAppData[] = L"appdata";
    if (length != 7) return false;
    for (size_t i = 0; i < length; i++) {
        if (std::towlower(text[i]) != kAppData[i]) return false;
    }
    return true;
}

// True when `path` itself or one of its ancestors is hidden. Missing paths are
// treated as visible: the index can still hold entries that were deleted after
// the last USN pass, and hiding those would silently drop valid neighbours.
bool probeHidden(const std::wstring& path) {
    std::wstring current = path;
    bool hidden = false;
    for (int guard = 0; guard < 64 && !current.empty(); guard++) {
        bool value = false;
        if (cachedLookup(current, value)) {
            hidden = value;
            break;
        }
        DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            // A path we may not stat (another user's profile, system hives) is
            // treated as hidden: it is both invisible and useless in results.
            if (GetLastError() == ERROR_ACCESS_DENIED) hidden = true;
            break;
        }
        if ((attributes & FILE_ATTRIBUTE_HIDDEN) != 0) {
            hidden = true;
            break;
        }
        size_t pos = current.find_last_of(L"\\/");
        if (pos == std::wstring::npos) break;
        std::wstring parent = current.substr(0, pos);
        // "C:" / "" / "." have no usable parent left.
        if (parent.empty() || parent == current) break;
        if (parent.size() == 2 && parent[1] == L':') break;
        current = parent;
    }
    cacheStore(path, hidden);
    return hidden;
}

} // namespace

bool isUnderAppData(const std::wstring& path) {
    const wchar_t* text = path.c_str();
    size_t length = path.size();
    size_t i = 0;
    while (i < length) {
        while (i < length && (text[i] == L'\\' || text[i] == L'/')) i++;
        size_t start = i;
        while (i < length && text[i] != L'\\' && text[i] != L'/') i++;
        if (i > start && componentIsAppData(text + start, i - start)) return true;
    }
    return false;
}

bool isHidden(const std::wstring& path) { return path.empty() ? false : probeHidden(path); }

bool isExcluded(const std::wstring& path) {
    if (path.empty()) return false;
    if (isUnderAppData(path)) return true; // cheapest check first
    return isHidden(path);
}

bool isExcludedEntry(const std::wstring& name, DWORD attributes) {
    if ((attributes & FILE_ATTRIBUTE_HIDDEN) != 0) return true;
    return componentIsAppData(name.c_str(), name.size());
}

void clearCache() {
    std::lock_guard<std::mutex> lock(cacheMutex());
    cache().clear();
}

} // namespace search_filter
