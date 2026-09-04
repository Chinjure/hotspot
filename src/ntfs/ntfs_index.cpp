#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "ntfs_index.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

#include "winutil.h"

namespace {

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (uint16_t(p[1]) << 8)); }
uint32_t readU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t readU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= uint64_t(p[i]) << (8 * i);
    return v;
}

// One-time invariant-locale lowercase table for 16-bit code units. The record
// scan in NtfsIndex::search compares names directly inside the memory-mapped
// name section with this table, so a std::wstring is only allocated when a
// record actually matches (the C# version does the same with spans).
const uint16_t* caseFoldTable() {
    static const std::array<uint16_t, 0x10000> table = [] {
        std::array<uint16_t, 0x10000> t{};
        for (uint32_t i = 0; i < 0x10000; i++) t[i] = static_cast<uint16_t>(i);
        for (uint32_t i = 0; i < 0x10000; i++) {
            wchar_t src = static_cast<wchar_t>(i);
            wchar_t out[2] = {};
            int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, &src, 1, out, 2,
                                  nullptr, nullptr, 0);
            if (n == 1) t[i] = static_cast<uint16_t>(out[0]);
        }
        return t;
    }();
    return table.data();
}

inline uint16_t foldChar(wchar_t c) { return caseFoldTable()[static_cast<uint16_t>(c)]; }

// Case-insensitive "contains" against memory-mapped name bytes, allocation-free.
// `folded` must already be lowercased with foldChar.
bool containsFolded(const wchar_t* hay, size_t hayLen, const wchar_t* folded, size_t needleLen) {
    if (needleLen == 0) return true;
    if (hayLen < needleLen) return false;
    const uint16_t* table = caseFoldTable();
    const uint16_t first = static_cast<uint16_t>(folded[0]);
    const size_t last = hayLen - needleLen;
    for (size_t i = 0; i <= last; i++) {
        if (table[static_cast<uint16_t>(hay[i])] != first) continue;
        size_t j = 1;
        for (; j < needleLen; j++) {
            if (table[static_cast<uint16_t>(hay[i + j])] != static_cast<uint16_t>(folded[j])) break;
        }
        if (j == needleLen) return true;
    }
    return false;
}

int computeRank(const std::wstring& name, const std::wstring& needle) {
    if (winutil::equalsIgnoreCase(name, needle)) return 0;
    if (winutil::startsWithIgnoreCase(name, needle)) return 1;
    return 2;
}

void appendLoadError(const std::wstring& indexPath, const std::wstring& message) {
    std::wstring dir = winutil::parentDir(indexPath);
    std::wstring log = winutil::joinPath(dir, L"load-error.log");
    std::wstring line = winutil::nowIsoLocal() + L" " + indexPath + L": " + message + L"\r\n";
    HANDLE h = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        std::string utf8 = winutil::wideToUtf8(line);
        DWORD written = 0;
        WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        CloseHandle(h);
    }
}

std::wstring readNameAt(const uint8_t* base, uint64_t nameSectionOffset, uint32_t nameOffset,
                        uint16_t expectedLen, bool& ok) {
    ok = false;
    const uint8_t* ptr = base + nameSectionOffset + nameOffset;
    uint16_t storedLen = readU16(ptr);
    if (storedLen != expectedLen || (storedLen & 1) != 0) return L"";
    std::wstring name;
    name.reserve(storedLen / 2);
    const uint8_t* chars = ptr + 2;
    for (size_t i = 0; i < storedLen / 2; i++) {
        name.push_back(static_cast<wchar_t>(chars[i * 2] | (uint16_t(chars[i * 2 + 1]) << 8)));
    }
    ok = true;
    return name;
}

} // namespace

