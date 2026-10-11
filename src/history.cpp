#include "history.h"

#include "app_paths.h"
#include "json.h"
#include "winutil.h"

HistoryService::HistoryService() {
    path_ = winutil::joinPath(app_paths::dataDir(), L"history.json");
    load();
}

std::wstring HistoryService::path() const { return path_; }

void HistoryService::add(const std::wstring& query, const std::wstring& title, const std::wstring& pluginName,
                         const std::wstring& resultPath) {
    for (auto it = entries.begin(); it != entries.end();) {
        if (winutil::equalsIgnoreCase(it->title, title) && winutil::equalsIgnoreCase(it->pluginName, pluginName)) {
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
    HistoryEntry e;
    e.query = query;
    e.title = title;
    e.pluginName = pluginName;
    e.usedAt = winutil::nowIsoLocal();
    e.path = resultPath;
    entries.insert(entries.begin(), std::move(e));
    if (entries.size() > 50) entries.resize(50);
    save();
}

void HistoryService::load() {
    try {
        std::string content;
        if (!winutil::readTextFile(path_, content)) return;
        json::Value root = json::parse(content);
        if (!root.isArray()) return;
        entries.clear();
        for (const auto& v : root.asArray()) {
            if (!v.isObject()) continue;
            HistoryEntry e;
            const json::Value* query = v.find("Query");
            const json::Value* title = v.find("Title");
            const json::Value* plugin = v.find("PluginName");
            const json::Value* used = v.find("UsedAt");
            const json::Value* entryPath = v.find("Path");
            if (query && query->isString()) e.query = winutil::utf8ToWide(query->asString());
            if (title && title->isString()) e.title = winutil::utf8ToWide(title->asString());
            if (plugin && plugin->isString()) e.pluginName = winutil::utf8ToWide(plugin->asString());
            if (used && used->isString()) e.usedAt = winutil::utf8ToWide(used->asString());
            if (entryPath && entryPath->isString()) e.path = winutil::utf8ToWide(entryPath->asString());
            entries.push_back(std::move(e));
        }
    } catch (...) {
        // Ignore corrupt history.
    }
}

void HistoryService::save() {
    try {
        json::Array arr;
        for (const auto& e : entries) {
            json::Object obj;
            obj["Query"] = json::Value(winutil::wideToUtf8(e.query));
            obj["Title"] = json::Value(winutil::wideToUtf8(e.title));
            obj["PluginName"] = json::Value(winutil::wideToUtf8(e.pluginName));
            obj["UsedAt"] = json::Value(winutil::wideToUtf8(e.usedAt));
            if (!e.path.empty()) obj["Path"] = json::Value(winutil::wideToUtf8(e.path));
            arr.push_back(json::Value(std::move(obj)));
        }
        winutil::writeTextFile(path_, json::serialize(json::Value(std::move(arr))));
    } catch (...) {
        // Ignore write failures.
    }
}
