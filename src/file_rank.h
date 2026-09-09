#pragma once

#include <string>

// Result ordering shared by every file-search path (NTFS index, live scan,
// legacy folder walk) so a launcher hit ranks the same everywhere.
//
// Priority, strongest first:
//   1. kind            launchable (.exe/.com/.bat/.cmd/.msi/.ps1/.lnk/.url)
//                      > other files > folders
//   2. match quality   exact file name > exact stem > prefix > substring
//   3. shorter name, then case-insensitive alphabetical
//
// Kind leads because this is a launcher: typing "steam" must put steam.exe and
// Steam.lnk above the many directories that merely contain the word, while
// typing "report" still puts report.md/report.py above the Crashpad "reports"
// folders. Inside one kind, match quality decides.
namespace file_rank {

enum class Kind {
    Executable = 0, // .exe .com .bat .cmd .msi .ps1 ...
    Shortcut = 1,   // .lnk .url
    File = 2,
    Directory = 3,
};

// Launchable = the user can double-click it to start something.
inline bool isLaunchable(Kind kind) {
    return kind == Kind::Executable || kind == Kind::Shortcut;
}

// Extension-based classification; directories win over any extension.
Kind classify(const std::wstring& name, bool isDirectory);

// File-name extension including the dot, lowercased ("" when there is none).
std::wstring extensionOf(const std::wstring& name);

// Lower match tier = better (0 exact name, 1 exact stem, 2 prefix, 3 substring).
int matchScore(const std::wstring& name, const std::wstring& needle);

// Full ordering used to sort candidates for one query.
bool betterMatch(const std::wstring& aName, bool aIsDir,
                 const std::wstring& bName, bool bIsDir,
                 const std::wstring& needle);

// Ordering used when the query is a path (it contains a separator). Priority:
//   1. launchable (.exe/.lnk/...)   launcher priority, unchanged
//   2. directories                  "second only to exe/lnk"
//   3. other files
//   4. inside one group: path tier, then kind, then shorter name, alphabetical
// `tier` is the path_query score, so typing "C:\Users\me\Desktop\proj" still
// puts that exact folder above loose files found deeper in its subtree.
bool betterPathMatch(const std::wstring& aName, bool aIsDir, int aTier,
                     const std::wstring& bName, bool bIsDir, int bTier);

} // namespace file_rank
