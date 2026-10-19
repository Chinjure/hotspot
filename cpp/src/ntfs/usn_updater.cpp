#include "usn_updater.h"

#include <windows.h>

#include <cstring>
#include <unordered_map>

#include "json.h"
#include "ntfs_win32.h"
#include "winutil.h"

namespace usn {

namespace {

uint64_t utcNowTicks() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    uint64_t fileTime = uint64_t(ft.dwHighDateTime) << 32 | uint64_t(ft.dwLowDateTime);
    // Convert FILETIME (1601) to .NET ticks (0001).
    return fileTime + 116444736000000000ULL;
}

std::wstring readWide(const uint8_t* p, size_t chars) {
    std::wstring s;
    s.reserve(chars);
    for (size_t i = 0; i < chars; i++) {
        s.push_back(static_cast<wchar_t>(p[i * 2] | (uint16_t(p[i * 2 + 1]) << 8)));
    }
    return s;
}

void applyRecord(const uint8_t* buffer, size_t offset, std::unordered_map<uint64_t, DeltaRecord>& pending) {
    uint64_t fileRef = 0, parentRef = 0, timestamp = 0;
    uint32_t reason = 0, attributes = 0;
    uint16_t nameLength = 0, nameOffset = 0;
    memcpy(&fileRef, buffer + offset + 8, 8);
    memcpy(&parentRef, buffer + offset + 16, 8);
    memcpy(&timestamp, buffer + offset + 32, 8);
    memcpy(&reason, buffer + offset + 40, 4);
    memcpy(&attributes, buffer + offset + 52, 4);
    memcpy(&nameLength, buffer + offset + 56, 2);
    memcpy(&nameOffset, buffer + offset + 58, 2);

    std::wstring name;
    if (nameLength > 0 && offset + nameOffset + nameLength <= 1u << 20) {
        name = readWide(buffer + offset + nameOffset, nameLength / 2);
    }

    uint64_t key = fileRef & 0xFFFFFFFFFFFFULL;
    bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    DeltaRecord rec{};
    rec.isDirectory = isDirectory;
    rec.ref = fileRef;
    rec.parentRef = parentRef;
    rec.modifiedTime = timestamp;
    rec.name = name;

    if ((reason & ntfs_win32::kUsnReasonFileDelete) != 0) {
        rec.op = DeltaOp::Delete;
        pending[key] = std::move(rec);
    } else if ((reason & ntfs_win32::kUsnReasonFileCreate) != 0) {
        rec.op = DeltaOp::Add;
        rec.size = 0;
        pending[key] = std::move(rec);
    } else if ((reason & ntfs_win32::kUsnReasonRenameNewName) != 0) {
        rec.op = DeltaOp::Rename;
        pending[key] = std::move(rec);
    } else if ((reason & ntfs_win32::kUsnReasonClose) != 0) {
        auto it = pending.find(key);
        if (it != pending.end()) {
            it->second.modifiedTime = timestamp;
        } else {
            rec.op = DeltaOp::Update;
            rec.name.clear(); // merge keeps the base name
            pending[key] = std::move(rec);
        }
    }
}

json::Value deltaToJson(const DeltaData& data) {
    json::Array records;
    for (const auto& r : data.records) {
        json::Object obj;
        obj["Op"] = json::Value(static_cast<int64_t>(r.op));
        obj["IsDirectory"] = json::Value(r.isDirectory);
        obj["Ref"] = json::Value(static_cast<int64_t>(r.ref));
        obj["ParentRef"] = json::Value(static_cast<int64_t>(r.parentRef));
        obj["Size"] = json::Value(static_cast<int64_t>(r.size));
        obj["ModifiedTime"] = json::Value(static_cast<int64_t>(r.modifiedTime));
        obj["Name"] = json::Value(winutil::wideToUtf8(r.name));
        records.push_back(json::Value(std::move(obj)));
    }
    json::Object obj;
    obj["Epoch"] = json::Value(data.epoch);
    obj["Records"] = json::Value(std::move(records));
    return json::Value(std::move(obj));
}

DeltaData deltaFromJson(const json::Value& root) {
    DeltaData data;
    if (!root.isObject()) return data;
    const json::Value* epoch = root.find("Epoch");
    if (epoch) data.epoch = epoch->asInt(data.epoch);
    const json::Value* records = root.find("Records");
    if (!records || !records->isArray()) return data;
    for (const auto& v : records->asArray()) {
        if (!v.isObject()) continue;
        DeltaRecord r{};
        const json::Value* op = v.find("Op");
        if (op) r.op = static_cast<DeltaOp>(op->asInt(0));
        const json::Value* isDir = v.find("IsDirectory");
        if (isDir) r.isDirectory = isDir->asBool();
        const json::Value* ref = v.find("Ref");
        if (ref) r.ref = static_cast<uint64_t>(ref->asInt(0));
        const json::Value* parent = v.find("ParentRef");
        if (parent) r.parentRef = static_cast<uint64_t>(parent->asInt(0));
        const json::Value* size = v.find("Size");
        if (size) r.size = static_cast<uint64_t>(size->asInt(0));
        const json::Value* mt = v.find("ModifiedTime");
        if (mt) r.modifiedTime = static_cast<uint64_t>(mt->asInt(0));
        const json::Value* name = v.find("Name");
        if (name && name->isString()) r.name = winutil::utf8ToWide(name->asString());
        data.records.push_back(std::move(r));
    }
    return data;
}

} // namespace

