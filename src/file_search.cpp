#include "file_search.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <deque>
#include <functional>
#include <iterator>
#include <unordered_set>

#include "ntfs/ntfs_win32.h"
#include "winutil.h"

namespace {

std::wstring caseFold(const std::wstring& s) {
    std::wstring out = s;
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

struct CIHash {
    size_t operator()(const std::wstring& s) const {
        return std::hash<std::wstring>()(caseFold(s));
    }
};
struct CIEqual {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return winutil::equalsIgnoreCase(a, b);
    }
};
using PathSet = std::unordered_set<std::wstring, CIHash, CIEqual>;

void addResult(const std::wstring& name, const std::wstring& path, bool isDir,
               const std::wstring& extraSubtitle, std::vector<ResultItem>& out) {
    ResultItem item;
    item.title = name;
    item.subtitle = extraSubtitle.empty() ? path : path + L"　· " + extraSubtitle;
    // Emoji fallback only; the launcher draws the real shell icon from iconPath.
    item.icon = isDir ? L"📁" : L"📄";
    item.iconPath = path;
    item.pluginName = L"File Search";
    item.copyToClipboard = true;
    item.clipboardText = path;
    item.path = path;
    item.isDirectory = isDir;
    item.kind = file_rank::classify(name, isDir);
    item.launchable = file_rank::isLaunchable(item.kind);
    out.push_back(std::move(item));
}

// Recursive enumeration that skips reparse points and inaccessible dirs.
// `onHit` receives the full path, its containing directory and the leaf name.
// Returns true when fully done; false when time/cancellation stopped it.
bool enumerateTree(const std::wstring& root, int maxDepth,
                   const std::function<bool(const std::wstring& path, const std::wstring& parent,
                                            const std::wstring& name, bool isDir)>& onHit,
                   const std::atomic<bool>* cancel,
                   std::chrono::steady_clock::time_point deadline) {
    struct Frame {
        std::wstring path;
        int depth = 0;
    };
    std::deque<Frame> stack;
    stack.push_back(Frame{root, 0});
    while (!stack.empty()) {
        if (cancel && cancel->load()) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        Frame frame = stack.back();
        stack.pop_back();
        if (frame.depth > maxDepth) continue;

        std::wstring pattern = frame.path + L"\\*";
        WIN32_FIND_DATAW data{};
        HANDLE h = FindFirstFileExW(pattern.c_str(),
                                    static_cast<FINDEX_INFO_LEVELS>(ntfs_win32::kFindExInfoBasic), &data,
                                    static_cast<FINDEX_SEARCH_OPS>(ntfs_win32::kFindExSearchNameMatch), nullptr,
                                    static_cast<DWORD>(ntfs_win32::kFindFirstExLargeFetch));
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring name = data.cFileName;
            if (name.empty() || name == L"." || name == L"..") continue;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;

            bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            std::wstring full = frame.path + L"\\" + name;
            if (!onHit(full, frame.path, name, isDir)) {
                FindClose(h);
                return true;
            }
            if (isDir) stack.push_back(Frame{full, frame.depth + 1});
        } while (FindNextFileW(h, &data));
        FindClose(h);
    }
    return true;
}

} // namespace

FileSearchService::FileSearchService(SettingsService& settings, NtfsService& ntfs, HistoryService& history)
    : settings_(settings), ntfs_(ntfs), history_(history) {}

bool FileSearchService::startsWithCommandPrefix(const std::wstring& raw) {
    if (raw.empty()) return false;
    // Symbol-led commands belong to other plugins (shell, registry, URI...).
    std::wstring symbols = L"? > $ = ~ ! : # / _ @ < )";
    if (symbols.find(raw[0]) != std::wstring::npos) return true;

    static const wchar_t* prefixes[] = {
        L"fs ", L"ww ", L"kill ", L"env ", L"reg ", L"folder ", L"uri ", L"win ",
        L"vsc ", L"u ", L"o:", L"??", L"!!", L"//", L"mailto:", L"http://", L"https://",
    };
    for (auto* p : prefixes) {
        if (winutil::startsWithIgnoreCase(raw, p)) return true;
    }
    return false;
}

