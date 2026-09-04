#include "ntfs_index_writer.h"

#include <windows.h>

#include <cstring>
#include <filesystem>

#include "winutil.h"

namespace fs = std::filesystem;

namespace {

void writeU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void writeU32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}
void writeU64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

std::vector<uint8_t> buildHeader(const NtfsIndexMeta& meta) {
    std::vector<uint8_t> header(ntfs_index_format::kHeaderSize, 0);
    const char magic[8] = {'P', 'R', 'S', 'I', 'D', 'X', '0', '1'};
    memcpy(header.data(), magic, 8);
    writeU32(header.data() + 8, static_cast<uint32_t>(ntfs_index_format::kVersion));
    writeU64(header.data() + 16, static_cast<uint64_t>(meta.epoch));
    writeU64(header.data() + 24, static_cast<uint64_t>(meta.fileCount + meta.directoryCount));
    writeU64(header.data() + 32, static_cast<uint64_t>(ntfs_index_format::kHeaderSize +
                                                      (meta.fileCount + meta.directoryCount) * ntfs_index_format::kRecordSize));
    writeU64(header.data() + 40, static_cast<uint64_t>(meta.directoryCount));
    writeU16(header.data() + 48, static_cast<uint16_t>(meta.drive));
    writeU32(header.data() + 52, meta.bytesPerRecord);
    writeU32(header.data() + 56, static_cast<uint32_t>(meta.source));
    return header;
}

} // namespace

NtfsIndexWriter::NtfsIndexWriter(const std::wstring& directory, const std::wstring& tempPrefix)
    : directory_(directory), tempPrefix_(tempPrefix.empty() ? L"index" : tempPrefix) {
    winutil::createDirectory(directory_);
    fs::path dir(directory_);
    recordsFile_.open(dir / (tempPrefix_ + L"-records.tmp"), std::ios::binary | std::ios::trunc);
    namesFile_.open(dir / (tempPrefix_ + L"-names.tmp"), std::ios::binary | std::ios::trunc);
    opened_ = recordsFile_.is_open() && namesFile_.is_open();
}

NtfsIndexWriter::~NtfsIndexWriter() { cleanup(); }

void NtfsIndexWriter::append(const NtfsEntry& entry) {
    if (!opened_) return;

    std::vector<uint8_t> record(ntfs_index_format::kRecordSize, 0);
    writeU64(record.data(), entry.ref);
    writeU64(record.data() + 8, entry.parentRef);
    writeU64(record.data() + 16, entry.size);
    writeU64(record.data() + 24, entry.modifiedTime);
    writeU32(record.data() + 32, static_cast<uint32_t>(namePosition_));
    writeU16(record.data() + 36, static_cast<uint16_t>(entry.name.size() * 2));
    record[38] = entry.isDirectory ? 1 : 0;
    record[39] = 0;
    recordsFile_.write(reinterpret_cast<const char*>(record.data()), static_cast<std::streamsize>(record.size()));

    uint8_t lengthBytes[2] = {};
    writeU16(lengthBytes, static_cast<uint16_t>(entry.name.size() * 2));
    namesFile_.write(reinterpret_cast<const char*>(lengthBytes), 2);
    for (wchar_t c : entry.name) {
        char bytes[2] = {static_cast<char>(c & 0xFF), static_cast<char>((c >> 8) & 0xFF)};
        namesFile_.write(bytes, 2);
    }

    namePosition_ += 2 + entry.name.size() * 2;
    if (entry.isDirectory) dirCount_++;
    else fileCount_++;
}

bool NtfsIndexWriter::commit(const NtfsIndexMeta& meta, const std::wstring& indexPath) {
    if (!opened_) return false;
    recordsFile_.flush();
    namesFile_.flush();

    std::vector<uint8_t> header = buildHeader(meta);
    std::wstring tempPath = indexPath + L".tmp";
    fs::path finalPath(indexPath);
    fs::path temp(tempPath);

    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));

    fs::path recordsPath = fs::path(directory_) / (tempPrefix_ + L"-records.tmp");
    fs::path namesPath = fs::path(directory_) / (tempPrefix_ + L"-names.tmp");
    {
        std::ifstream inRecords(recordsPath, std::ios::binary);
        if (inRecords.is_open()) out << inRecords.rdbuf();
    }
    {
        std::ifstream inNames(namesPath, std::ios::binary);
        if (inNames.is_open()) out << inNames.rdbuf();
    }
    out.flush();
    out.close();

    if (!MoveFileExW(tempPath.c_str(), indexPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    committed_ = true;
    return true;
}

void NtfsIndexWriter::cleanup() {
    if (recordsFile_.is_open()) recordsFile_.close();
    if (namesFile_.is_open()) namesFile_.close();
    if (!committed_) {
        fs::path dir(directory_);
        DeleteFileW((dir / (tempPrefix_ + L"-records.tmp")).c_str());
        DeleteFileW((dir / (tempPrefix_ + L"-names.tmp")).c_str());
    }
}