std::wstring deltaPath(const std::wstring& indexDirectory, wchar_t drive) {
    return winutil::joinPath(indexDirectory, std::wstring(1, drive) + L"-delta.json");
}

DeltaData loadDelta(const std::wstring& indexDirectory, wchar_t drive, int64_t expectEpoch) {
    std::wstring path = deltaPath(indexDirectory, drive);
    try {
        std::string content;
        if (!winutil::readTextFile(path, content)) return DeltaData{expectEpoch, {}};
        json::Value root = json::parse(content);
        DeltaData data = deltaFromJson(root);
        if (data.epoch != expectEpoch) return DeltaData{expectEpoch, {}};
        return data;
    } catch (...) {
        return DeltaData{expectEpoch, {}};
    }
}

void saveDelta(const std::wstring& path, const DeltaData& data) {
    winutil::writeTextFile(path, json::serialize(deltaToJson(data)));
}

bool tryUpdate(const std::wstring& indexDirectory, wchar_t drive, NtfsIndexMeta& meta,
               std::vector<DeltaRecord>& newRecords, int64_t& nextUsn, std::wstring& error) {
    newRecords.clear();
    nextUsn = meta.nextUsn;
    error.clear();

    HANDLE volume = ntfs_win32::openVolume(drive, error);
    if (volume == INVALID_HANDLE_VALUE) return false;

    uint8_t journalBuffer[64] = {};
    DWORD returned = 0;
    std::wstring ioError;
    if (!ntfs_win32::deviceIoControl(volume, ntfs_win32::kFsctlQueryUsnJournal, nullptr, 0,
                                     journalBuffer, sizeof(journalBuffer), returned, ioError) ||
        returned < 56) {
        error = L"USN journal 不可用: " + ioError;
        CloseHandle(volume);
        return false;
    }

    uint64_t currentJournalId = 0;
    int64_t currentNextUsn = 0;
    memcpy(&currentJournalId, journalBuffer, 8);
    memcpy(&currentNextUsn, journalBuffer + 16, 8);

    if (meta.journalId == 0) {
        // Walk-built index has no baseline: calibrate.
        meta.journalId = currentJournalId;
        meta.nextUsn = currentNextUsn;
        meta.lastScanTimeUtcTicks = static_cast<int64_t>(utcNowTicks());
        CloseHandle(volume);
        nextUsn = currentNextUsn;
        return true;
    }

    if (meta.journalId != currentJournalId) {
        error = L"USN journal 已被重建，需要全量重建索引";
        CloseHandle(volume);
        return false;
    }

    std::unordered_map<uint64_t, DeltaRecord> pending;
    int64_t usn = meta.nextUsn;
    constexpr size_t bufferSize = 1024 * 1024;
    std::vector<uint8_t> input(40, 0);
    std::vector<uint8_t> output(bufferSize);

    while (true) {
        memset(input.data(), 0, input.size());
        memcpy(input.data(), &usn, 8);
        uint32_t reasonMask = 0xFFFFFFFF;
        memcpy(input.data() + 8, &reasonMask, 4);
        uint32_t returnOnly = 0;
        memcpy(input.data() + 12, &returnOnly, 4);
        uint64_t timeout = 0;
        memcpy(input.data() + 16, &timeout, 8);
        uint64_t bytesToWait = 0;
        memcpy(input.data() + 24, &bytesToWait, 8);
        memcpy(input.data() + 32, &currentJournalId, 8);

        DWORD bytes = 0;
        if (!ntfs_win32::deviceIoControl(volume, ntfs_win32::kFsctlReadUsnJournal,
                                         input.data(), static_cast<DWORD>(input.size()),
                                         output.data(), static_cast<DWORD>(output.size()), bytes, ioError)) {
            DWORD code = GetLastError();
            if (code == ntfs_win32::kErrorJournalDeleted) error = L"USN journal 已被删除，需要全量重建索引";
            else if (code == ntfs_win32::kErrorJournalNotActive) error = L"USN journal 未启用";
            else error = L"读取 USN journal 失败: " + winutil::lastErrorText(code);
            CloseHandle(volume);
            return false;
        }

        if (bytes < 8) break;
        int64_t next = 0;
        memcpy(&next, output.data(), 8);
        if (next == usn) break;

        size_t offset = 8;
        while (offset + 8 <= bytes) {
            uint32_t recordLength = 0;
            memcpy(&recordLength, output.data() + offset, 4);
            if (recordLength == 0 || offset + recordLength > bytes) break;
            uint16_t majorVersion = 0;
            memcpy(&majorVersion, output.data() + offset + 4, 2);
            if (majorVersion >= 2) applyRecord(output.data(), offset, pending);
            offset += recordLength;
        }
        usn = next;
    }

    CloseHandle(volume);

    std::wstring path = deltaPath(indexDirectory, drive);
    DeltaData existing = loadDelta(indexDirectory, drive, meta.epoch);
    std::vector<DeltaRecord> pendingRecords;
    pendingRecords.reserve(pending.size());
    for (auto& [k, v] : pending) pendingRecords.push_back(std::move(v));
    existing.records.insert(existing.records.end(), pendingRecords.begin(), pendingRecords.end());
    saveDelta(path, existing);

    newRecords = std::move(pendingRecords);
    nextUsn = usn;
    meta.nextUsn = nextUsn;
    meta.lastScanTimeUtcTicks = static_cast<int64_t>(utcNowTicks());
    return true;
}

} // namespace usn
