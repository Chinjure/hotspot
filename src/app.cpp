#include "app.h"

#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "file_search.h"
#include "history.h"
#include "ui/main_window.h"
#include "ntfs/ntfs_service.h"
#include "ntfs/ntfs_win32.h"
#include "path_query.h"
#include "settings.h"
#include "tray.h"
#include "winutil.h"

namespace {

std::vector<std::wstring> parseArgs(const std::wstring& commandLine) {
    std::vector<std::wstring> args;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &argc);
    if (!argv) return args;
    for (int i = 1; i < argc; i++) args.emplace_back(argv[i]);
    LocalFree(argv);
    return args;
}

bool hasArg(const std::vector<std::wstring>& args, const std::wstring& name) {
    for (const auto& a : args) {
        if (winutil::equalsIgnoreCase(a, name)) return true;
    }
    return false;
}

std::wstring argAfter(const std::vector<std::wstring>& args, const std::wstring& name) {
    for (size_t i = 0; i + 1 < args.size(); i++) {
        if (winutil::equalsIgnoreCase(args[i], name)) return args[i + 1];
    }
    return L"";
}

void waitForReady(NtfsService& ntfs, int maxSeconds = 120) {
    for (int i = 0; i < maxSeconds * 2; i++) {
        if (ntfs.status().anyReady) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void writeLog(const std::wstring& path, const std::vector<std::wstring>& lines) {
    std::string content;
    for (const auto& line : lines) content += winutil::wideToUtf8(line) + "\r\n";
    winutil::writeTextFile(path, content);
}

std::wstring kindLabel(file_rank::Kind kind) {
    switch (kind) {
        case file_rank::Kind::Executable: return L"exe";
        case file_rank::Kind::Shortcut: return L"lnk";
        case file_rank::Kind::Directory: return L"dir";
        default: return L"file";
    }
}

std::wstring describeHit(const std::wstring& name, const std::wstring& path, file_rank::Kind kind,
                         int tier) {
    return name + L" | " + path + L" | " + kindLabel(kind) + L" tier=" + std::to_wstring(tier);
}

int runCli(const std::vector<std::wstring>& args) {
    bool isCli = hasArg(args, L"--index-build") || hasArg(args, L"--index-search") ||
                 hasArg(args, L"--index-status") || hasArg(args, L"--live-search");
    if (!isCli) return -1;

    SettingsService settings;
    HistoryService history;
    NtfsService ntfs;
    ntfs.initialize();

    std::wstring logDir = ntfs.indexDirectory();
    winutil::createDirectory(logDir);

    if (hasArg(args, L"--index-build")) {
        std::wstring driveArg = argAfter(args, L"--index-build");
        std::vector<wchar_t> drives;
        if (!driveArg.empty()) drives.push_back(driveArg[0]);
        else drives = ntfs_win32::getFixedNtfsDrives();
        std::vector<std::wstring> log;
        log.push_back(L"[build] " + winutil::nowIsoLocal() + L" drives=" + driveArg);
        for (wchar_t d : drives) {
            log.push_back(L"[build] 开始盘: " + std::wstring(1, d));
            bool ok = ntfs.rebuildDriveAndWait(d, &log);
            log.push_back(std::wstring(1, d) + L": " + (ok ? L"OK" : L"FAILED"));
            log.push_back(L"[build] status: " + ntfs.status().summary);
        }
        writeLog(winutil::joinPath(logDir, L"build.log"), log);
        return 0;
    }

    if (hasArg(args, L"--index-search")) {
        std::wstring q = argAfter(args, L"--index-search");
        waitForReady(ntfs);
        auto status = ntfs.status();
        path_query::Query pathQuery;
        const path_query::Query* pathPtr = nullptr;
        if (path_query::looksLikePath(q)) {
            pathQuery = path_query::parse(q);
            if (pathQuery.valid) pathPtr = &pathQuery;
        }
        auto hits = ntfs.search(q, 30, nullptr, pathPtr);
        std::vector<std::wstring> log;
        log.push_back(L"[search] " + winutil::nowIsoLocal() + L" query=" + q +
                      (pathPtr ? L" (path mode)" : L" (name mode)"));
        log.push_back(L"[search] status: " + status.summary);
        log.push_back(L"[search] results=" + std::to_wstring(hits.size()));
        for (const auto& h : hits) {
            log.push_back(describeHit(h.name, h.path, h.kind, h.matchTier));
        }
        writeLog(winutil::joinPath(logDir, L"search.log"), log);
        return 0;
    }

    if (hasArg(args, L"--index-status")) {
        waitForReady(ntfs);
        auto status = ntfs.status();
        std::vector<std::wstring> log;
        log.push_back(L"[status] " + winutil::nowIsoLocal());
        log.push_back(status.summary);
        for (const auto& s : ntfs.driveStates()) {
            log.push_back(std::wstring(1, s.drive) + L": ready=" + (s.ready ? L"1" : L"0") +
                          L" building=" + (s.building ? L"1" : L"0") +
                          L" failed=" + (s.failed ? L"1" : L"0") +
                          L" count=" + std::to_wstring(s.recordCount) +
                          L" source=" + s.source + L" message=" + s.message);
        }
        writeLog(winutil::joinPath(logDir, L"status.log"), log);
        return 0;
    }

    if (hasArg(args, L"--live-search")) {
        std::wstring q = argAfter(args, L"--live-search");
        waitForReady(ntfs);
        FileSearchService fileSearch(settings, ntfs, history);
        std::vector<ResultItem> results;
        fileSearch.query(L"fs " + q, 30, results, nullptr);
        auto status = ntfs.status();
        std::vector<std::wstring> log;
        log.push_back(L"[live] " + winutil::nowIsoLocal() + L" query=" + q + L" results=" +
                      std::to_wstring(results.size()));
        log.push_back(L"[live] status: " + status.summary);
        for (size_t i = 0; i < results.size() && i < 50; i++) {
            log.push_back(describeHit(results[i].title, results[i].subtitle, results[i].kind,
                                      results[i].matchTier));
        }
        writeLog(winutil::joinPath(logDir, L"live-search.log"), log);
        return 0;
    }

    return -1; // not a CLI mode
}

} // namespace

int appMain(HINSTANCE instance, const std::wstring& commandLine, int showCmd) {
    (void)showCmd;
    // Shell icons (SHGetImageList) and WIC both need COM on this thread.
    HRESULT oleHr = OleInitialize(nullptr);
    std::vector<std::wstring> args = parseArgs(commandLine);
    winutil::debugLog(L"appMain start, args=" + (args.empty() ? L"(none)" : args[0]));

    int cliResult = runCli(args);
    if (cliResult >= 0) {
        if (SUCCEEDED(oleHr)) OleUninitialize();
        return cliResult;
    }

    SettingsService settings;
    HistoryService history;
    NtfsService ntfs;
    ntfs.initialize();
    FileSearchService fileSearch(settings, ntfs, history);
    winutil::debugLog(L"services created");

    MainWindow main(instance, settings, history, fileSearch, ntfs);
    winutil::debugLog(L"before main.create");
    if (!main.create()) {
        winutil::debugLog(L"main.create failed");
        return 1;
    }
    winutil::debugLog(L"main created");
    main.setAutoHide(!hasArg(args, L"--stay"));
    if (hasArg(args, L"--settings")) main.openSettingsWindow();

    if (settings.current.showOnStartup || hasArg(args, L"--stay")) {
        main.showLauncher();
    } else {
        main.hide();
    }
    winutil::debugLog(L"main shown/hidden");

    TrayIcon tray(main.hwnd(), instance,
                  [&]() { main.showLauncher(); },
                  [&]() { main.openSettingsWindow(); },
                  [&]() { PostQuitMessage(0); });
    tray.add();
    main.setTray(&tray);
    winutil::debugLog(L"tray added");

    MSG msg{};
    winutil::debugLog(L"entering message loop");
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    winutil::debugLog(L"message loop exited");
    main.setTray(nullptr);
    tray.remove();
    ntfs.dispose();
    if (SUCCEEDED(oleHr)) OleUninitialize();
    return 0;
}
