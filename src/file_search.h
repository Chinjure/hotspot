#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "file_rank.h"
#include "history.h"
#include "ntfs/ntfs_service.h"
#include "path_query.h"
#include "settings.h"

struct ResultItem {
    std::wstring title;
    std::wstring subtitle;
    // Emoji fallback, drawn only when no shell icon could be resolved.
    std::wstring icon;
    // File-system path whose real shell icon should be shown for this row.
    std::wstring iconPath;
    std::wstring pluginName;
    std::wstring clipboardText;
    std::wstring path;
    bool isDirectory = false;
    bool copyToClipboard = false;
    bool closeAfterExecute = true;
    // Ranking fields (file results only); hints keep the defaults.
    int matchTier = 3;
    file_rank::Kind kind = file_rank::Kind::File;
    bool launchable = false;
};

class FileSearchService {
public:
    FileSearchService(SettingsService& settings, NtfsService& ntfs, HistoryService& history);

    // Runs the file-search query pipeline (fs / ? / plain) and appends results.
    // A query containing a path separator ("C:\...", "src\components") switches
    // to path search: candidates are matched against their full path and ranked
    // exe/lnk > folder > file.
    void query(const std::wstring& raw, int maxResults, std::vector<ResultItem>& out,
               const std::atomic<bool>* cancel = nullptr);

    // "history" / "hist" / "!!" support (last 30 entries).
    void queryHistory(const std::wstring& raw, std::vector<ResultItem>& out);

private:
    void searchIndex(const std::wstring& search, int max, std::vector<ResultItem>& out,
                     const std::atomic<bool>* cancel, const path_query::Query* pathQuery);
    void liveSearch(const std::wstring& search, int max, std::vector<ResultItem>& out,
                    const std::atomic<bool>* cancel, const path_query::Query* pathQuery);
    void legacySearch(const std::wstring& search, int max, std::vector<ResultItem>& out,
                      const std::atomic<bool>* cancel, const path_query::Query* pathQuery);
    // Name mode: match quality, then exe/lnk first, then name length.
    // Path mode: exe/lnk, then folders, then files (see file_rank::betterPathMatch).
    static void rankResults(const std::wstring& search, std::vector<ResultItem>& out,
                            const path_query::Query* pathQuery);
    // Roots for the time-boxed scans: the query's own existing directory prefix
    // when it is absolute, otherwise the usual folder/drive list.
    static std::vector<std::wstring> scanRoots(const path_query::Query* pathQuery, bool includeAllDrives);

    static bool startsWithCommandPrefix(const std::wstring& raw);
    static std::vector<std::wstring> searchRoots();
    static std::vector<std::wstring> commonSearchRoots();

    SettingsService& settings_;
    NtfsService& ntfs_;
    HistoryService& history_;
};
