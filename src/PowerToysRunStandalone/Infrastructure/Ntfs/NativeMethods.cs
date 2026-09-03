using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace PowerToysRunStandalone.Infrastructure.Ntfs;

/// <summary>
/// Minimal Win32 P/Invoke surface for the NTFS indexer: volume &amp; $MFT
/// access, SeBackupPrivilege, USN journal, and directory walk helpers.
/// Everything is mirroring the approach proven in the "undex" tool.
/// </summary>
internal static class NativeMethods
{
    // ---- CreateFile ----
    public const uint GENERIC_READ = 0x80000000;
    public const uint FILE_READ_ATTRIBUTES = 0x00000080;
    public const uint FILE_SHARE_READ = 0x00000001;
    public const uint FILE_SHARE_WRITE = 0x00000002;
    public const uint FILE_SHARE_DELETE = 0x00000004;
    public const uint OPEN_EXISTING = 3;
    public const uint FILE_ATTRIBUTE_NORMAL = 0x00000080;
    public const uint FILE_ATTRIBUTE_DIRECTORY = 0x00000010;
    public const uint FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400;
    public const uint FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000;
    public const uint FILE_FLAG_BACKUP_SEMANTICS = 0x02000000;

    // ---- DeviceIoControl ----
    // CTL_CODE(FILE_DEVICE_FILE_SYSTEM=9, fn, METHOD, FILE_ANY_ACCESS=0)
    public const uint FSCTL_GET_NTFS_VOLUME_DATA = 0x00090064;
    public const uint FSCTL_ENUM_USN_DATA = 0x000900B3;
    public const uint FSCTL_READ_USN_JOURNAL = 0x000900BB;
    public const uint FSCTL_QUERY_USN_JOURNAL = 0x000900F4;

    // ---- USN reasons ----
    public const uint USN_REASON_DATA_OVERWRITE = 0x00000001;
    public const uint USN_REASON_DATA_EXTEND = 0x00000002;
    public const uint USN_REASON_FILE_CREATE = 0x00000100;
    public const uint USN_REASON_FILE_DELETE = 0x00000200;
    public const uint USN_REASON_RENAME_NEW_NAME = 0x00002000;
    public const uint USN_REASON_BASIC_INFO_CHANGE = 0x00008000;
    public const uint USN_REASON_CLOSE = 0x80000000;

    // ---- Errors ----
    public const uint ERROR_SUCCESS = 0;
    public const uint ERROR_FILE_NOT_FOUND = 2;
    public const uint ERROR_ACCESS_DENIED = 5;
    public const uint ERROR_JOURNAL_NOT_ACTIVE = 1179;
    public const uint ERROR_JOURNAL_DELETED = 1178;

    // ---- Token privileges ----
    public const uint TOKEN_ADJUST_PRIVILEGES = 0x0020;
    public const uint TOKEN_QUERY = 0x0008;
    public const uint SE_PRIVILEGE_ENABLED = 0x00000002;

    // ---- File info classes ----
    public const int FileIdInfo = 18;

    // ---- FindFirstFileEx ----
    public const int FindExInfoBasic = 1;
    public const int FindExSearchNameMatch = 0;
    public const int FIND_FIRST_EX_LARGE_FETCH = 2;

