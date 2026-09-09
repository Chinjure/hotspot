#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ntfs_index.h"
#include "ntfs_types.h"
#include "path_query.h"

struct NtfsDriveState {
    wchar_t drive = 0;
    bool ready = false;
    bool building = false;
    bool failed = false;
    int64_t processed = 0;
    int64_t recordCount = 0;
    std::wstring source = L"-";
    std::wstring message;
    int64_t lastScanTimeUtcTicks = 0;
    bool hasLastScan = false;
};

struct NtfsServiceStatus {
    bool anyReady = false;
    int readyCount = 0;
    int buildingCount = 0;
    int failedCount = 0;
    int64_t totalRecords = 0;
    std::wstring summary;
};

class NtfsService {
public:
    NtfsService();
    ~NtfsService();

    void initialize();
    void startRebuildAll();
    bool rebuildDriveAndWait(wchar_t drive, std::vector<std::wstring>* log = nullptr);

    std::vector<NtfsDriveState> driveStates();
    NtfsServiceStatus status();
    // Name search when `pathQuery` is null, path search otherwise (the query's
    // segments are matched against every volume's reconstructed paths).
    std::vector<NtfsSearchResult> search(const std::wstring& needle, int maxResults,
                                         const std::atomic<bool>* cancel = nullptr,
                                         const path_query::Query* pathQuery = nullptr);

    std::wstring indexDirectory() const { return indexDirectory_; }
    void dispose();

private:
    void buildDriveCore(wchar_t drive);
    bool tryLoadIndex(wchar_t drive, std::unique_ptr<NtfsIndex>& index, NtfsIndexMeta& meta);
    void tryUpdateDrive(wchar_t drive);
    void tryUpdateAll();
    void cleanupTemporaryFiles();

    std::wstring metaPath(wchar_t drive) const;
    std::wstring indexPath(wchar_t drive) const;
    void saveMeta(const std::wstring& path, const NtfsIndexMeta& meta);
    bool loadMeta(const std::wstring& path, NtfsIndexMeta& meta);

    std::wstring indexDirectory_;
    std::mutex gate_;
    std::unordered_map<wchar_t, std::unique_ptr<NtfsIndex>> indexes_;
    std::unordered_map<wchar_t, NtfsIndexMeta> metas_;
    std::unordered_map<wchar_t, NtfsDriveState> states_;
};
