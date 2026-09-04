#include "ntfs_scanner.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "ntfs_win32.h"
#include "winutil.h"

namespace ntfsscanner {

namespace {

constexpr int kSystemRecordCount = 24;
constexpr uint64_t kRootDirectoryRecord = 5;
constexpr int kMaxDirectoryDepth = 128;

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (uint16_t(p[1]) << 8)); }
uint32_t readU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t readU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= uint64_t(p[i]) << (8 * i);
    return v;
}

bool tryParseMftRecord(const uint8_t* record, size_t length, int64_t recordNumber, NtfsEntry& entry) {
    if (length < 0x30) return false;
    if (record[0] != 'F' || record[1] != 'I' || record[2] != 'L' || record[3] != 'E') return false;

    uint16_t flags = readU16(record + 0x16);
    if ((flags & 0x0001) == 0) return false;               // deleted
    if (recordNumber < kSystemRecordCount) return false;   // $MFT/$Bitmap/etc.
    if (readU64(record + 0x20) != 0) return false;         // extension record

    uint16_t attrsOffset = readU16(record + 0x14);
    uint32_t usedSize = readU32(record + 0x18);
    if (attrsOffset < 0x28 || attrsOffset >= usedSize || usedSize > length) return false;

    uint16_t sequence = readU16(record + 0x10);
    bool isDirectory = (flags & 0x0002) != 0;

    bool hasData = false;
    bool hasName = false;
    int bestNamespace = -1;
    uint64_t parentRef = 0, size = 0, modifiedTime = 0;
    std::wstring name;

    uint32_t offset = attrsOffset;
    while (offset + 0x18 <= usedSize) {
        const uint8_t* attr = record + offset;
        uint32_t type = readU32(attr);
        uint32_t lengthAttr = readU32(attr + 4);
        if (type == 0xFFFFFFFF || lengthAttr < 0x18 || offset + lengthAttr > usedSize) break;

        uint8_t nonResident = attr[0x08];
        uint8_t nameLength = attr[0x09];

        if (type == 0x30 && nameLength == 0 && nonResident == 0) {
            uint32_t valueLength = readU32(attr + 0x10);
            uint16_t valueOffset = readU16(attr + 0x14);
            if (valueOffset <= lengthAttr) {
                size_t valueLen = std::min<size_t>(valueLength, lengthAttr - valueOffset);
                const uint8_t* value = attr + valueOffset;
                if (valueLen >= 0x42) {
                    uint8_t fileNameLength = value[0x40];
                    uint8_t fileNameNamespace = value[0x41];
                    if (fileNameLength > 0 && fileNameLength <= 255 && 0x42 + uint32_t(fileNameLength) * 2 <= valueLen) {
                        int priority = 0;
                        if (fileNameNamespace == 1) priority = 3;      // Win32
                        else if (fileNameNamespace == 3) priority = 2; // Win32 & DOS
                        else if (fileNameNamespace == 2) priority = 1; // DOS
                        if (priority > bestNamespace) {
                            parentRef = readU64(value);
                            size = readU64(value + 0x30);
                            modifiedTime = readU64(value + 0x10);
                            name.clear();
                            name.reserve(fileNameLength);
                            const uint8_t* chars = value + 0x42;
                            for (int i = 0; i < fileNameLength; i++) {
                                name.push_back(static_cast<wchar_t>(chars[i * 2] | (uint16_t(chars[i * 2 + 1]) << 8)));
                            }
                            bestNamespace = priority;
                            hasName = true;
                        }
                    }
                }
            }
        } else if (type == 0x80 && nameLength == 0) {
            hasData = true;
        }

        offset += lengthAttr;
    }

    if (!hasName) return false;
    if (!isDirectory && !hasData) return false; // special files skipped

    entry = NtfsEntry{};
    entry.ref = uint64_t(recordNumber) | (uint64_t(sequence) << 48);
    entry.parentRef = parentRef;
    entry.size = size;
    entry.modifiedTime = modifiedTime;
    entry.isDirectory = isDirectory;
    entry.name = std::move(name);
    return true;
}

struct WorkItem {
    std::wstring path;
    uint64_t parentRef = 0;
    int depth = 0;
};

void walkDirectory(const std::wstring& directoryPath, uint64_t parentRef, int depth,
                   std::vector<NtfsEntry>& local, std::vector<WorkItem>& subs) {
    if (depth > kMaxDirectoryDepth) return;
    std::wstring pattern = directoryPath + L"\\*";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(),
                                   static_cast<FINDEX_INFO_LEVELS>(ntfs_win32::kFindExInfoBasic), &data,
                                   static_cast<FINDEX_SEARCH_OPS>(ntfs_win32::kFindExSearchNameMatch), nullptr,
                                   static_cast<DWORD>(ntfs_win32::kFindFirstExLargeFetch));
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        std::wstring name = data.cFileName;
        if (name.empty() || name == L"." || name == L"..") continue;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue; // avoid cycles

        bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        std::wstring fullPath = directoryPath + L"\\" + name;
        std::wstring error;
        HANDLE h = ntfs_win32::openForFileId(fullPath, isDirectory, error);
        if (h == INVALID_HANDLE_VALUE) continue; // ACL denied etc.
        uint64_t refValue = ntfs_win32::getFileId(h);
        CloseHandle(h);
        if (refValue == 0) continue;

        uint64_t size = uint64_t(data.nFileSizeHigh) << 32 | uint64_t(data.nFileSizeLow);
        uint64_t modified = uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32 |
                            uint64_t(data.ftLastWriteTime.dwLowDateTime);

        NtfsEntry e{};
        e.ref = refValue;
        e.parentRef = parentRef;
        e.size = size;
        e.modifiedTime = modified;
        e.isDirectory = isDirectory;
        e.name = name;
        local.push_back(std::move(e));

        if (isDirectory && depth < kMaxDirectoryDepth) {
            subs.push_back(WorkItem{fullPath, refValue, depth + 1});
        }
    } while (FindNextFileW(find, &data));

    FindClose(find);
}

} // namespace

