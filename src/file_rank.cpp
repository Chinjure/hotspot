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

bool betterMatch(const std::wstring& aName, bool aIsDir,
                 const std::wstring& bName, bool bIsDir,
                 const std::wstring& needle) {
    // 1) Launchable first (executables and shortcuts share one group).
    bool aLaunch = isLaunchable(classify(aName, aIsDir));
    bool bLaunch = isLaunchable(classify(bName, bIsDir));
    if (aLaunch != bLaunch) return aLaunch;

    // 2) Then match quality inside the same group.
    int am = matchScore(aName, needle);
    int bm = matchScore(bName, needle);
    if (am != bm) return am < bm;

    // 3) Then the finer kind (exe before lnk, file before folder).
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
    // 1) Executables and shortcuts stay on top, folders come right after them.
    int ga = pathGroup(aName, aIsDir);
    int gb = pathGroup(bName, bIsDir);
    if (ga != gb) return ga < gb;

    // 2) Inside one group the path match tier decides.
    if (aTier != bTier) return aTier < bTier;

    // 3) Then the finer kind (exe before lnk), name length and alphabetical.
    Kind ak = classify(aName, aIsDir);
    Kind bk = classify(bName, bIsDir);
    if (ak != bk) return static_cast<int>(ak) < static_cast<int>(bk);

    if (aName.size() != bName.size()) return aName.size() < bName.size();
    return winutil::lessIgnoreCase(aName, bName);
}

} // namespace file_rank
