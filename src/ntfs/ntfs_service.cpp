#include "ntfs_service.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <thread>

#include "app_paths.h"
#include "json.h"
#include "ntfs_index_writer.h"
#include "ntfs_scanner.h"
#include "ntfs_win32.h"
#include "usn_updater.h"
#include "winutil.h"

namespace {

int64_t utcNowTicks() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    uint64_t fileTime = uint64_t(ft.dwHighDateTime) << 32 | uint64_t(ft.dwLowDateTime);
    return static_cast<int64_t>(fileTime + 116444736000000000ULL);
}

json::Value metaToJson(const NtfsIndexMeta& m) {
    json::Object obj;
    obj["Version"] = json::Value(m.version);
    obj["Drive"] = json::Value(std::string(1, static_cast<char>(m.drive)));
    obj["VolumeSerial"] = json::Value(static_cast<int64_t>(m.volumeSerial));
    obj["Epoch"] = json::Value(m.epoch);
    obj["FileCount"] = json::Value(m.fileCount);
    obj["DirectoryCount"] = json::Value(m.directoryCount);
    obj["LastScanTimeUtcTicks"] = json::Value(m.lastScanTimeUtcTicks);
    obj["RootRef"] = json::Value(static_cast<int64_t>(m.rootRef));
    obj["BytesPerRecord"] = json::Value(static_cast<int64_t>(m.bytesPerRecord));
    obj["Source"] = json::Value(m.source);
    obj["JournalId"] = json::Value(static_cast<int64_t>(m.journalId));
    obj["NextUsn"] = json::Value(m.nextUsn);
    return json::Value(std::move(obj));
}

bool metaFromJson(const json::Value& root, NtfsIndexMeta& m) {
    if (!root.isObject()) return false;
    const json::Value* v = nullptr;
    v = root.find("Version"); if (v) m.version = static_cast<int>(v->asInt(m.version));
    v = root.find("Drive");
    if (v && v->isString()) {
        std::string d = v->asString();
        if (!d.empty()) m.drive = static_cast<wchar_t>(d[0]);
    }
    v = root.find("VolumeSerial"); if (v) m.volumeSerial = static_cast<uint32_t>(v->asInt(0));
    v = root.find("Epoch"); if (v) m.epoch = v->asInt(0);
    v = root.find("FileCount"); if (v) m.fileCount = v->asInt(0);
    v = root.find("DirectoryCount"); if (v) m.directoryCount = v->asInt(0);
    v = root.find("LastScanTimeUtcTicks"); if (v) m.lastScanTimeUtcTicks = v->asInt(0);
    v = root.find("RootRef"); if (v) m.rootRef = static_cast<uint64_t>(v->asInt(5));
    v = root.find("BytesPerRecord"); if (v) m.bytesPerRecord = static_cast<uint32_t>(v->asInt(0));
    v = root.find("Source"); if (v) m.source = static_cast<int>(v->asInt(0));
    v = root.find("JournalId"); if (v) m.journalId = static_cast<uint64_t>(v->asInt(0));
    v = root.find("NextUsn"); if (v) m.nextUsn = v->asInt(0);
    return m.drive != 0;
}

} // namespace

NtfsService::NtfsService() {
    indexDirectory_ = winutil::joinPath(app_paths::dataDir(), L"ntfs");
    winutil::createDirectory(indexDirectory_);
    cleanupTemporaryFiles();
}

NtfsService::~NtfsService() { dispose(); }

void NtfsService::dispose() {
    std::lock_guard<std::mutex> lock(gate_);
    indexes_.clear();
    metas_.clear();
    states_.clear();
}

void NtfsService::initialize() {
    std::vector<wchar_t> drives = ntfs_win32::getFixedNtfsDrives();
    for (wchar_t drive : drives) {
        std::string metaContent;
        std::unique_ptr<NtfsIndex> index;
        NtfsIndexMeta meta;
        if (tryLoadIndex(drive, index, meta)) {
            std::lock_guard<std::mutex> lock(gate_);
            indexes_[drive] = std::move(index);
            metas_[drive] = meta;
            NtfsDriveState st{};
            st.drive = drive;
            st.ready = true;
            st.recordCount = meta.fileCount + meta.directoryCount;
            st.source = meta.source == 0 ? L"$MFT" : L"walk";
            st.lastScanTimeUtcTicks = meta.lastScanTimeUtcTicks;
            st.hasLastScan = meta.lastScanTimeUtcTicks != 0;
            st.message = L"索引加载完成";
            states_[drive] = st;
        } else {
            {
                std::lock_guard<std::mutex> lock(gate_);
                NtfsDriveState st{};
                st.drive = drive;
                st.message = L"索引尚未建立";
                states_[drive] = st;
            }
            std::thread([this, drive]() { buildDriveCore(drive); }).detach();
        }
    }
    std::thread([this]() { tryUpdateAll(); }).detach();
}

