#include "ntfs_win32.h"

#include <cwchar>

#include "winutil.h"

namespace ntfs_win32 {

std::wstring win32Message(DWORD code) { return ::winutil::lastErrorText(code); }

bool deviceIoControl(HANDLE device, DWORD code, const void* in, DWORD inSize,
                     void* out, DWORD outSize, DWORD& returned, std::wstring& error) {
    if (DeviceIoControl(device, code, const_cast<void*>(in), inSize, out, outSize,
                        &returned, nullptr)) {
        error.clear();
        return true;
    }
    error = win32Message(GetLastError());
    return false;
}

std::vector<wchar_t> getFixedNtfsDrives() {
    std::vector<wchar_t> drives;
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'C'; c <= L'Z'; ++c) {
        DWORD bit = 1u << (c - L'A');
        if ((mask & bit) == 0) continue;
        std::wstring root(1, c);
        root += L":\\";
        UINT type = GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED) continue;
        wchar_t fsName[MAX_PATH] = {};
        if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsName,
                                   static_cast<DWORD>(MAX_PATH))) {
            continue;
        }
        if (_wcsicmp(fsName, L"NTFS") == 0) drives.push_back(c);
    }
    return drives;
}

HANDLE openVolume(wchar_t drive, std::wstring& error) {
    std::wstring path = L"\\\\.\\";
    path += drive;
    path += L":";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"无法打开卷 " + path + L": " + win32Message(GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    error.clear();
    return h;
}

HANDLE openMft(wchar_t drive, std::wstring& error) {
    std::wstring path = L"\\\\?\\";
    path += drive;
    path += L":\\$MFT";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"无法打开 " + path + L" (error " + std::to_wstring(GetLastError()) + L"): " +
                win32Message(GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    error.clear();
    return h;
}

HANDLE openForFileId(const std::wstring& path, bool isDirectory, std::wstring& error) {
    DWORD flags = isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = win32Message(GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    error.clear();
    return h;
}

bool tryEnableBackupPrivilege(std::wstring& error) {
    error.clear();
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        error = L"OpenProcessToken 失败: " + win32Message(GetLastError());
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeBackupPrivilege", &luid)) {
        error = L"LookupPrivilegeValue(SeBackupPrivilege) 失败: " + win32Message(GetLastError());
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr)) {
        error = L"AdjustTokenPrivileges 失败: " + win32Message(GetLastError());
        CloseHandle(token);
        return false;
    }
    CloseHandle(token);
    return true;
}

uint64_t getFileId(HANDLE handle) {
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) return 0;
    // A 64-bit NTFS File ID = low 8 bytes of FILE_ID_128.
    uint64_t low = 0;
    memcpy(&low, &info.FileId, sizeof(low));
    return low;
}

uint64_t tryGetRootRef(wchar_t drive) {
    std::wstring root(1, drive);
    root += L":\\";
    std::wstring error;
    HANDLE h = openForFileId(root, true, error);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint64_t id = getFileId(h);
    CloseHandle(h);
    return id;
}

bool tryGetVolumeInfo(wchar_t drive, NtfsVolumeInfo& info, std::wstring& error) {
    info = NtfsVolumeInfo{};
    error.clear();
    HANDLE volume = openVolume(drive, error);
    if (volume == INVALID_HANDLE_VALUE) return false;

    std::wstring root(1, drive);
    root += L":\\";
    DWORD serial = 0;
    if (!GetVolumeInformationW(root.c_str(), nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
        error = L"GetVolumeInformation(" + root + L") 失败: " + win32Message(GetLastError());
        CloseHandle(volume);
        return false;
    }

    uint64_t bpr = 1024;
    uint8_t outBuf[96] = {};
    DWORD returned = 0;
    std::wstring ioError;
    if (deviceIoControl(volume, kFsctlGetNtfsVolumeData, nullptr, 0, outBuf, sizeof(outBuf),
                        returned, ioError) && returned >= 56) {
        int32_t raw = 0;
        memcpy(&raw, outBuf + 48, 4);
        if (raw > 0) {
            bpr = static_cast<uint64_t>(raw);
        } else if (raw == 0) {
            uint32_t bytesPerCluster = 0, clusters = 0;
            memcpy(&bytesPerCluster, outBuf + 44, 4);
            memcpy(&clusters, outBuf + 52, 4);
            if (clusters != 0 && bytesPerCluster != 0) bpr = uint64_t(bytesPerCluster) * clusters;
        } else {
            bpr = 1ULL << (-raw);
        }
    }

    uint64_t journalId = 0;
    int64_t firstUsn = 0, nextUsn = 0;
    uint64_t journalMax = 0;
    uint8_t journalBuf[64] = {};
    if (deviceIoControl(volume, kFsctlQueryUsnJournal, nullptr, 0, journalBuf, sizeof(journalBuf),
                        returned, ioError) && returned >= 56) {
        memcpy(&journalId, journalBuf, 8);
        memcpy(&firstUsn, journalBuf + 8, 8);
        memcpy(&nextUsn, journalBuf + 16, 8);
        memcpy(&journalMax, journalBuf + 40, 8);
    }

    CloseHandle(volume);
    info.drive = drive;
    info.volumeSerial = serial;
    info.bytesPerRecord = bpr;
    info.journalId = journalId;
    info.firstUsn = firstUsn;
    info.nextUsn = nextUsn;
    info.journalMaxSize = journalMax;
    return true;
}

} // namespace ntfs_win32
