#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ntfs_types.h"

class NtfsIndex {
public:
    static std::unique_ptr<NtfsIndex> tryLoad(const std::wstring& indexPath,
                                              const NtfsIndexMeta& meta,
                                              const DeltaData& delta);
    ~NtfsIndex();

    NtfsIndex(const NtfsIndex&) = delete;
    NtfsIndex& operator=(const NtfsIndex&) = delete;

    int64_t recordCount() const { return recordCount_; }

    std::vector<NtfsSearchResult> search(const std::wstring& needle, int maxResults,
                                         const std::atomic<bool>* cancel = nullptr);

private:
    struct DirEntry {
        uint64_t parentRef = 0;
        uint32_t nameOffset = 0;
        uint16_t nameLenBytes = 0;
        std::wstring overlayName;
        bool hasOverlay = false;
    };

    NtfsIndex(const std::wstring& indexPath, const NtfsIndexMeta& meta, const DeltaData& delta);

    bool tryReadHeader(size_t mapSize);
    void buildDirectoryMap();
    std::wstring dirName(const DirEntry& dir);
    bool tryGetDir(uint64_t key, DirEntry& out);
    std::wstring resolvePath(uint64_t refValue, int depth);
    std::wstring resolvePath(uint64_t refValue);

    HANDLE fileHandle_ = INVALID_HANDLE_VALUE;
    HANDLE mappingHandle_ = nullptr;
    uint8_t* base_ = nullptr;
    size_t mapSize_ = 0;

    NtfsIndexMeta meta_;
    int64_t recordCount_ = 0;
    int64_t dirCount_ = 0;
    uint64_t nameSectionOffset_ = 0;
    std::wstring driveRoot_;
    uint64_t rootRef_ = 5;

    std::unordered_map<uint64_t, DirEntry> dirs_;
    std::unordered_map<uint64_t, std::wstring> pathMemo_;
    std::mutex pathMutex_;
    std::unique_ptr<DeltaState> delta_;
};