void NtfsService::startRebuildAll() {
    std::vector<wchar_t> drives = ntfs_win32::getFixedNtfsDrives();
    for (wchar_t drive : drives) {
        std::thread([this, drive]() { buildDriveCore(drive); }).detach();
    }
}

bool NtfsService::rebuildDriveAndWait(wchar_t drive, std::vector<std::wstring>* log) {
    if (log) log->push_back(L"[build] 开始重建 " + std::wstring(1, drive) + L":");
    buildDriveCore(drive);
    std::lock_guard<std::mutex> lock(gate_);
    auto it = states_.find(drive);
    return it != states_.end() && it->second.ready;
}

std::wstring NtfsService::metaPath(wchar_t drive) const {
    return winutil::joinPath(indexDirectory_, std::wstring(1, drive) + L"-meta.json");
}

std::wstring NtfsService::indexPath(wchar_t drive) const {
    return winutil::joinPath(indexDirectory_, std::wstring(1, drive) + L"-index.dat");
}

void NtfsService::saveMeta(const std::wstring& path, const NtfsIndexMeta& meta) {
    try {
        winutil::writeTextFile(path, json::serialize(metaToJson(meta)));
    } catch (...) {
        // best effort
    }
}

bool NtfsService::loadMeta(const std::wstring& path, NtfsIndexMeta& meta) {
    try {
        std::string content;
        if (!winutil::readTextFile(path, content)) return false;
        return metaFromJson(json::parse(content), meta);
    } catch (...) {
        return false;
    }
}

bool NtfsService::tryLoadIndex(wchar_t drive, std::unique_ptr<NtfsIndex>& index, NtfsIndexMeta& meta) {
    std::wstring mp = metaPath(drive);
    if (!loadMeta(mp, meta)) return false;
    DeltaData delta = usn::loadDelta(indexDirectory_, drive, meta.epoch);
    auto loaded = NtfsIndex::tryLoad(indexPath(drive), meta, delta);
    if (!loaded) return false;
    index = std::move(loaded);
    return true;
}