uint64_t tryGetRootRef(wchar_t drive) { return ntfs_win32::tryGetRootRef(drive); }

int64_t scanMft(wchar_t drive, const EntryCallback& onEntry, const ProgressCallback& onProgress,
                std::atomic<bool>& cancel, std::wstring& error) {
    error.clear();
    NtfsVolumeInfo info{};
    if (!ntfs_win32::tryGetVolumeInfo(drive, info, error)) return 0;

    uint64_t bpr = info.bytesPerRecord;
    if (bpr == 0 || bpr > (1u << 16)) {
        error = L"MFT 记录大小异常: " + std::to_wstring(bpr);
        return 0;
    }

    std::wstring privilegeError;
    ntfs_win32::tryEnableBackupPrivilege(privilegeError);

    std::wstring mftError;
    HANDLE mft = ntfs_win32::openMft(drive, mftError);
    if (mft == INVALID_HANDLE_VALUE) {
        error = mftError;
        return 0;
    }

    constexpr size_t bufferSize = 1024 * 1024;
    std::vector<uint8_t> buffer(bufferSize);
    int64_t filePosition = 0;
    int64_t visited = 0;

    while (!cancel.load()) {
        DWORD read = 0;
        if (!ReadFile(mft, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) break;
        size_t offset = 0;
        while (offset + bpr <= read) {
            if (cancel.load()) break;
            int64_t recordNumber = (filePosition + static_cast<int64_t>(offset)) / static_cast<int64_t>(bpr);
            NtfsEntry entry{};
            if (tryParseMftRecord(buffer.data() + offset, bpr, recordNumber, entry)) {
                onEntry(entry);
            }
            visited++;
            offset += static_cast<size_t>(bpr);
        }
        filePosition += read;
        if (visited % 200000 == 0) onProgress(visited);
    }

    onProgress(visited);
    CloseHandle(mft);
    return visited;
}

int64_t walkVolume(wchar_t drive, const EntryCallback& onEntry, const ProgressCallback& onProgress,
                   std::atomic<bool>& cancel, std::wstring& error) {
    error.clear();
    std::wstring root(1, drive);
    root += L":\\";
    uint64_t rootRef = tryGetRootRef(drive);
    if (rootRef == 0) {
        error = L"无法获取卷根目录文件 ID: " + root;
        return 0;
    }

    int threadCount = static_cast<int>(std::thread::hardware_concurrency());
    threadCount = std::clamp(threadCount, 2, 8);

    std::deque<WorkItem> queue;
    std::mutex queueMutex;
    std::condition_variable cv;
    queue.push_back(WorkItem{root, rootRef, 0});
    std::atomic<int64_t> pending{1};

    std::vector<std::vector<NtfsEntry>> localResults(static_cast<size_t>(threadCount));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(threadCount));

    for (int i = 0; i < threadCount; i++) {
        workers.emplace_back([&, i]() {
            auto& local = localResults[static_cast<size_t>(i)];
            while (true) {
                if (cancel.load()) {
                    if (--pending == 0) {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        cv.notify_all();
                    }
                    break;
                }
                WorkItem item;
                {
                    std::unique_lock<std::mutex> lock(queueMutex);
                    if (queue.empty()) {
                        if (pending.load() == 0) break;
                        cv.wait_for(lock, std::chrono::milliseconds(10));
                        continue;
                    }
                    item = queue.front();
                    queue.pop_front();
                }
                (void)0;

                std::vector<NtfsEntry> entries;
                std::vector<WorkItem> subs;
                walkDirectory(item.path, item.parentRef, item.depth, entries, subs);
                for (auto& e : entries) local.push_back(std::move(e));

                if (!subs.empty()) {
                    pending.fetch_add(static_cast<int64_t>(subs.size()));
                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        for (auto& s : subs) queue.push_back(std::move(s));
                    }
                    cv.notify_all();
                }
                if (--pending == 0) {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    cv.notify_all();
                    break;
                }
            }
        });
    }

    for (auto& t : workers) t.join();

    int64_t total = 0;
    int64_t emitted = 0;
    for (const auto& list : localResults) {
        for (const auto& entry : list) {
            if (cancel.load()) return emitted;
            onEntry(entry);
            emitted++;
            if (emitted % 50000 == 0) onProgress(emitted);
        }
        total += static_cast<int64_t>(list.size());
    }
    onProgress(total);
    return total;
}

} // namespace ntfsscanner
