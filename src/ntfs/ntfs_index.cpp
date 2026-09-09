#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "ntfs_index.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "file_rank.h"
#include "path_query.h"
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

// One-time invariant-locale lowercase folding lives in winutil (shared with the
// path-search matcher); names are compared directly inside the memory-mapped
// name section with it, so a std::wstring is only allocated when a record
// actually matches (the C# version does the same with spans).

int computeRank(const std::wstring& name, const std::wstring& needle) {
    return file_rank::matchScore(name, needle);
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
                                                const std::atomic<bool>* cancel,
                                                const path_query::Query* pathQuery) {
    struct Candidate {
        int rank = 0;
        NtfsSearchResult result;
    };
    const bool pathMode = pathQuery != nullptr && pathQuery->valid;
    auto better = [pathMode](const Candidate& a, const Candidate& b) {
        if (pathMode) {
            return file_rank::betterPathMatch(a.result.name, a.result.isDirectory, a.rank,
                                              b.result.name, b.result.isDirectory, b.rank);
        }
        if (a.result.launchable != b.result.launchable) return a.result.launchable;
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.result.kind != b.result.kind) {
            return static_cast<int>(a.result.kind) < static_cast<int>(b.result.kind);
        }
        if (a.result.name.size() != b.result.name.size()) {
            return a.result.name.size() < b.result.name.size();
        }
        return winutil::lessIgnoreCase(a.result.name, b.result.name);
    };
    std::vector<Candidate> found;
    if (needle.empty() || maxResults <= 0) return {};

    // Fold the needle once; the scan then compares directly against the
    // memory-mapped name section without per-record allocations.
    std::wstring foldedNeedle = winutil::foldString(needle);

    // Path mode prefilter: unless the query ends with a separator, its final
    // segment must occur in the candidate name, so path reconstruction only
    // runs for plausible records.
    std::wstring foldedLast;
    bool namePrefilter = false;
    if (pathMode) {
        foldedLast = pathQuery->foldedSegments.empty() ? std::wstring() : pathQuery->foldedSegments.back();
        namePrefilter = !pathQuery->trailingSeparator && !foldedLast.empty();
    }

    // Path scoring needs the parent directory of every candidate; resolvePath()
    // returns a copy, so cache the per-directory result for the scan.
    std::unordered_map<uint64_t, std::wstring> parentCache;
    auto parentPathOf = [&](uint64_t ref) -> const std::wstring& {
        auto it = parentCache.find(ref);
        if (it != parentCache.end()) return it->second;
        return parentCache.emplace(ref, resolvePath(ref)).first->second;
    };

    int candidateLimit = std::max(2000, maxResults * 10);
    const uint8_t* records = base_ + ntfs_index_format::kHeaderSize;
    const uint8_t* names = base_ + nameSectionOffset_;

    auto emitCandidate = [&](const std::wstring& displayName, uint64_t parent, bool isDirectory,
                             uint64_t size, uint64_t modifiedTime, uint64_t refValue, int rank) {
        std::wstring path;
        if (isDirectory) {
            path = resolvePath(refValue & 0xFFFFFFFFFFFFULL);
        } else {
            std::wstring directory = resolvePath(parent);
            path = (!directory.empty() && directory.back() == L'\\') ? directory + displayName
                                                                     : directory + L"\\" + displayName;
        }
        Candidate c;
        c.rank = rank;
        c.result.name = displayName;
        c.result.path = std::move(path);
        c.result.isDirectory = isDirectory;
        c.result.matchTier = rank;
        c.result.kind = file_rank::classify(c.result.name, isDirectory);
        c.result.launchable = file_rank::isLaunchable(c.result.kind);
        c.result.size = size;
        c.result.modifiedTime = modifiedTime;
        c.result.ref = refValue;
        found.push_back(std::move(c));
    };

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

        const wchar_t* namePtr = nullptr;
        size_t nameLen = 0;
        if (hasEffectiveName) {
            namePtr = effectiveName.data();
            nameLen = effectiveName.size();
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
        }

        if (pathMode) {
            if (namePrefilter &&
                !winutil::containsFolded(namePtr, nameLen, foldedLast.data(), foldedLast.size())) {
                continue;
            }
            std::wstring displayName(namePtr, nameLen);
            path_query::Match match = path_query::scorePath(parentPathOf(parentRef), displayName, isDirectory, *pathQuery);
            if (match == path_query::Match::None) continue;
            emitCandidate(displayName, parentRef, isDirectory, size, modifiedTime, refValue,
                          static_cast<int>(match));
        } else {
            if (!winutil::containsFolded(namePtr, nameLen, foldedNeedle.data(), foldedNeedle.size())) {
                continue;
            }
            std::wstring displayName(namePtr, nameLen);
            emitCandidate(displayName, parentRef, isDirectory, size, modifiedTime, refValue,
                          computeRank(displayName, needle));
        }
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

            if (pathMode) {
                if (namePrefilter && !winutil::containsFolded(effectiveName, foldedLast)) continue;
                path_query::Match match =
                    path_query::scorePath(parentPathOf(effectiveParent), effectiveName, add.isDirectory, *pathQuery);
                if (match == path_query::Match::None) continue;
                emitCandidate(effectiveName, effectiveParent, add.isDirectory, add.size, effectiveTime,
                              add.ref, static_cast<int>(match));
            } else {
                if (!winutil::containsFolded(effectiveName, foldedNeedle)) continue;
                emitCandidate(effectiveName, effectiveParent, add.isDirectory, add.size, effectiveTime,
                              add.ref, computeRank(effectiveName, needle));
            }
        }
    }

    std::sort(found.begin(), found.end(), better);

    std::vector<NtfsSearchResult> out;
    size_t count = std::min<size_t>(found.size(), static_cast<size_t>(maxResults));
    out.reserve(count);
    for (size_t i = 0; i < count; i++) out.push_back(std::move(found[i].result));
    return out;
}