void NtfsService::buildDriveCore(wchar_t drive) {
    {
        std::lock_guard<std::mutex> lock(gate_);
        NtfsDriveState st{};
        st.drive = drive;
        st.building = true;
        st.message = L"正在建立索引…";
        states_[drive] = st;
    }

    std::atomic<bool> cancel{false};
    NtfsIndexMeta meta{};
    meta.drive = drive;
    meta.rootRef = 5;
    meta.source = 0;
    meta.epoch = utcNowTicks();
    meta.lastScanTimeUtcTicks = utcNowTicks();

    NtfsVolumeInfo volumeInfo{};
    std::wstring volError;
    ntfs_win32::tryGetVolumeInfo(drive, volumeInfo, volError);
    meta.volumeSerial = volumeInfo.volumeSerial;
    meta.bytesPerRecord = static_cast<uint32_t>(std::min<uint64_t>(volumeInfo.bytesPerRecord, 0xFFFFFFFFULL));
    meta.journalId = volumeInfo.journalId;
    meta.nextUsn = volumeInfo.nextUsn;

    std::unique_ptr<NtfsIndexWriter> writer =
        std::make_unique<NtfsIndexWriter>(indexDirectory_, std::wstring(1, drive));
    std::wstring failure;

    auto progress = [this, drive](int64_t p) {
        std::lock_guard<std::mutex> lock(gate_);
        auto it = states_.find(drive);
        if (it != states_.end()) it->second.processed = p;
    };

    std::wstring mftError;
    ntfsscanner::scanMft(drive,
                         [&](const NtfsEntry& e) { writer->append(e); },
                         progress, cancel, mftError);

    if (!mftError.empty() || writer->recordCount() == 0) {
        writer.reset(); // deletes temp record/name files
        writer = std::make_unique<NtfsIndexWriter>(indexDirectory_, std::wstring(1, drive));
        meta.source = 1;
        meta.rootRef = ntfsscanner::tryGetRootRef(drive);
        std::wstring walkError;
        ntfsscanner::walkVolume(drive,
                                [&](const NtfsEntry& e) { writer->append(e); },
                                progress, cancel, walkError);
        if (!walkError.empty()) failure = L"walk 模式失败: " + walkError;
    }

    meta.fileCount = writer->fileCount();
    meta.directoryCount = writer->dirCount();
    meta.lastScanTimeUtcTicks = utcNowTicks();

    if (failure.empty()) {
        if (!writer->commit(meta, indexPath(drive))) {
            failure = L"索引写入失败";
        } else {
            saveMeta(metaPath(drive), meta);
        }
    }

    DeltaData delta;
    std::unique_ptr<NtfsIndex> loaded;
    if (failure.empty()) {
        delta = usn::loadDelta(indexDirectory_, drive, meta.epoch);
        loaded = NtfsIndex::tryLoad(indexPath(drive), meta, delta);
    }

    if (failure.empty() && loaded) {
        {
            std::lock_guard<std::mutex> lock(gate_);
            indexes_[drive] = std::move(loaded);
            metas_[drive] = meta;
            NtfsDriveState st{};
            st.drive = drive;
            st.ready = true;
            st.recordCount = meta.fileCount + meta.directoryCount;
            st.source = meta.source == 0 ? L"$MFT" : L"walk";
            st.lastScanTimeUtcTicks = meta.lastScanTimeUtcTicks;
            st.hasLastScan = true;
            st.message = L"索引完成: " + winutil::formatInt(st.recordCount) + L" 项 (" + st.source + L")";
            states_[drive] = st;
        }
        std::thread([this, drive]() { tryUpdateDrive(drive); }).detach();
    } else {
        std::lock_guard<std::mutex> lock(gate_);
        auto it = states_.find(drive);
        if (it != states_.end()) {
            it->second.building = false;
            it->second.failed = true;
            it->second.ready = false;
            it->second.message = failure.empty() ? L"新索引加载失败" : failure;
        }
    }
}

void NtfsService::tryUpdateDrive(wchar_t drive) {
    NtfsIndexMeta meta;
    {
        std::lock_guard<std::mutex> lock(gate_);
        auto it = metas_.find(drive);
        if (it == metas_.end() || indexes_.count(drive) == 0) return;
        meta = it->second;
    }

    std::vector<DeltaRecord> newRecords;
    int64_t nextUsn = 0;
    std::wstring error;
    if (!usn::tryUpdate(indexDirectory_, drive, meta, newRecords, nextUsn, error)) {
        std::lock_guard<std::mutex> lock(gate_);
        auto it = states_.find(drive);
        if (it != states_.end() && it->second.ready) {
            it->second.message = L"增量更新不可用: " + error;
        }
        return;
    }

    saveMeta(metaPath(drive), meta);
    DeltaData delta = usn::loadDelta(indexDirectory_, drive, meta.epoch);
    auto reloaded = NtfsIndex::tryLoad(indexPath(drive), meta, delta);
    if (reloaded) {
        std::lock_guard<std::mutex> lock(gate_);
        indexes_[drive] = std::move(reloaded);
        metas_[drive] = meta;
        auto it = states_.find(drive);
        if (it != states_.end()) {
            if (newRecords.empty()) it->second.message = L"已是最新";
            else it->second.message = L"增量更新: " + winutil::formatInt(static_cast<int64_t>(newRecords.size())) + L" 条变更";
        }
    }
}

void NtfsService::tryUpdateAll() {
    std::vector<wchar_t> drives;
    {
        std::lock_guard<std::mutex> lock(gate_);
        for (auto& [d, st] : states_) {
            if (st.ready) drives.push_back(d);
        }
    }
    for (wchar_t d : drives) tryUpdateDrive(d);
}

std::vector<NtfsDriveState> NtfsService::driveStates() {
    std::lock_guard<std::mutex> lock(gate_);
    std::vector<NtfsDriveState> out;
    for (auto& [d, st] : states_) out.push_back(st);
    std::sort(out.begin(), out.end(), [](const NtfsDriveState& a, const NtfsDriveState& b) {
        return a.drive < b.drive;
    });
    return out;
}

