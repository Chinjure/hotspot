#pragma once

#include <string>
#include <vector>

struct HistoryEntry {
    std::wstring query;
    std::wstring title;
    std::wstring pluginName;
    std::wstring usedAt; // ISO-8601 local text, compatible with System.Text.Json
    // Optional: file/folder path of the executed result. Lets the history view
    // show the real shell icon instead of a generic glyph. Older history.json
    // files simply have no Path field.
    std::wstring path;
};

class HistoryService {
public:
    HistoryService();

    std::vector<HistoryEntry> entries;
    std::wstring path() const;
    void add(const std::wstring& query, const std::wstring& title, const std::wstring& pluginName,
             const std::wstring& resultPath = L"");
    void load();
    void save();

private:
    std::wstring path_;
};
