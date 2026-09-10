#pragma once

#include <string>

// Result ordering shared by every file-search path (NTFS index, live scan,
// legacy folder walk) so a launcher hit ranks the same everywhere.
//
// Priority, strongest first:
//   1. match quality   exact file name > exact stem > prefix > substring
//   2. kind            launchable (.exe/.com/.bat/.cmd/.msi/.ps1/.lnk/.url)
//                      > other files > folders
//   3. shorter name, then case-insensitive alphabetical
//
// Match quality leads, kind only breaks ties. Typing "clock" must put the
// folder literally named "clock" above Clock Widget.lnk: a type bonus (or the
// folder bonus) may never outrank a better name match. Among equally good
// matches the launcher order still holds, so "steam" gives steam.exe, then
// Steam.lnk, then the folder Steam (exact name and exact stem are one quality
// class: both answer the query with the item's own name).
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

// Full ordering used to sort candidates for one name query. `tier` is
// matchScore for that name. Priority:
//   1. match-quality class  exact name / exact stem, then prefix, then contains
//   2. launchable (.exe/.lnk/...)   only among equally good name matches
//   3. finer kind (exe before lnk, file before folder), exact name before exact
//      stem, then shorter name and alphabetical
bool betterNameMatch(const std::wstring& aName, bool aIsDir, int aTier,
                     const std::wstring& bName, bool bIsDir, int bTier);

// Ordering used when the query is a path (it contains a separator). `tier` is
// the path_query score. Same rule as name mode: the match decides first.
// Priority:
//   1. path match quality  exact name / exact stem, prefix, contains, then
//                          descendants of the folder named by the query
//   2. launchable (.exe/.lnk/...) > folders > other files
//   3. finer kind, exact name before exact stem, shorter name, alphabetical
// So typing "C:\Users\me\Desktop\proj" still puts that exact folder above loose
// files found deeper in its subtree (and above a mere prefix hit next to it),
// while a folder the query names exactly is never pushed down by a shortcut
// that merely happens to sit inside it.
bool betterPathMatch(const std::wstring& aName, bool aIsDir, int aTier,
                     const std::wstring& bName, bool bIsDir, int bTier);

} // namespace file_rank