NtfsServiceStatus NtfsService::status() {
    std::lock_guard<std::mutex> lock(gate_);
    NtfsServiceStatus s;
    int64_t records = 0;
    for (auto& [d, index] : indexes_) records += index->recordCount();
    s.totalRecords = records;

    std::vector<std::wstring> parts;
    std::vector<NtfsDriveState> ordered;
    for (auto& [d, st] : states_) ordered.push_back(st);
    std::sort(ordered.begin(), ordered.end(), [](const NtfsDriveState& a, const NtfsDriveState& b) {
        return a.drive < b.drive;
    });
    for (const auto& st : ordered) {
        if (st.ready) {
            s.readyCount++;
            s.anyReady = true;
            parts.push_back(std::wstring(1, st.drive) + L": " + winutil::formatInt(st.recordCount) + L" 项");
        } else if (st.building) {
            s.buildingCount++;
            parts.push_back(std::wstring(1, st.drive) + L": 索引中 " + winutil::formatInt(st.processed));
        } else if (st.failed) {
            s.failedCount++;
            parts.push_back(std::wstring(1, st.drive) + L": 失败");
        } else {
            parts.push_back(std::wstring(1, st.drive) + L": 等待");
        }
    }
    if (parts.empty()) s.summary = L"未找到固定 NTFS 卷";
    else {
        for (size_t i = 0; i < parts.size(); i++) {
            if (i) s.summary += L" · ";
            s.summary += parts[i];
        }
    }
    return s;
}

std::vector<NtfsSearchResult> NtfsService::search(const std::wstring& needle, int maxResults,
                                                  const std::atomic<bool>* cancel,
                                                  const path_query::Query* pathQuery) {
    std::vector<NtfsSearchResult> results;
    std::lock_guard<std::mutex> lock(gate_);
    if (indexes_.empty()) return results;

    // An absolute path query can only hit its own volume: skip the other
    // indexes and spend the whole budget on the drive it names.
    wchar_t onlyDrive = 0;
    if (pathQuery && pathQuery->valid && pathQuery->absolute && !pathQuery->segments.empty() &&
        pathQuery->segments[0].size() == 2) {
        onlyDrive = static_cast<wchar_t>(towupper(pathQuery->segments[0][0]));
    }

    size_t driveCount = 0;
    for (const auto& [d, index] : indexes_) {
        if (onlyDrive && static_cast<wchar_t>(towupper(d)) != onlyDrive) continue;
        driveCount++;
    }
    if (driveCount == 0) return results;

    int perDrive = std::max(1, static_cast<int>(std::ceil(static_cast<double>(maxResults) / driveCount)));
    for (auto& [d, index] : indexes_) {
        if (cancel && cancel->load()) break;
        if (onlyDrive && static_cast<wchar_t>(towupper(d)) != onlyDrive) continue;
        auto found = index->search(needle, perDrive, cancel, pathQuery);
        results.insert(results.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    }
    // Per-drive results are already ranked internally, but the concatenation is
    // not: re-rank the merged set before cutting it to maxResults so a drive's
    // low-quality tail can never push a better hit from another drive out.
    if (results.size() > 1) {
        bool pathMode = pathQuery != nullptr && pathQuery->valid;
        std::sort(results.begin(), results.end(),
                  [&needle, pathMode](const NtfsSearchResult& a, const NtfsSearchResult& b) {
                      if (pathMode) {
                          return file_rank::betterPathMatch(a.name, a.isDirectory, a.matchTier,
                                                            b.name, b.isDirectory, b.matchTier);
                      }
                      if (a.launchable != b.launchable) return a.launchable;
                      if (a.matchTier != b.matchTier) return a.matchTier < b.matchTier;
                      if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
                      if (a.name.size() != b.name.size()) return a.name.size() < b.name.size();
                      return winutil::lessIgnoreCase(a.name, b.name);
                  });
    }
    if (static_cast<int>(results.size()) > maxResults) results.resize(static_cast<size_t>(maxResults));
    return results;
}

void NtfsService::cleanupTemporaryFiles() {
    std::wstring pattern = indexDirectory_ + L"\\*.tmp";
    WIN32_FIND_DATAW data{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DeleteFileW(winutil::joinPath(indexDirectory_, data.cFileName).c_str());
    } while (FindNextFileW(h, &data));
    FindClose(h);
}
