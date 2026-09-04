#pragma once

#include <cstdint>
#include <fstream>
#include <string>

#include "ntfs_types.h"

class NtfsIndexWriter {
public:
    explicit NtfsIndexWriter(const std::wstring& directory, const std::wstring& tempPrefix = L"index");
    ~NtfsIndexWriter();

    NtfsIndexWriter(const NtfsIndexWriter&) = delete;
    NtfsIndexWriter& operator=(const NtfsIndexWriter&) = delete;

    void append(const NtfsEntry& entry);
    bool commit(const NtfsIndexMeta& meta, const std::wstring& indexPath);
    int64_t recordCount() const { return fileCount_ + dirCount_; }
    int64_t fileCount() const { return fileCount_; }
    int64_t dirCount() const { return dirCount_; }

private:
    void cleanup();

    std::wstring directory_;
    std::wstring tempPrefix_;
    std::ofstream recordsFile_;
    std::ofstream namesFile_;
    int64_t fileCount_ = 0;
    int64_t dirCount_ = 0;
    uint64_t namePosition_ = 0;
    bool opened_ = false;
    bool committed_ = false;
};
