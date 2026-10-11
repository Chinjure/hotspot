#include "settings.h"

#include <windows.h>
#include <objbase.h>
#include <taskschd.h>
#include <comdef.h>

// CLSID_TaskScheduler / IID_ITaskService / IID_IExecAction live in taskschd.lib,
// which the shared link line does not pull in.
#pragma comment(lib, "taskschd.lib")

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

// The launcher requires elevation, and a HKCU Run entry cannot start an
// elevated process without a UAC prompt on every logon. A scheduled task with
// RunLevel=Highest registers as an elevated logon start instead, so autostart
// stays silent. The Run key is kept only as a fallback for the (administrator
// free) case where the task cannot be registered at all.
namespace {

constexpr wchar_t kTaskName[] = L"hotspot";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"hotspot";

bool runValueExists() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    bool exists = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

// The UI thread already initialized COM (OleInitialize); S_OK / S_FALSE mean we
// own the uninitialize, RPC_E_CHANGED_MODE means someone else does.
bool comReady(HRESULT hr) {
    return hr == S_OK || hr == S_FALSE || hr == RPC_E_CHANGED_MODE;
}

// Paths with & or < would break the task XML, so escape them properly.
std::wstring xmlEscape(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        switch (ch) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            default: out += ch; break;
        }
    }
    return out;
}

std::wstring taskXml(const std::wstring& exePath) {
    const std::wstring command = xmlEscape(exePath);
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
        L"  <RegistrationInfo>\n"
        L"    <Description>hotspot 启动器（最高权限，用于 NTFS 索引增量更新）</Description>\n"
        L"  </RegistrationInfo>\n"
        L"  <Triggers>\n"
        L"    <LogonTrigger>\n"
        L"      <Enabled>true</Enabled>\n"
        L"    </LogonTrigger>\n"
        L"  </Triggers>\n"
        L"  <Principals>\n"
        L"    <Principal id=\"Author\">\n"
        L"      <LogonType>InteractiveToken</LogonType>\n"
        L"      <RunLevel>HighestAvailable</RunLevel>\n"
        L"    </Principal>\n"
        L"  </Principals>\n"
        L"  <Settings>\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
        L"    <AllowHardTerminate>false</AllowHardTerminate>\n"
        L"    <StartWhenAvailable>true</StartWhenAvailable>\n"
        L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n"
        L"    <IdleSettings>\n"
        L"      <StopOnIdleEnd>false</StopOnIdleEnd>\n"
        L"      <RestartOnIdle>false</RestartOnIdle>\n"
        L"    </IdleSettings>\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\n"
        L"    <Enabled>true</Enabled>\n"
        L"    <Hidden>false</Hidden>\n"
        L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n"
        L"    <WakeToRun>false</WakeToRun>\n"
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
        L"    <Priority>7</Priority>\n"
        L"  </Settings>\n"
        L"  <Actions Context=\"Author\">\n"
        L"    <Exec>\n"
        L"      <Command>" + command + L"</Command>\n"
        L"    </Exec>\n"
        L"  </Actions>\n"
        L"</Task>\n";
    return xml;
}

bool removeScheduledTask() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!comReady(hr)) return false;
    bool ok = false;
    ITaskService* service = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITaskService, reinterpret_cast<void**>(&service)))) {
        VARIANT empty;
        VariantInit(&empty);
        if (SUCCEEDED(service->Connect(empty, empty, empty, empty))) {
            ITaskFolder* root = nullptr;
            BSTR rootPath = SysAllocString(L"\\");
            if (SUCCEEDED(service->GetFolder(rootPath, &root)) && root) {
                BSTR name = SysAllocString(kTaskName);
                // S_OK when deleted, failure when it was not there: both mean
                // "no task left behind".
                ok = root->DeleteTask(name, 0) == S_OK;
                SysFreeString(name);
                root->Release();
            }
            SysFreeString(rootPath);
        }
        service->Release();
    }
    if (hr == S_OK || hr == S_FALSE) CoUninitialize();
    return ok;
}