NtfsIndex::NtfsIndex(const std::wstring& indexPath, const NtfsIndexMeta& meta, const DeltaData& delta)
    : meta_(meta) {
    fileHandle_ = CreateFileW(indexPath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fileHandle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open index file");

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(fileHandle_, &size) || size.QuadPart <= 0) {
        CloseHandle(fileHandle_);
        fileHandle_ = INVALID_HANDLE_VALUE;
        throw std::runtime_error("bad index size");
    }
    mapSize_ = static_cast<size_t>(size.QuadPart);

    mappingHandle_ = CreateFileMappingW(fileHandle_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mappingHandle_) {
        CloseHandle(fileHandle_);
        fileHandle_ = INVALID_HANDLE_VALUE;
        throw std::runtime_error("cannot create file mapping");
    }
    base_ = static_cast<uint8_t*>(MapViewOfFile(mappingHandle_, FILE_MAP_READ, 0, 0, 0));
    if (!base_) {
        CloseHandle(mappingHandle_);
        mappingHandle_ = nullptr;
        CloseHandle(fileHandle_);
        fileHandle_ = INVALID_HANDLE_VALUE;
        throw std::runtime_error("cannot map index file");
    }

    if (!tryReadHeader(mapSize_)) throw std::runtime_error("invalid index header");

    driveRoot_ = std::wstring(1, meta_.drive) + L":\\";
    rootRef_ = meta_.rootRef;
    dirs_.reserve(static_cast<size_t>(std::min<int64_t>(dirCount_, 1'000'000)));
    buildDirectoryMap();

    if (!delta.records.empty()) {
        delta_ = std::make_unique<DeltaState>(DeltaState::build(delta.records));
    }
}

std::unique_ptr<NtfsIndex> NtfsIndex::tryLoad(const std::wstring& indexPath,
                                              const NtfsIndexMeta& meta,
                                              const DeltaData& delta) {
    try {
        return std::unique_ptr<NtfsIndex>(new NtfsIndex(indexPath, meta, delta));
    } catch (const std::exception& ex) {
        appendLoadError(indexPath, winutil::utf8ToWide(ex.what()));
        return nullptr;
    } catch (...) {
        appendLoadError(indexPath, L"unknown error");
        return nullptr;
    }
}

NtfsIndex::~NtfsIndex() {
    if (base_) {
        UnmapViewOfFile(base_);
        base_ = nullptr;
    }
    if (mappingHandle_) {
        CloseHandle(mappingHandle_);
        mappingHandle_ = nullptr;
    }
    if (fileHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(fileHandle_);
        fileHandle_ = INVALID_HANDLE_VALUE;
    }
}

bool NtfsIndex::tryReadHeader(size_t mapSize) {
    if (mapSize < static_cast<size_t>(ntfs_index_format::kHeaderSize + ntfs_index_format::kRecordSize)) {
        return false;
    }
    const char magic[8] = {'P', 'R', 'S', 'I', 'D', 'X', '0', '1'};
    if (memcmp(base_, magic, 8) != 0) return false;
    if (readU32(base_ + 8) != ntfs_index_format::kVersion) return false;

    recordCount_ = static_cast<int64_t>(readU64(base_ + 24));
    nameSectionOffset_ = readU64(base_ + 32);
    dirCount_ = static_cast<int64_t>(readU64(base_ + 40));
    return recordCount_ >= 0 &&
           nameSectionOffset_ == static_cast<uint64_t>(ntfs_index_format::kHeaderSize +
                                                       recordCount_ * ntfs_index_format::kRecordSize) &&
           nameSectionOffset_ <= mapSize;
}

void NtfsIndex::buildDirectoryMap() {
    const uint8_t* records = base_ + ntfs_index_format::kHeaderSize;
    for (int64_t i = 0; i < recordCount_; i++) {
        const uint8_t* record = records + i * ntfs_index_format::kRecordSize;
        if (record[38] == 0) continue;
        uint64_t refValue = readU64(record) & 0xFFFFFFFFFFFFULL;
        DirEntry dir;
        dir.parentRef = readU64(record + 8);
        dir.nameOffset = readU32(record + 32);
        dir.nameLenBytes = readU16(record + 36);
        dirs_[refValue] = dir;
    }
}

bool NtfsIndex::tryGetDir(uint64_t key, DirEntry& out) {
    if (delta_) {
        if (delta_->deletedDirectories.count(key)) return false;
        auto it = delta_->dirOverlay.find(key);
        if (it != delta_->dirOverlay.end()) {
            out = DirEntry{};
            out.parentRef = it->second.parentRef;
            out.overlayName = it->second.name;
            out.hasOverlay = true;
            return true;
        }
    }
    auto it = dirs_.find(key);
    if (it == dirs_.end()) return false;
    out = it->second;
    return true;
}

