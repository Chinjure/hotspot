#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "file_rank.h"

// One logical file-system object produced by a scan or USN update.
struct NtfsEntry {
    uint64_t ref = 0;
    uint64_t parentRef = 0;
    uint64_t size = 0;
    uint64_t modifiedTime = 0;
    bool isDirectory = false;
    std::wstring name;
};

// Volume-level data collected before a scan.
struct NtfsVolumeInfo {
    wchar_t drive = 0;
    uint32_t volumeSerial = 0;
    uint64_t bytesPerRecord = 1024;
    uint64_t journalId = 0;
    int64_t firstUsn = 0;
    int64_t nextUsn = 0;
    uint64_t journalMaxSize = 0;
};

// A search hit from the NTFS index.
struct NtfsSearchResult {
    std::wstring name;
    std::wstring path;
    bool isDirectory = false;
    // Match quality (0 = exact name, 1 = exact stem, 2 = prefix, 3 = contains).
    int matchTier = 3;
    // Executable / shortcut / file / directory, used to rank launchable hits first.
    file_rank::Kind kind = file_rank::Kind::File;
    // Cached file_rank::isLaunchable(kind) so sorts stay cheap.
    bool launchable = false;
    uint64_t size = 0;
    uint64_t modifiedTime = 0;
    uint64_t ref = 0;
};

// Persisted metadata for one volume index (X-meta.json).
struct NtfsIndexMeta {
    int version = 1;
    wchar_t drive = 0;
    uint32_t volumeSerial = 0;
    int64_t epoch = 0;
    int64_t fileCount = 0;
    int64_t directoryCount = 0;
    int64_t lastScanTimeUtcTicks = 0;
    uint64_t rootRef = 5;
    uint32_t bytesPerRecord = 0;
    int source = 0;      // 0 = $MFT, 1 = walk
    uint64_t journalId = 0;
    int64_t nextUsn = 0;
};

// USN delta operations (mirrors the C# version).
enum class DeltaOp : uint8_t {
    Add = 1,
    Delete = 2,
    Rename = 3,
    Update = 4,
};

struct DeltaRecord {
    DeltaOp op = DeltaOp::Add;
    bool isDirectory = false;
    uint64_t ref = 0;
    uint64_t parentRef = 0;
    uint64_t size = 0;
    uint64_t modifiedTime = 0;
    std::wstring name;
};

struct DeltaData {
    int64_t epoch = 0;
    std::vector<DeltaRecord> records;
};

// In-memory view of a delta file: overlays applied on top of a base index.
struct DeltaState {
    std::unordered_map<uint64_t, DeltaRecord> fileOverlay;
    std::unordered_map<uint64_t, DeltaRecord> dirOverlay;
    std::unordered_set<uint64_t> deletedFiles;
    std::unordered_set<uint64_t> deletedDirectories;
    std::vector<DeltaRecord> added;

    static DeltaState build(const std::vector<DeltaRecord>& records) {
        DeltaState state;
        // Two facts about the journal drive this fold:
        //  - records arrive in USN order, so the last record for a file id wins;
        //  - NTFS reuses file ids, and a close/update record carries no name.
        // A naive "later record wins" fold therefore renames a brand new file to
        // whatever last used its id (and a stale delete hides it entirely), so
        // names are only ever overwritten by a record that actually has one.
        for (const auto& d : records) {
            uint64_t key = d.ref & 0xFFFFFFFFFFFFULL;
            bool deleted = d.isDirectory ? state.deletedDirectories.count(key) != 0
                                         : state.deletedFiles.count(key) != 0;
            if (d.op == DeltaOp::Delete) {
                if (d.isDirectory) {
                    state.deletedDirectories.insert(key);
                    state.dirOverlay.erase(key);
                } else {
                    state.deletedFiles.insert(key);
                    state.fileOverlay.erase(key);
                }
                if (deleted) {
                    // The id is free again; a later create owns it from scratch.
                    state.added.erase(std::remove_if(state.added.begin(), state.added.end(),
                                                     [key](const DeltaRecord& r) {
                                                         return (r.ref & 0xFFFFFFFFFFFFULL) == key;
                                                     }),
                                      state.added.end());
                }
                continue;
            }

            if (!deleted) {
                if (d.isDirectory) state.deletedDirectories.erase(key);
                else state.deletedFiles.erase(key);
            }

            auto& overlay = d.isDirectory ? state.dirOverlay[key] : state.fileOverlay[key];
            bool isNew = overlay.ref == 0;
            if (isNew || !d.name.empty()) overlay.name = d.name;
            if (isNew || d.parentRef != 0) overlay.parentRef = d.parentRef;
            overlay.modifiedTime = d.modifiedTime > 0 ? d.modifiedTime : overlay.modifiedTime;
            overlay.ref = d.ref;
            overlay.isDirectory = d.isDirectory;
            overlay.op = d.op;
            if (deleted) {
                // Update for an id whose file is gone: keep the tombstone.
                continue;
            }
            if (d.op == DeltaOp::Add) {
                // Re-point the entry that may already exist for this id, so a
                // recreated file is listed once, under its current name.
                bool replaced = false;
                for (auto& existing : state.added) {
                    if ((existing.ref & 0xFFFFFFFFFFFFULL) == key) {
                        existing = d;
                        if (overlay.name.empty()) existing.name = overlay.name;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) state.added.push_back(d);
            }
        }
        // Names resolve through the overlay, so every listed entry needs one.
        state.added.erase(std::remove_if(state.added.begin(), state.added.end(),
                                         [&state](const DeltaRecord& r) {
                                             uint64_t key = r.ref & 0xFFFFFFFFFFFFULL;
                                             const auto& overlay = r.isDirectory
                                                                       ? state.dirOverlay[key]
                                                                       : state.fileOverlay[key];
                                             return r.name.empty() && overlay.name.empty();
                                         }),
                          state.added.end());
        return state;
    }
};

// Binary constants for index.dat (little-endian).
namespace ntfs_index_format {
inline constexpr int kHeaderSize = 64;
inline constexpr int kRecordSize = 40;
inline constexpr long long kVersion = 1;
} // namespace ntfs_index_format
