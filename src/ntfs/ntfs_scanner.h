#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "ntfs_types.h"

namespace ntfsscanner {

using EntryCallback = std::function<void(const NtfsEntry&)>;
using ProgressCallback = std::function<void(int64_t)>;

// Streams all active $MFT FILE records. Requires SeBackupPrivilege +
// administrator on most systems. Returns records visited.
int64_t scanMft(wchar_t drive, const EntryCallback& onEntry, const ProgressCallback& onProgress,
                std::atomic<bool>& cancel, std::wstring& error);

// Full recursive directory walk (no admin required). Returns entries emitted.
int64_t walkVolume(wchar_t drive, const EntryCallback& onEntry, const ProgressCallback& onProgress,
                   std::atomic<bool>& cancel, std::wstring& error);

uint64_t tryGetRootRef(wchar_t drive);

} // namespace ntfsscanner