std::wstring NtfsIndex::dirName(const DirEntry& dir) {
    if (dir.hasOverlay && !dir.overlayName.empty()) return dir.overlayName;
    bool ok = false;
    std::wstring name = readNameAt(base_, nameSectionOffset_, dir.nameOffset, dir.nameLenBytes, ok);
    return ok ? name : L"";
}

std::wstring NtfsIndex::resolvePath(uint64_t refValue, int depth) {
    if (depth > 256) return driveRoot_;
    uint64_t key = refValue & 0xFFFFFFFFFFFFULL;
    if (key == rootRef_) return driveRoot_;

    {
        std::lock_guard<std::mutex> lock(pathMutex_);
        auto it = pathMemo_.find(key);
        if (it != pathMemo_.end()) return it->second;
    }

    DirEntry dir;
    if (!tryGetDir(key, dir)) return driveRoot_;
    std::wstring parent = resolvePath(dir.parentRef, depth + 1);
    std::wstring name = dirName(dir);
    std::wstring path = (!parent.empty() && parent.back() == L'\\') ? parent + name : parent + L"\\" + name;

    {
        std::lock_guard<std::mutex> lock(pathMutex_);
        pathMemo_[key] = path;
    }
    return path;
}

std::wstring NtfsIndex::resolvePath(uint64_t refValue) { return resolvePath(refValue, 0); }