std::vector<std::wstring> FileSearchService::commonSearchRoots() {
    std::vector<std::wstring> roots;
    PathSet seen;

    auto add = [&](const std::wstring& p) {
        if (p.empty() || !winutil::dirExists(p)) return;
        if (seen.insert(p).second) roots.push_back(p);
    };

    add(winutil::knownFolder(CSIDL_DESKTOPDIRECTORY, L"Desktop"));
    add(winutil::knownFolder(CSIDL_PERSONAL, L"Documents"));
    add(winutil::knownFolder(CSIDL_MYPICTURES, L"Pictures"));
    add(winutil::knownFolder(CSIDL_MYMUSIC, L"Music"));
    add(winutil::knownFolder(CSIDL_MYVIDEO, L"Videos"));
    add(winutil::joinPath(winutil::userProfile(), L"Downloads"));
    return roots;
}

std::vector<std::wstring> FileSearchService::searchRoots() {
    std::vector<std::wstring> roots = commonSearchRoots();
    PathSet seen;
    for (const auto& r : roots) seen.insert(r);

    auto add = [&](const std::wstring& p) {
        if (p.empty() || !winutil::dirExists(p)) return;
        if (seen.insert(p).second) roots.push_back(p);
    };

    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'C'; c <= L'Z'; ++c) {
        DWORD bit = 1u << (c - L'A');
        if ((mask & bit) == 0) continue;
        std::wstring root(1, c);
        root += L":\\";
        UINT type = GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE) continue;
        add(root);
    }
    return roots;
}

std::vector<std::wstring> FileSearchService::scanRoots(const path_query::Query* pathQuery,
                                                       bool includeAllDrives) {
    // An absolute path query can start right next to its target instead of at
    // every drive root; the index still covers hits outside that prefix.
    if (pathQuery && pathQuery->absolute) {
        std::wstring root = path_query::existingRootOf(*pathQuery);
        if (!root.empty()) return {root};
    }
    return includeAllDrives ? searchRoots() : commonSearchRoots();
}

