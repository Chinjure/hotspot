#include "settings.h"

#include <windows.h>

#include <string>

#include "app_paths.h"
#include "json.h"
#include "winutil.h"

namespace {

json::Value toJson(const AppSettings& s) {
    json::Object plugins;
    for (const auto& [name, enabled] : s.plugins) {
        plugins[winutil::wideToUtf8(name)] = json::Value(enabled);
    }
    json::Object obj;
    obj["Hotkey"] = json::Value(winutil::wideToUtf8(s.hotkey));
    obj["AutoStart"] = json::Value(s.autoStart);
    obj["WebSearchUrl"] = json::Value(winutil::wideToUtf8(s.webSearchUrl));
    obj["ShowOnStartup"] = json::Value(s.showOnStartup);
    obj["FileSearchMaxResults"] = json::Value(s.fileSearchMaxResults);
    obj["NtfsIndexEnabled"] = json::Value(s.ntfsIndexEnabled);
    obj["NtfsLiveSearchEnabled"] = json::Value(s.ntfsLiveSearchEnabled);
    obj["PlainTextFileSearch"] = json::Value(s.plainTextFileSearch);
    obj["NtfsLiveSearchTimeoutMs"] = json::Value(s.ntfsLiveSearchTimeoutMs);
    obj["Plugins"] = json::Value(std::move(plugins));
    return json::Value(std::move(obj));
}

std::wstring strField(const json::Value& root, const char* key, const std::wstring& def) {
    const json::Value* v = root.find(key);
    if (v && v->isString()) return winutil::utf8ToWide(v->asString());
    return def;
}

bool boolField(const json::Value& root, const char* key, bool def) {
    const json::Value* v = root.find(key);
    return v && v->isBool() ? v->asBool() : def;
}

int64_t intField(const json::Value& root, const char* key, int64_t def) {
    const json::Value* v = root.find(key);
    return v ? v->asInt(def) : def;
}

} // namespace

SettingsService::SettingsService() {
    path_ = winutil::joinPath(app_paths::dataDir(), L"settings.json");
    load();
}

std::wstring SettingsService::configPath() const { return path_; }

bool SettingsService::isPluginEnabled(const std::wstring& name, bool defaultEnabled) const {
    auto it = current.plugins.find(name);
    if (it == current.plugins.end()) return defaultEnabled;
    return it->second;
}

void SettingsService::setPluginEnabled(const std::wstring& name, bool enabled) {
    current.plugins[name] = enabled;
}

void SettingsService::save() {
    try {
        std::string content = json::serialize(toJson(current));
        winutil::writeTextFile(path_, content);
    } catch (...) {
        // Read-only profile or disk failure must not crash the launcher.
    }
}

void SettingsService::load() {
    try {
        std::string content;
        if (!winutil::readTextFile(path_, content)) return; // defaults
        json::Value root = json::parse(content);
        current.hotkey = strField(root, "Hotkey", current.hotkey);
        current.autoStart = boolField(root, "AutoStart", current.autoStart);
        current.webSearchUrl = strField(root, "WebSearchUrl", current.webSearchUrl);
        current.showOnStartup = boolField(root, "ShowOnStartup", current.showOnStartup);
        current.fileSearchMaxResults = static_cast<int>(intField(root, "FileSearchMaxResults", current.fileSearchMaxResults));
        current.ntfsIndexEnabled = boolField(root, "NtfsIndexEnabled", current.ntfsIndexEnabled);
        current.ntfsLiveSearchEnabled = boolField(root, "NtfsLiveSearchEnabled", current.ntfsLiveSearchEnabled);
        current.plainTextFileSearch = boolField(root, "PlainTextFileSearch", current.plainTextFileSearch);
        current.ntfsLiveSearchTimeoutMs = static_cast<int>(intField(root, "NtfsLiveSearchTimeoutMs", current.ntfsLiveSearchTimeoutMs));

        const json::Value* plugins = root.find("Plugins");
        if (plugins && plugins->isObject()) {
            current.plugins.clear();
            for (const auto& [key, val] : plugins->asObject()) {
                if (val.isBool()) current.plugins[winutil::utf8ToWide(key)] = val.asBool();
            }
        }
    } catch (...) {
        // Corrupt settings: keep defaults.
    }
}

bool applyAutoStart(bool enabled, const std::wstring& exePath) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = false;
    if (enabled) {
        ok = RegSetValueExW(key, L"hotspot", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(exePath.c_str()),
                            static_cast<DWORD>((exePath.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        ok = RegDeleteValueW(key, L"hotspot") == ERROR_SUCCESS ||
             RegQueryValueExW(key, L"hotspot", nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return ok;
}
