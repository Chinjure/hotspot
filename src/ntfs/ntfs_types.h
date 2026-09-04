#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
        for (const auto& d : records) {
            uint64_t key = d.ref & 0xFFFFFFFFFFFFULL;
            if (d.op == DeltaOp::Delete) {
                if (d.isDirectory) state.deletedDirectories.insert(key);
                else state.deletedFiles.insert(key);
                continue;
            }
            if (d.op == DeltaOp::Add) {
                state.added.push_back(d);
                if (d.isDirectory) state.dirOverlay[key] = d;
                continue;
            }
            if (d.isDirectory) state.dirOverlay[key] = d;
            else state.fileOverlay[key] = d;
        }
        return state;
    }
};

// Binary constants for index.dat (little-endian).
namespace ntfs_index_format {
inline constexpr int kHeaderSize = 64;
inline constexpr int kRecordSize = 40;
inline constexpr long long kVersion = 1;
} // namespace ntfs_index_format
