#pragma once

#include <windows.h>

#include <string>

// Search noise policy: nothing under an AppData folder and nothing hidden
// (hidden folders and their whole subtree, plus hidden files) is searched or
// listed. The NTFS index keeps recording those entries so existing index files
// stay usable — the filter runs while results are produced, which also covers
// entries resurrected by the USN delta overlay.
namespace search_filter {

// True when one path component is named "AppData" (case-insensitive).
bool isUnderAppData(const std::wstring& path);

// True when `path` or any parent directory carries FILE_ATTRIBUTE_HIDDEN.
// Directory attributes are memoised, so a query hitting many files in the same
// folder costs one attribute probe per distinct path.
bool isHidden(const std::wstring& path);

// Full policy for a result path (index hit or live scan hit).
bool isExcluded(const std::wstring& path);

// Enumeration-time variant: walks only descend into included folders, so the
// entry's own name and the attributes already returned by FindFirstFile are
// enough and no extra syscall is needed.
bool isExcludedEntry(const std::wstring& name, DWORD attributes);

// Forgets memoised attributes (e.g. after drives are mounted/unmounted).
void clearCache();

} // namespace search_filter
