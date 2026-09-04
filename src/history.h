#pragma once

#include <string>
#include <vector>

struct HistoryEntry {
    std::wstring query;
    std::wstring title;
    std::wstring pluginName;
    std::wstring usedAt; // ISO-8601 local text, compatible with System.Text.Json
};

class HistoryService {
public:
    HistoryService();

    std::vector<HistoryEntry> entries;
    std::wstring path() const;
    void add(const std::wstring& query, const std::wstring& title, const std::wstring& pluginName);
    void load();
    void save();

private:
    std::wstring path_;
};
