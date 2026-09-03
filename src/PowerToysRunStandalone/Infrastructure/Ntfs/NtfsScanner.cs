using System.Buffers.Binary;
using System.IO;
using System.Text;

namespace PowerToysRunStandalone.Infrastructure.Ntfs;

/// <summary>
/// Reconstructs file-system contents directly from NTFS on-disk structures.
/// Primary mode reads $MFT and rebuilds full paths from parent-reference
/// chains; when $MFT is inaccessible (Win11 24H2+ admin protection, or a
/// non-elevated process) it falls back to a parallel directory walk that
/// records each entry's NTFS File ID, exactly like undex.
/// </summary>
internal static class NtfsScanner
{
    private const int SystemRecordCount = 24;
    private const ulong RootDirectoryRecord = 5;
    private const int MaxDirectoryDepth = 128;

    public static IReadOnlyList<char> GetFixedNtfsDrives()
    {
        var drives = new List<char>();
        try
        {
            foreach (var drive in DriveInfo.GetDrives())
            {
                if (drive.DriveType != DriveType.Fixed || !drive.IsReady)
                {
                    continue;
                }

                if (!string.Equals(drive.DriveFormat, "NTFS", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                drives.Add(drive.Name[0]);
            }
        }
        catch
        {
            // DriveInfo can throw for the odd disconnected volume.
        }

        return drives;
    }

    public static bool TryGetVolumeInfo(char drive, out NtfsVolumeInfo info, out string error)
    {
        info = default;
        error = string.Empty;

        using var volume = NativeMethods.OpenVolume(drive, out var openError);
        if (volume is null)
        {
            error = openError ?? "打开卷失败";
            return false;
        }

        if (!NativeMethods.GetVolumeSerial(drive, out var serial, out var serialError))
        {
            error = serialError ?? "获取卷序列号失败";
            return false;
        }

        var bpr = 1024UL;
        var outBuffer = new byte[96];
        if (NativeMethods.DeviceIoControl(volume, NativeMethods.FSCTL_GET_NTFS_VOLUME_DATA,
                Array.Empty<byte>(), 0, outBuffer, (uint)outBuffer.Length, out var returned, IntPtr.Zero)
            && returned >= 56)
        {
            var raw = BitConverter.ToInt32(outBuffer, 48); // BytesPerFileRecordSegment
            if (raw > 0)
            {
                bpr = (ulong)raw;
            }
            else if (raw == 0)
            {
                var bytesPerCluster = BitConverter.ToUInt32(outBuffer, 44);
                var clusters = BitConverter.ToUInt32(outBuffer, 52);
                if (clusters != 0 && bytesPerCluster != 0)
                {
                    bpr = (ulong)bytesPerCluster * clusters;
                }
            }
            else
            {
                bpr = 1UL << -raw;
            }
        }

        // USN journal info (best effort; some systems reject it without admin).
        ulong journalId = 0;
        long firstUsn = 0, nextUsn = 0;
        ulong journalMax = 0;
        var journalBuffer = new byte[64];
        if (NativeMethods.DeviceIoControl(volume, NativeMethods.FSCTL_QUERY_USN_JOURNAL,
                Array.Empty<byte>(), 0, journalBuffer, (uint)journalBuffer.Length, out var jr, IntPtr.Zero)
            && jr >= 56)
        {
            journalId = BitConverter.ToUInt64(journalBuffer, 0);
            firstUsn = BitConverter.ToInt64(journalBuffer, 8);
            nextUsn = BitConverter.ToInt64(journalBuffer, 16);
            journalMax = BitConverter.ToUInt64(journalBuffer, 40);
        }

        info = new NtfsVolumeInfo
        {
            Drive = drive,
            VolumeSerial = serial,
            BytesPerRecord = bpr,
            JournalId = journalId,
            FirstUsn = firstUsn,
            NextUsn = nextUsn,
            JournalMaxSize = journalMax,
        };
        return true;
    }

    // ================= $MFT scan =================

    /// <summary>
    /// Streams all active <c>$MFT</c> FILE records. Returns the number of
    /// records visited; <paramref name="onEntry"/> gets one call per active
    /// user file/directory. Requires SeBackupPrivilege (administrator) on
    /// most systems.
    /// </summary>
    public static long ScanMft(char drive, Action<NtfsEntry> onEntry, Action<long> onProgress, CancellationToken ct, out string error)
    {
        error = string.Empty;
        if (!TryGetVolumeInfo(drive, out var info, out var infoError))
        {
            error = infoError;
            return 0;
        }

        var bpr = info.BytesPerRecord;
        if (bpr == 0 || bpr > 1 << 16)
        {
            error = $"MFT 记录大小异常: {bpr}";
            return 0;
        }

        NativeMethods.TryEnableBackupPrivilege(out var privilegeError);

        using var mft = NativeMethods.OpenMft(drive, out var mftError);
        if (mft is null)
        {
            error = mftError ?? "无法打开 $MFT" +
                    (string.IsNullOrEmpty(privilegeError) ? string.Empty : $" (SeBackupPrivilege: {privilegeError})");
            return 0;
        }

        const int bufferSize = 1024 * 1024;
        var buffer = new byte[bufferSize];
        long filePosition = 0;
        long visited = 0;

        using var stream = new FileStream(mft, FileAccess.Read, bufferSize, false);
        long totalRecords = 0;
        try
        {
            totalRecords = stream.Length / (long)bpr;
        }
        catch
        {
            // Ignore; progress just won't show a total.
        }

        while (!ct.IsCancellationRequested)
        {
            int read = stream.Read(buffer, 0, bufferSize);
            if (read == 0)
            {
                break;
            }

            int offset = 0;
            while (offset + (int)bpr <= read)
            {
                ct.ThrowIfCancellationRequested();
                long recordNumber = (filePosition + offset) / (long)bpr;
                if (TryParseMftRecord(buffer.AsSpan(offset, (int)bpr), recordNumber, out var entry))
                {
                    onEntry(entry);
                }

                visited++;
                offset += (int)bpr;
            }

            filePosition += read;
            if (visited % 200_000 == 0)
            {
                onProgress(visited);
            }
        }

        onProgress(visited);
        return visited;
    }

    /// <summary>
    /// Parses one NTFS FILE record. Layout mirrors undex's mftscan.cpp:
    /// attribute 0x30 FILE_NAME provides name/parent/size/mtime, and a plain
    /// file must carry a 0x80 DATA attribute.
    /// </summary>
    private static bool TryParseMftRecord(ReadOnlySpan<byte> record, long recordNumber, out NtfsEntry entry)
    {
        entry = default;
        if (record.Length < 0x30 ||
            record[0] != (byte)'F' || record[1] != (byte)'I' ||
            record[2] != (byte)'L' || record[3] != (byte)'E')
        {
            return false;
        }

        var flags = BinaryPrimitives.ReadUInt16LittleEndian(record.Slice(0x16, 2));
        if ((flags & 0x0001) == 0)
        {
            return false; // deleted record
        }

        if (recordNumber < SystemRecordCount)
        {
            return false; // $MFT/$Bitmap/etc.
        }

        var baseReference = BinaryPrimitives.ReadUInt64LittleEndian(record.Slice(0x20, 8));
        if (baseReference != 0)
        {
            return false; // extension record of an over-64KB file
        }

        var attrsOffset = BinaryPrimitives.ReadUInt16LittleEndian(record.Slice(0x14, 2));
        var usedSize = BinaryPrimitives.ReadUInt32LittleEndian(record.Slice(0x18, 4));
        if (attrsOffset < 0x28 || attrsOffset >= usedSize || usedSize > record.Length)
        {
            return false;
        }

        var sequence = BinaryPrimitives.ReadUInt16LittleEndian(record.Slice(0x10, 2));
        var isDirectory = (flags & 0x0002) != 0;

        bool hasData = false;
        bool hasName = false;
        int bestNamespace = -1;
        ulong parentRef = 0, size = 0, modifiedTime = 0;
        string name = string.Empty;

        uint offset = attrsOffset;
        while (offset + 0x18 <= usedSize)
        {
            var attr = record.Slice((int)offset);
            var type = BinaryPrimitives.ReadUInt32LittleEndian(attr.Slice(0, 4));
            var length = BinaryPrimitives.ReadUInt32LittleEndian(attr.Slice(4, 4));
            if (type == 0xFFFFFFFF || length < 0x18 || offset + length > usedSize)
            {
                break;
            }

            var nonResident = attr[0x08];
            var nameLength = attr[0x09];

            if (type == 0x30 && nameLength == 0 && nonResident == 0)
            {
                // FILE_NAME is always resident.
                var valueLength = BinaryPrimitives.ReadUInt32LittleEndian(attr.Slice(0x10, 4));
                var valueOffset = BinaryPrimitives.ReadUInt16LittleEndian(attr.Slice(0x14, 2));
                var value = attr.Slice(valueOffset, (int)Math.Min(valueLength, (uint)attr.Length - valueOffset));
                if (value.Length >= 0x42)
                {
                    byte fileNameLength = value[0x40];
                    byte fileNameNamespace = value[0x41];
                    if (fileNameLength > 0 && fileNameLength <= 255 && 0x42 + (uint)fileNameLength * 2 <= value.Length)
                    {
                        // Namespace preference: Win32 (1) > Win32&DOS (3) > DOS (2) > POSIX (0).
                        int priority = fileNameNamespace switch
                        {
                            1 => 3,
                            3 => 2,
                            2 => 1,
                            _ => 0,
                        };

                        if (priority > bestNamespace)
                        {
                            parentRef = BinaryPrimitives.ReadUInt64LittleEndian(value.Slice(0x00, 8));
                            size = BinaryPrimitives.ReadUInt64LittleEndian(value.Slice(0x30, 8));
                            modifiedTime = BinaryPrimitives.ReadUInt64LittleEndian(value.Slice(0x10, 8));
                            name = Encoding.Unicode.GetString(value.Slice(0x42, fileNameLength * 2));
                            bestNamespace = priority;
                            hasName = true;
                        }
                    }
                }
            }
            else if (type == 0x80 && nameLength == 0)
            {
                hasData = true;
            }

            offset += length;
        }

        if (!hasName)
        {
            return false;
        }

        if (!isDirectory && !hasData)
        {
            return false; // special files (reparse points etc.) are skipped
        }

        entry = new NtfsEntry
        {
            Ref = (ulong)recordNumber | ((ulong)sequence << 48),
            ParentRef = parentRef,
            Size = size,
            ModifiedTime = modifiedTime,
            IsDirectory = isDirectory,
            Name = name,
        };
        return true;
    }

    // ================= walk fallback =================

    /// <summary>
    /// Full recursive directory walk. No administrator required, slower than
    /// the $MFT scan, but works when $MFT access is blocked. Each entry's NTFS
    /// File ID is captured so the produced index is USN-compatible.
    /// </summary>
    public static long WalkVolume(char drive, Action<NtfsEntry> onEntry, Action<long> onProgress, CancellationToken ct, out string error)
    {
        error = string.Empty;
        var root = $"{drive}:\\";
        var rootRef = GetFileIdForPath(root, isDirectory: true);
        if (rootRef == 0)
        {
            error = $"无法获取卷根目录文件 ID: {root}";
            return 0;
        }

        var threadCount = Math.Clamp(Environment.ProcessorCount, 2, 8);
        var queue = new System.Collections.Concurrent.ConcurrentQueue<(string Path, ulong ParentRef, int Depth)>();
        queue.Enqueue((root, rootRef, 0));

        var localResults = new System.Collections.Concurrent.ConcurrentBag<List<NtfsEntry>>();
        var pending = 1;
        var finished = new ManualResetEventSlim(false);

        var workers = new Task[threadCount];
        for (var i = 0; i < threadCount; i++)
        {
            workers[i] = Task.Run(() =>
            {
                var local = new List<NtfsEntry>();
                while (true)
                {
                    if (ct.IsCancellationRequested)
                    {
                        break;
                    }

                    if (!queue.TryDequeue(out var item))
                    {
                        if (Volatile.Read(ref pending) == 0 || ct.IsCancellationRequested)
                        {
                            break;
                        }

                        Thread.Sleep(10);
                        continue;
                    }

                    var subs = new List<(string Path, ulong ParentRef, int Depth)>();
                    WalkDirectory(item.Path, item.ParentRef, item.Depth, local, subs);
                    if (subs.Count > 0)
                    {
                        Interlocked.Add(ref pending, subs.Count);
                        foreach (var sub in subs)
                        {
                            queue.Enqueue(sub);
                        }
                    }

                    if (Interlocked.Decrement(ref pending) == 0)
                    {
                        finished.Set();
                        break;
                    }
                }

                localResults.Add(local);
            });
        }

        Task.WaitAll(workers);
        finished.Set();

        long total = 0;
        long emitted = 0;
        foreach (var list in localResults)
        {
            foreach (var entry in list)
            {
                ct.ThrowIfCancellationRequested();
                onEntry(entry);
                emitted++;
                if (emitted % 50_000 == 0)
                {
                    onProgress(emitted);
                }
            }

            total += list.Count;
        }

        onProgress(total);
        return total;
    }

    private static void WalkDirectory(
        string directoryPath,
        ulong parentRef,
        int depth,
        List<NtfsEntry> local,
        List<(string Path, ulong ParentRef, int Depth)> subDirectories)
    {
        if (depth > MaxDirectoryDepth)
        {
            return;
        }

        var pattern = directoryPath + "\\*";
        var handle = NativeMethods.FindFirstFileExW(
            pattern, NativeMethods.FindExInfoBasic, out var data,
            NativeMethods.FindExSearchNameMatch, IntPtr.Zero, NativeMethods.FIND_FIRST_EX_LARGE_FETCH);
        if (handle == IntPtr.Zero || handle == new IntPtr(-1))
        {
            return; // empty / access denied
        }

        try
        {
            do
            {
                var name = data.cFileName;
                if (string.IsNullOrEmpty(name) || name == "." || name == "..")
                {
                    continue;
                }

                if ((data.dwFileAttributes & NativeMethods.FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                {
                    continue; // junctions/symlinks: avoid cycles
                }

                var isDirectory = (data.dwFileAttributes & NativeMethods.FILE_ATTRIBUTE_DIRECTORY) != 0;
                var fullPath = directoryPath + "\\" + name;
                var refValue = GetFileIdForPath(fullPath, isDirectory);
                if (refValue == 0)
                {
                    continue; // ACL denied etc.
                }

                ulong sizeHigh = data.nFileSizeHigh;
                ulong sizeLow = data.nFileSizeLow;
                ulong timeHigh = (uint)data.ftLastWriteTime.dwHighDateTime;
                ulong timeLow = (uint)data.ftLastWriteTime.dwLowDateTime;
                var size = (sizeHigh << 32) | (sizeLow & 0xFFFFFFFFUL);
                var modified = (timeHigh << 32) | (timeLow & 0xFFFFFFFFUL);
                local.Add(new NtfsEntry
                {
                    Ref = refValue,
                    ParentRef = parentRef,
                    Size = size,
                    ModifiedTime = modified,
                    IsDirectory = isDirectory,
                    Name = name,
                });

                if (isDirectory && depth < MaxDirectoryDepth)
                {
                    subDirectories.Add((fullPath, refValue, depth + 1));
                }
            }
            while (NativeMethods.FindNextFileW(handle, ref data));
        }
        finally
        {
            NativeMethods.FindClose(handle);
        }
    }

    private static ulong GetFileIdForPath(string path, bool isDirectory)
    {
        using var handle = NativeMethods.OpenForFileId(path, isDirectory, out _);
        if (handle is null)
        {
            return 0;
        }

        return NativeMethods.GetFileId(handle);
    }

    /// <summary>Resolves the NTFS File ID of a volume root (used by walk mode).</summary>
    public static ulong TryGetRootRef(char drive)
    {
        return GetFileIdForPath($"{drive}:\\", isDirectory: true);
    }
}