void FileSearchService::query(const std::wstring& raw, int maxResults, std::vector<ResultItem>& out,
                              const std::atomic<bool>* cancel) {
    if (raw.empty()) return;
    bool useIndexer = winutil::startsWithIgnoreCase(raw, L"?") && !winutil::startsWithIgnoreCase(raw, L"??");
    bool useFileSearch = winutil::startsWithIgnoreCase(raw, L"fs ");
    // Typing a path is an explicit file request: it enables the plain-text
    // pipeline even when prefix-free search is switched off.
    bool pathCandidate = path_query::looksLikePath(raw);
    bool plainSearch = !useIndexer && !useFileSearch &&
                       (settings_.current.plainTextFileSearch || pathCandidate) &&
                       !startsWithCommandPrefix(raw);
    if (!useIndexer && !useFileSearch && !plainSearch) return;

    std::wstring search;
    if (useIndexer) search = winutil::trim(raw.substr(1));
    else if (useFileSearch) search = winutil::trim(raw.substr(3));
    else search = winutil::trim(raw);

    if (search.size() < 2) {
        if (plainSearch) return;
        ResultItem hint;
        hint.title = L"Enter a file name";
        hint.subtitle = L"Example: fs report.docx  ·  ? 2024 会议纪要  ·  直接输入文件名  ·  输入 \\ 按路径搜索";
        hint.icon = L"📁";
        hint.pluginName = L"File Search";
        hint.closeAfterExecute = false;
        out.push_back(std::move(hint));
        return;
    }

    // A separator turns the query into a path query ("C:\Users\me\Desktop",
    // "src\components"); everything else stays a plain name search.
    path_query::Query pathQuery;
    const path_query::Query* pathPtr = nullptr;
    if (path_query::looksLikePath(search)) {
        pathQuery = path_query::parse(search);
        if (pathQuery.valid) pathPtr = &pathQuery;
    }

    int max = std::clamp(settings_.current.fileSearchMaxResults, 1, 2000);
    if (maxResults > 0 && maxResults < max) max = maxResults; // caller's cap wins when smaller
    if (!settings_.current.ntfsIndexEnabled) {
        legacySearch(search, max, out, cancel, pathPtr);
        rankResults(search, out, pathPtr);
        return;
    }

    NtfsServiceStatus status = ntfs_.status();
    std::vector<ResultItem> indexed;
    searchIndex(search, max, indexed, cancel, pathPtr);

    PathSet seen;
    for (const auto& r : indexed) seen.insert(r.path);
    out.insert(out.end(), std::make_move_iterator(indexed.begin()), std::make_move_iterator(indexed.end()));

    bool runLive = settings_.current.ntfsLiveSearchEnabled &&
                   (!plainSearch || out.empty()) &&
                   static_cast<int>(out.size()) < max;
    if (runLive) {
        std::vector<ResultItem> live;
        liveSearch(search, max - static_cast<int>(out.size()), live, cancel, pathPtr);
        for (auto& r : live) {
            if (!seen.insert(r.path).second) continue;
            out.push_back(std::move(r));
        }
    }

    // Rank the merged set (index + live scan) with one comparator so executables
    // and shortcuts float to the top no matter which source produced them.
    rankResults(search, out, pathPtr);

    if (!out.empty()) return;

    if (status.buildingCount > 0) {
        ResultItem hint;
        hint.title = L"NTFS 索引构建中，实时搜索未命中…";
        hint.subtitle = status.summary + L" · 索引完成后将全盘秒搜";
        hint.icon = L"⏳";
        hint.pluginName = L"File Search";
        hint.closeAfterExecute = false;
        out.push_back(std::move(hint));
        return;
    }
    if (status.failedCount > 0) {
        ResultItem hint;
        hint.title = L"NTFS 索引不可用";
        hint.subtitle = status.summary;
        hint.icon = L"⚠";
        hint.pluginName = L"File Search";
        hint.closeAfterExecute = false;
        out.push_back(std::move(hint));
        return;
    }
    legacySearch(search, max, out, cancel, pathPtr);
    if (pathPtr) rankResults(search, out, pathPtr);
    if (!out.empty()) return;

    if (pathPtr) {
        ResultItem hint;
        hint.title = L"路径搜索无命中";
        hint.subtitle = L"示例：C:\\Users\\me\\Desktop\\proj  ·  src\\components\\button.tsx（\\ 触发按路径搜索）";
        hint.icon = L"📁";
        hint.pluginName = L"File Search";
        hint.closeAfterExecute = false;
        out.push_back(std::move(hint));
    }
}

void FileSearchService::queryHistory(const std::wstring& raw, std::vector<ResultItem>& out) {
    if (!(winutil::equalsIgnoreCase(raw, L"history") ||
          winutil::equalsIgnoreCase(raw, L"hist") ||
          winutil::startsWithIgnoreCase(raw, L"!!"))) {
        return;
    }
    size_t count = std::min<size_t>(30, history_.entries.size());
    for (size_t i = 0; i < count; i++) {
        const auto& e = history_.entries[i];
        ResultItem item;
        item.title = e.title;
        item.subtitle = e.pluginName + L" · " + e.usedAt.substr(0, std::min<size_t>(16, e.usedAt.size()));
        item.icon = L"🕘";

        // Resolve the row's real path so history shows the same shell icon as
        // live results. Entries written by this version carry it directly;
        // older entries are matched back through the index by exact file name.
        std::wstring resolved = e.path;
        if (resolved.empty() && !e.title.empty() && settings_.current.ntfsIndexEnabled) {
            auto hits = ntfs_.search(e.title, 8, nullptr);
            for (const auto& h : hits) {
                if (winutil::equalsIgnoreCase(h.name, e.title)) {
                    resolved = h.path;
                    break;
                }
            }
        }
        if (!resolved.empty()) {
            DWORD attrs = GetFileAttributesW(resolved.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                item.iconPath = resolved;
                item.path = resolved;
                item.isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
                item.kind = file_rank::classify(e.title, item.isDirectory);
            }
        }

        item.pluginName = L"History";
        item.copyToClipboard = true;
        item.clipboardText = e.title;
        item.closeAfterExecute = false;
        out.push_back(std::move(item));
    }
}

