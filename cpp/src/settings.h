#pragma once

#include <map>
#include <string>

struct AppSettings {
    std::wstring hotkey = L"Alt+Space";
    bool autoStart = true;
    std::wstring webSearchUrl = L"https://www.bing.com/search?q={0}";
    bool showOnStartup = true;
    int fileSearchMaxResults = 200;
    bool ntfsIndexEnabled = true;
    bool ntfsLiveSearchEnabled = true;
    bool plainTextFileSearch = true;
    int ntfsLiveSearchTimeoutMs = 800;
    std::map<std::wstring, bool> plugins; // "File Search", "History", ...
};

class SettingsService {
public:
    SettingsService();

    AppSettings current;
    std::wstring configPath() const;

    bool isPluginEnabled(const std::wstring& name, bool defaultEnabled = true) const;
    void setPluginEnabled(const std::wstring& name, bool enabled);
    void save();
    void load();

private:
    std::wstring path_;
};

// Autostart (HKCU Run) helpers used by the settings window.
bool applyAutoStart(bool enabled, const std::wstring& exePath);