bool registerScheduledTask(const std::wstring& exePath) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!comReady(hr)) return false;
    bool ok = false;
    ITaskService* service = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITaskService, reinterpret_cast<void**>(&service)))) {
        VARIANT empty;
        VariantInit(&empty);
        if (SUCCEEDED(service->Connect(empty, empty, empty, empty))) {
            ITaskDefinition* task = nullptr;
            if (SUCCEEDED(service->NewTask(0, &task)) && task) {
                IRegistrationInfo* info = nullptr;
                if (SUCCEEDED(task->get_RegistrationInfo(&info)) && info) {
                    BSTR author = SysAllocString(L"hotspot");
                    info->put_Author(author);
                    SysFreeString(author);
                    info->Release();
                }
                IPrincipal* principal = nullptr;
                if (SUCCEEDED(task->get_Principal(&principal)) && principal) {
                    principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
                    principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
                    principal->Release();
                }
                ITaskSettings* settings = nullptr;
                if (SUCCEEDED(task->get_Settings(&settings)) && settings) {
                    settings->put_StartWhenAvailable(VARIANT_TRUE);
                    settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
                    settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
                    settings->put_AllowDemandStart(VARIANT_TRUE);
                    // -1 == "no time limit" (PT0S in XML).
                    settings->put_ExecutionTimeLimit(SysAllocString(L"PT0S"));
                    settings->Release();
                }
                ITriggerCollection* triggers = nullptr;
                if (SUCCEEDED(task->get_Triggers(&triggers)) && triggers) {
                    ITrigger* trigger = nullptr;
                    if (SUCCEEDED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) && trigger) {
                        trigger->Release();
                    }
                    triggers->Release();
                }
                IActionCollection* actions = nullptr;
                if (SUCCEEDED(task->get_Actions(&actions)) && actions) {
                    IAction* action = nullptr;
                    if (SUCCEEDED(actions->Create(TASK_ACTION_EXEC, &action)) && action) {
                        IExecAction* exec = nullptr;
                        if (SUCCEEDED(action->QueryInterface(IID_IExecAction,
                                                             reinterpret_cast<void**>(&exec))) && exec) {
                            BSTR command = SysAllocString(exePath.c_str());
                            exec->put_Path(command);
                            SysFreeString(command);
                            exec->Release();
                        }
                        action->Release();
                    }
                    actions->Release();
                }
                ITaskFolder* root = nullptr;
                BSTR rootPath = SysAllocString(L"\\");
                if (SUCCEEDED(service->GetFolder(rootPath, &root)) && root) {
                    IRegisteredTask* registered = nullptr;
                    VARIANT empty2;
                    VariantInit(&empty2);
                    BSTR name = SysAllocString(kTaskName);
                    BSTR xml = SysAllocString(taskXml(exePath).c_str());
                    ok = SUCCEEDED(root->RegisterTaskDefinition(
                        name, task, TASK_CREATE_OR_UPDATE,
                        empty2, empty2, TASK_LOGON_INTERACTIVE_TOKEN, empty2, &registered));
                    if (ok && registered) registered->Release();
                    SysFreeString(xml);
                    SysFreeString(name);
                    root->Release();
                }
                SysFreeString(rootPath);
                task->Release();
            }
        }
        service->Release();
    }
    if (hr == S_OK || hr == S_FALSE) CoUninitialize();
    return ok;
}

bool writeRunValue(bool enabled, const std::wstring& exePath) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = false;
    if (enabled) {
        ok = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(exePath.c_str()),
                            static_cast<DWORD>((exePath.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        ok = RegDeleteValueW(key, kRunValue) == ERROR_SUCCESS ||
             RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return ok;
}

} // namespace

bool autostartEnabled() {
    if (runValueExists()) return true;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!comReady(hr)) return false;
    bool exists = false;
    ITaskService* service = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITaskService, reinterpret_cast<void**>(&service)))) {
        VARIANT empty;
        VariantInit(&empty);
        if (SUCCEEDED(service->Connect(empty, empty, empty, empty))) {
            ITaskFolder* root = nullptr;
            BSTR rootPath = SysAllocString(L"\\");
            if (SUCCEEDED(service->GetFolder(rootPath, &root)) && root) {
                BSTR name = SysAllocString(kTaskName);
                IRegisteredTask* task = nullptr;
                exists = SUCCEEDED(root->GetTask(name, &task)) && task != nullptr;
                if (task) task->Release();
                SysFreeString(name);
                root->Release();
            }
            SysFreeString(rootPath);
        }
        service->Release();
    }
    if (hr == S_OK || hr == S_FALSE) CoUninitialize();
    return exists;
}

bool applyAutoStart(bool enabled, const std::wstring& exePath) {
    if (enabled) {
        if (registerScheduledTask(exePath)) {
            // Exactly one start path: the task replaces the Run entry, which
            // would otherwise launch a second, non-elevated instance.
            writeRunValue(false, exePath);
            return true;
        }
        // No elevation rights to register the task: fall back to the Run key.
        return writeRunValue(true, exePath);
    }
    removeScheduledTask();
    return writeRunValue(false, exePath);
}