void FileSearchService::rankResults(const std::wstring& search, std::vector<ResultItem>& out,
                                    const path_query::Query* pathQuery) {
    if (out.size() < 2) return;
    if (!pathQuery) {
        for (auto& item : out) {
            if (item.path.empty()) continue; // hint row, never ranked with hits
            item.matchTier = file_rank::matchScore(item.title, search);
        }
    }
    // Path mode keeps the tier computed against the path (re-scoring the bare
    // title against the whole query would flatten every hit to "contains").
    std::stable_sort(out.begin(), out.end(), [pathQuery](const ResultItem& a, const ResultItem& b) {
        bool aHint = a.path.empty();
        bool bHint = b.path.empty();
        if (aHint != bHint) return !aHint; // hints always last
        if (aHint && bHint) return false;
        if (pathQuery) {
            return file_rank::betterPathMatch(a.title, a.isDirectory, a.matchTier,
                                              b.title, b.isDirectory, b.matchTier);
        }
        return file_rank::betterNameMatch(a.title, a.isDirectory, a.matchTier,
                                          b.title, b.isDirectory, b.matchTier);
    });
}

void FileSearchService::searchIndex(const std::wstring& search, int max, std::vector<ResultItem>& out,
                                    const std::atomic<bool>* cancel, const path_query::Query* pathQuery) {
    auto hits = ntfs_.search(search, max, cancel, pathQuery);
    for (auto& h : hits) {
        addResult(h.name, h.path, h.isDirectory, L"", out);
        out.back().matchTier = h.matchTier;
        out.back().kind = h.kind;
        out.back().launchable = h.launchable;
    }
}

void FileSearchService::liveSearch(const std::wstring& search, int max, std::vector<ResultItem>& out,
                                   const std::atomic<bool>* cancel, const path_query::Query* pathQuery) {
    if (max <= 0) return;
    int timeoutMs = std::clamp(settings_.current.ntfsLiveSearchTimeoutMs, 300, 5000);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    PathSet seen;

    for (const auto& root : scanRoots(pathQuery, true)) {
        if (static_cast<int>(out.size()) >= max) break;
        enumerateTree(root, 256,
                      [&](const std::wstring& path, const std::wstring& parent,
                          const std::wstring& name, bool isDir) -> bool {
                          if (static_cast<int>(out.size()) >= max) return false;
                          int tier = 3;
                          if (pathQuery) {
                              path_query::Match match = path_query::scorePath(parent, name, isDir, *pathQuery);
                              if (match == path_query::Match::None) return true;
                              tier = static_cast<int>(match);
                          } else if (!winutil::containsIgnoreCase(name, search)) {
                              return true;
                          }
                          if (!seen.insert(path).second) return true;
                          addResult(name, path, isDir, L"实时命中（索引外）", out);
                          out.back().matchTier = tier;
                          return static_cast<int>(out.size()) < max;
                      },
                      cancel, deadline);
    }
}

void FileSearchService::legacySearch(const std::wstring& search, int max, std::vector<ResultItem>& out,
                                     const std::atomic<bool>* cancel, const path_query::Query* pathQuery) {
    if (max <= 0) return;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    PathSet seen;
    // Only the common user folders (parity with the C# version); the whole
    // drive trees are covered by the NTFS index + time-boxed live scan. Path
    // queries may start at their own existing prefix instead.
    for (const auto& root : scanRoots(pathQuery, false)) {
        if (static_cast<int>(out.size()) >= max) break;
        enumerateTree(root, 256,
                      [&](const std::wstring& path, const std::wstring& parent,
                          const std::wstring& name, bool isDir) -> bool {
                          if (static_cast<int>(out.size()) >= max) return false;
                          // Name mode keeps files-only results; path search must
                          // return folders too ("search files or folders by path").
                          if (isDir && !pathQuery) return true;
                          int tier = 3;
                          if (pathQuery) {
                              path_query::Match match = path_query::scorePath(parent, name, isDir, *pathQuery);
                              if (match == path_query::Match::None) return true;
                              tier = static_cast<int>(match);
                          } else if (!winutil::containsIgnoreCase(name, search)) {
                              return true;
                          }
                          if (!seen.insert(path).second) return true;
                          addResult(name, path, isDir, L"", out);
                          out.back().matchTier = tier;
                          return static_cast<int>(out.size()) < max;
                      },
                      cancel, deadline);
    }
}