std::vector<NtfsSearchResult> NtfsIndex::search(const std::wstring& needle, int maxResults,
                                                const std::atomic<bool>* cancel) {
    struct Candidate {
        int rank = 0;
        NtfsSearchResult result;
    };
    std::vector<Candidate> found;
    if (needle.empty() || maxResults <= 0) return {};

    // Fold the needle once; the scan then compares directly against the
    // memory-mapped name section without per-record allocations.
    std::wstring foldedNeedle(needle.size(), L'\0');
    for (size_t i = 0; i < needle.size(); i++) foldedNeedle[i] = static_cast<wchar_t>(foldChar(needle[i]));

    int candidateLimit = std::max(2000, maxResults * 10);
    const uint8_t* records = base_ + ntfs_index_format::kHeaderSize;
    const uint8_t* names = base_ + nameSectionOffset_;

    for (int64_t i = 0; i < recordCount_ && static_cast<int>(found.size()) < candidateLimit; i++) {
        if ((i & 0x3FFF) == 0 && cancel && cancel->load()) break;
        const uint8_t* record = records + i * ntfs_index_format::kRecordSize;
        uint64_t refValue = readU64(record);
        uint64_t parentRef = readU64(record + 8);
        uint64_t size = readU64(record + 16);
        uint64_t modifiedTime = readU64(record + 24);
        uint32_t nameOffset = readU32(record + 32);
        uint16_t nameLenBytes = readU16(record + 36);
        bool isDirectory = record[38] != 0;
        uint64_t key = refValue & 0xFFFFFFFFFFFFULL;

        bool hasEffectiveName = false;
        std::wstring effectiveName;
        if (delta_) {
            if (isDirectory) {
                if (delta_->deletedDirectories.count(key)) continue;
                auto it = delta_->dirOverlay.find(key);
                if (it != delta_->dirOverlay.end()) {
                    if (it->second.op == DeltaOp::Rename || it->second.op == DeltaOp::Add) {
                        effectiveName = it->second.name;
                        hasEffectiveName = !effectiveName.empty();
                    }
                    parentRef = it->second.parentRef;
                }
            } else {
                if (delta_->deletedFiles.count(key)) continue;
                auto it = delta_->fileOverlay.find(key);
                if (it != delta_->fileOverlay.end()) {
                    parentRef = it->second.parentRef;
                    modifiedTime = it->second.modifiedTime;
                    if (it->second.op == DeltaOp::Rename) {
                        effectiveName = it->second.name;
                        hasEffectiveName = !effectiveName.empty();
                    }
                }
            }
        }

        bool matched = false;
        const wchar_t* namePtr = nullptr;
        size_t nameLen = 0;
        if (hasEffectiveName) {
            namePtr = effectiveName.data();
            nameLen = effectiveName.size();
            matched = containsFolded(namePtr, nameLen, foldedNeedle.data(), foldedNeedle.size());
        } else {
            // View the name bytes directly inside the mapped file; no copy until
            // the needle actually matches.
            const uint8_t* namePointer = names + nameOffset;
            uint16_t storedLen = readU16(namePointer);
            if (storedLen != nameLenBytes || (storedLen & 1) != 0) continue; // corrupt entry
            if (static_cast<uint64_t>(nameOffset) + 2u + storedLen > mapSize_ - nameSectionOffset_) {
                continue; // corrupt entry
            }
            namePtr = reinterpret_cast<const wchar_t*>(namePointer + 2);
            nameLen = storedLen / 2;
            matched = containsFolded(namePtr, nameLen, foldedNeedle.data(), foldedNeedle.size());
        }
        if (!matched) continue;

        std::wstring displayName;
        if (hasEffectiveName) {
            displayName = effectiveName;
        } else {
            displayName.assign(namePtr, nameLen);
        }

        std::wstring path;
        if (isDirectory) {
            path = resolvePath(key);
        } else {
            std::wstring directory = resolvePath(parentRef);
            path = (!directory.empty() && directory.back() == L'\\') ? directory + displayName
                                                                     : directory + L"\\" + displayName;
        }

        Candidate c;
        c.rank = computeRank(displayName, needle);
        c.result.name = std::move(displayName);
        c.result.path = std::move(path);
        c.result.isDirectory = isDirectory;
        c.result.size = size;
        c.result.modifiedTime = modifiedTime;
        c.result.ref = refValue;
        found.push_back(std::move(c));
    }

    if (delta_) {
        for (const auto& add : delta_->added) {
            if (static_cast<int>(found.size()) >= candidateLimit) break;
            uint64_t addKey = add.ref & 0xFFFFFFFFFFFFULL;
            if ((add.isDirectory && delta_->deletedDirectories.count(addKey)) ||
                (!add.isDirectory && delta_->deletedFiles.count(addKey))) {
                continue;
            }
            std::wstring effectiveName = add.name;
            uint64_t effectiveParent = add.parentRef;
            uint64_t effectiveTime = add.modifiedTime;
            if (add.isDirectory) {
                auto it = delta_->dirOverlay.find(addKey);
                if (it != delta_->dirOverlay.end()) {
                    effectiveName = it->second.name;
                    effectiveParent = it->second.parentRef;
                    effectiveTime = it->second.modifiedTime;
                }
            } else {
                auto it = delta_->fileOverlay.find(addKey);
                if (it != delta_->fileOverlay.end()) {
                    if (!it->second.name.empty()) effectiveName = it->second.name;
                    effectiveParent = it->second.parentRef;
                    effectiveTime = it->second.modifiedTime;
                }
            }
            if (!containsFolded(effectiveName.data(), effectiveName.size(),
                                foldedNeedle.data(), foldedNeedle.size())) {
                continue;
            }

            std::wstring path;
            if (add.isDirectory) {
                path = resolvePath(addKey);
            } else {
                std::wstring directory = resolvePath(effectiveParent);
                path = (!directory.empty() && directory.back() == L'\\') ? directory + effectiveName
                                                                         : directory + L"\\" + effectiveName;
            }
            Candidate c;
            c.rank = computeRank(effectiveName, needle);
            c.result.name = effectiveName;
            c.result.path = std::move(path);
            c.result.isDirectory = add.isDirectory;
            c.result.size = add.size;
            c.result.modifiedTime = effectiveTime;
            c.result.ref = add.ref;
            found.push_back(std::move(c));
        }
    }

    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.result.name.size() != b.result.name.size()) return a.result.name.size() < b.result.name.size();
        return winutil::lessIgnoreCase(a.result.name, b.result.name);
    });

    std::vector<NtfsSearchResult> out;
    size_t count = std::min<size_t>(found.size(), static_cast<size_t>(maxResults));
    out.reserve(count);
    for (size_t i = 0; i < count; i++) out.push_back(std::move(found[i].result));
    return out;
}
