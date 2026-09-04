#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ntfs_types.h"

namespace ntfs_win32 {

// ---- DeviceIoControl codes (renamed to avoid SDK macros) ----
inline constexpr DWORD kFsctlGetNtfsVolumeData = 0x00090064;
inline constexpr DWORD kFsctlReadUsnJournal = 0x000900BB;
inline constexpr DWORD kFsctlQueryUsnJournal = 0x000900F4;

// ---- USN reasons ----
inline constexpr DWORD kUsnReasonFileCreate = 0x00000100;
inline constexpr DWORD kUsnReasonFileDelete = 0x00000200;
inline constexpr DWORD kUsnReasonRenameNewName = 0x00002000;
inline constexpr DWORD kUsnReasonClose = 0x80000000;

// ---- Errors ----
inline constexpr DWORD kErrorJournalNotActive = 1179;
inline constexpr DWORD kErrorJournalDeleted = 1178;

// ---- FindFirstFileEx ----
inline constexpr int kFindExInfoBasic = 1;
inline constexpr int kFindExSearchNameMatch = 0;
inline constexpr DWORD kFindFirstExLargeFetch = 2;

// Volume open / $MFT / file-ID helpers --------------------------------------
std::wstring win32Message(DWORD code);

std::vector<wchar_t> getFixedNtfsDrives();
bool tryGetVolumeInfo(wchar_t drive, NtfsVolumeInfo& info, std::wstring& error);

HANDLE openVolume(wchar_t drive, std::wstring& error);
HANDLE openMft(wchar_t drive, std::wstring& error);
HANDLE openForFileId(const std::wstring& path, bool isDirectory, std::wstring& error);
bool tryEnableBackupPrivilege(std::wstring& error);
uint64_t getFileId(HANDLE handle);
uint64_t tryGetRootRef(wchar_t drive);
bool deviceIoControl(HANDLE device, DWORD code, const void* in, DWORD inSize,
                     void* out, DWORD outSize, DWORD& returned, std::wstring& error);

} // namespace ntfs_win32
