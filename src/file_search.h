#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "history.h"
#include "ntfs/ntfs_service.h"
#include "settings.h"

struct ResultItem {
    std::wstring title;
    std::wstring subtitle;
    std::wstring icon;
    std::wstring pluginName;
    std::wstring clipboardText;
    std::wstring path;
    bool isDirectory = false;
    bool copyToClipboard = false;
    bool closeAfterExecute = true;
};

class FileSearchService {
public:
    FileSearchService(SettingsService& settings, NtfsService& ntfs, HistoryService& history);

    // Runs the file-search query pipeline (fs / ? / plain) and appends results.
    void query(const std::wstring& raw, int maxResults, std::vector<ResultItem>& out,
               const std::atomic<bool>* cancel = nullptr);

    // "history" / "hist" / "!!" support (last 30 entries).
    void queryHistory(const std::wstring& raw, std::vector<ResultItem>& out);

private:
    void searchIndex(const std::wstring& search, int max, std::vector<ResultItem>& out,
                     const std::atomic<bool>* cancel);
    void liveSearch(const std::wstring& search, int max, std::vector<ResultItem>& out,
                    const std::atomic<bool>* cancel);
    void legacySearch(const std::wstring& search, int max, std::vector<ResultItem>& out,
                      const std::atomic<bool>* cancel);

    static bool startsWithCommandPrefix(const std::wstring& raw);
    static std::vector<std::wstring> searchRoots();
    static std::vector<std::wstring> commonSearchRoots();

    SettingsService& settings_;
    NtfsService& ntfs_;
    HistoryService& history_;
};