    [StructLayout(LayoutKind.Sequential)]
    public struct LUID
    {
        public uint LowPart;
        public int HighPart;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct LUID_AND_ATTRIBUTES
    {
        public LUID Luid;
        public uint Attributes;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct TOKEN_PRIVILEGES
    {
        public uint PrivilegeCount;
        public LUID_AND_ATTRIBUTES Privileges;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct FILE_ID_128
    {
        public ulong Low;
        public ulong High;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct FILE_ID_INFO
    {
        public ulong VolumeSerialNumber;
        public FILE_ID_128 FileId;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct WIN32_FIND_DATAW
    {
        public uint dwFileAttributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME ftCreationTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME ftLastAccessTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME ftLastWriteTime;
        public uint nFileSizeHigh;
        public uint nFileSizeLow;
        public uint dwReserved0;
        public uint dwReserved1;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string cFileName;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 14)]
        public string cAlternateFileName;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern SafeFileHandle CreateFileW(
        string lpFileName,
        uint dwDesiredAccess,
        uint dwShareMode,
        IntPtr lpSecurityAttributes,
        uint dwCreationDisposition,
        uint dwFlagsAndAttributes,
        IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool DeviceIoControl(
        SafeFileHandle hDevice,
        uint dwIoControlCode,
        byte[]? lpInBuffer,
        uint nInBufferSize,
        byte[] lpOutBuffer,
        uint nOutBufferSize,
        out uint lpBytesReturned,
        IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetVolumeInformationW(
        string lpRootPathName,
        StringBuilder? lpVolumeNameBuffer,
        uint nVolumeNameSize,
        out uint lpVolumeSerialNumber,
        out uint lpMaximumComponentLength,
        out uint lpFileSystemFlags,
        StringBuilder? lpFileSystemNameBuffer,
        uint nFileSystemNameSize);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetFileInformationByHandleEx(
        SafeFileHandle hFile,
        int fileInformationClass,
        out FILE_ID_INFO lpFileInformation,
        uint dwBufferSize);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool OpenProcessToken(
        IntPtr processHandle,
        uint desiredAccess,
        out IntPtr tokenHandle);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool LookupPrivilegeValueW(
        string? lpSystemName,
        string lpName,
        out LUID lpLuid);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool AdjustTokenPrivileges(
        IntPtr tokenHandle,
        [MarshalAs(UnmanagedType.Bool)] bool disableAllPrivileges,
        ref TOKEN_PRIVILEGES newState,
        uint bufferLength,
        IntPtr previousState,
        IntPtr returnLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr FindFirstFileExW(
        string lpFileName,
        int fInfoLevelId,
        out WIN32_FIND_DATAW lpFindFileData,
        int fSearchOp,
        IntPtr lpSearchFilter,
        int dwAdditionalFlags);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FindNextFileW(
        IntPtr hFindFile,
        ref WIN32_FIND_DATAW lpFindFileData);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FindClose(IntPtr hFindFile);

    /// <summary>Open a volume handle (needs administrator for $MFT modes; read-only access).</summary>
    public static SafeFileHandle? OpenVolume(char drive, out string? error)
    {
        var path = $@"\\.\{drive}:";
        var handle = CreateFileW(path, GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (handle.IsInvalid)
        {
            var code = Marshal.GetLastWin32Error();
            error = $"无法打开卷 {path}: {Win32Message(code)}";
            handle.Dispose();
            return null;
        }

        error = null;
        return handle;
    }

    /// <summary>Open \?\<drive>:\$MFT for sequential streaming reads.</summary>
    public static SafeFileHandle? OpenMft(char drive, out string? error)
    {
        var path = $@"\\?\{drive}:\$MFT";
        var handle = CreateFileW(path, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN, IntPtr.Zero);
        if (handle.IsInvalid)
        {
            var code = Marshal.GetLastWin32Error();
            error = $"无法打开 {path} (error {code}): {Win32Message(code)}";
            handle.Dispose();
            return null;
        }

        error = null;
        return handle;
    }

    /// <summary>Open a file/dir just to read its NTFS File ID.</summary>
    public static SafeFileHandle? OpenForFileId(string path, bool isDirectory, out string? error)
    {
        var flags = isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
        var handle = CreateFileW(path, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, IntPtr.Zero,
            OPEN_EXISTING, flags, IntPtr.Zero);
        if (handle.IsInvalid)
        {
            error = Win32Message(Marshal.GetLastWin32Error());
            handle.Dispose();
            return null;
        }

        error = null;
        return handle;
    }

    public static bool GetVolumeSerial(char drive, out uint serial, out string? error)
    {
        using var handle = OpenVolume(drive, out error);
        if (handle is null)
        {
            serial = 0;
            return false;
        }

        var root = $"{drive}:\\";
        if (!GetVolumeInformationW(root, null, 0, out serial, out _, out _, null, 0))
        {
            error = $"GetVolumeInformation({root}) 失败: {Win32Message(Marshal.GetLastWin32Error())}";
            return false;
        }

        error = null;
        return true;
    }

    public static bool TryEnableBackupPrivilege(out string? error)
    {
        error = null;
        var process = System.Diagnostics.Process.GetCurrentProcess();
        if (!OpenProcessToken(process.Handle, TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, out var token))
        {
            error = "OpenProcessToken 失败: " + Win32Message(Marshal.GetLastWin32Error());
            return false;
        }

        try
        {
            if (!LookupPrivilegeValueW(null, "SeBackupPrivilege", out var luid))
            {
                error = "LookupPrivilegeValue(SeBackupPrivilege) 失败: " + Win32Message(Marshal.GetLastWin32Error());
                return false;
            }

            var tp = new TOKEN_PRIVILEGES
            {
                PrivilegeCount = 1,
                Privileges = new LUID_AND_ATTRIBUTES { Luid = luid, Attributes = SE_PRIVILEGE_ENABLED },
            };

            if (!AdjustTokenPrivileges(token, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero))
            {
                error = "AdjustTokenPrivileges 失败: " + Win32Message(Marshal.GetLastWin32Error());
                return false;
            }

            // AdjustTokenPrivileges can succeed while the privilege was not actually
            // granted; the caller should still try to open $MFT and report the error.
            return true;
        }
        finally
        {
            CloseHandle(token);
        }
    }

    public static ulong GetFileId(SafeFileHandle handle)
    {
        if (!GetFileInformationByHandleEx(handle, FileIdInfo, out var info, (uint)Marshal.SizeOf<FILE_ID_INFO>()))
        {
            return 0;
        }

        // A 64-bit NTFS File ID is the low 8 bytes of FILE_ID_128: record number
        // in the low 48 bits, sequence number in the high 16 bits.
        return info.FileId.Low;
    }

    public static string Win32Message(int code)
    {
        try
        {
            return new Win32Exception((int)code).Message;
        }
        catch
        {
            return $"error {code}";
        }
    }
}
