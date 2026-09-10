#include "file_rank.h"

#include <algorithm>
#include <cwctype>

#include "winutil.h"

namespace file_rank {
namespace {

bool isExecutableExt(const std::wstring& ext) {
    static const wchar_t* kExecutables[] = {
        L".exe", L".com", L".bat", L".cmd", L".msi", L".msp", L".ps1",
        L".vbs", L".jar", L".appx", L".msix", L".cpl", L".scr", L".wsf",
    };
    for (auto* e : kExecutables) {
        if (ext == e) return true;
    }
    return false;
}

bool isShortcutExt(const std::wstring& ext) {
    return ext == L".lnk" || ext == L".url";
}

// "steam.exe" -> "steam"; a name without extension is its own stem.
std::wstring stemOf(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return name;
    return name.substr(0, dot);
}

// Match-quality class: which tier is compared before any kind preference.
// Exact name (0) and exact stem (1) share one class -- "steam" is answered by
// both steam.exe and a folder named Steam, so the kind decides between them
// rather than the tier. Prefix (2) and substring (3) stay their own classes,
// and the path-mode Descendant (4) stays the weakest. A negative tier is never
// a real match (path_query::Match::None), so it must not sort to the top.
int qualityClass(int tier) {
    if (tier < 0) return 99;
    return tier <= 1 ? 0 : tier;
}

} // namespace

std::wstring extensionOf(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return L"";
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext;
}

Kind classify(const std::wstring& name, bool isDirectory) {
    if (isDirectory) return Kind::Directory;
    std::wstring ext = extensionOf(name);
    if (isExecutableExt(ext)) return Kind::Executable;
    if (isShortcutExt(ext)) return Kind::Shortcut;
    return Kind::File;
}

int matchScore(const std::wstring& name, const std::wstring& needle) {
    if (needle.empty()) return 3;
    if (winutil::equalsIgnoreCase(name, needle)) return 0;
    std::wstring stem = stemOf(name);
    if (stem != name && winutil::equalsIgnoreCase(stem, needle)) return 1;
    if (winutil::startsWithIgnoreCase(name, needle)) return 2;
    if (stem != name && winutil::startsWithIgnoreCase(stem, needle)) return 2;
    return 3;
}

bool betterNameMatch(const std::wstring& aName, bool aIsDir, int aTier,
                     const std::wstring& bName, bool bIsDir, int bTier) {
    // 1) The better name match always wins; file type never outranks it.
    int ca = qualityClass(aTier);
    int cb = qualityClass(bTier);
    if (ca != cb) return ca < cb;

    // 2) Equally good name matches: launchable first (exe and lnk share a group).
    bool aLaunch = isLaunchable(classify(aName, aIsDir));
    bool bLaunch = isLaunchable(classify(bName, bIsDir));
    if (aLaunch != bLaunch) return aLaunch;

    // 3) Then the exact tier, the finer kind (exe before lnk, file before
    //    folder), name length, and finally the alphabet.
    if (aTier != bTier) return aTier < bTier;
    Kind ak = classify(aName, aIsDir);
    Kind bk = classify(bName, bIsDir);
    if (ak != bk) return static_cast<int>(ak) < static_cast<int>(bk);

    if (aName.size() != bName.size()) return aName.size() < bName.size();
    return winutil::lessIgnoreCase(aName, bName);
}

namespace {

// Path-search grouping: 0 = exe/lnk, 1 = folder, 2 = other file.
int pathGroup(const std::wstring& name, bool isDir) {
    Kind kind = classify(name, isDir);
    if (isLaunchable(kind)) return 0;
    return kind == Kind::Directory ? 1 : 2;
}

} // namespace

bool betterPathMatch(const std::wstring& aName, bool aIsDir, int aTier,
                     const std::wstring& bName, bool bIsDir, int bTier) {
    // 1) The better path match always wins; file type never outranks it.
    int ca = qualityClass(aTier);
    int cb = qualityClass(bTier);
    if (ca != cb) return ca < cb;

    // 2) Equally good path matches: exe/lnk, then folders, then other files.
    int ga = pathGroup(aName, aIsDir);
    int gb = pathGroup(bName, bIsDir);
    if (ga != gb) return ga < gb;

    // 3) Then the exact tier, the finer kind (exe before lnk), name length and
    //    the alphabet.
    if (aTier != bTier) return aTier < bTier;
    Kind ak = classify(aName, aIsDir);
    Kind bk = classify(bName, bIsDir);
    if (ak != bk) return static_cast<int>(ak) < static_cast<int>(bk);

    if (aName.size() != bName.size()) return aName.size() < bName.size();
    return winutil::lessIgnoreCase(aName, bName);
}

} // namespace file_rank
