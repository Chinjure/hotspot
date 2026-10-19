#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ntfs_types.h"

namespace usn {

// Reads USN Journal changes since meta.NextUsn and appends them to the
// delta file that overlays the base index. Returns false when the journal is
// unavailable; meta is updated on success (or calibration-only success).
bool tryUpdate(const std::wstring& indexDirectory, wchar_t drive, NtfsIndexMeta& meta,
               std::vector<DeltaRecord>& newRecords, int64_t& nextUsn, std::wstring& error);

DeltaData loadDelta(const std::wstring& indexDirectory, wchar_t drive, int64_t expectEpoch);
void saveDelta(const std::wstring& path, const DeltaData& data);
std::wstring deltaPath(const std::wstring& indexDirectory, wchar_t drive);

} // namespace usn
