using System.Buffers.Binary;
using System.IO;
using System.Text;
using System.Text.Json;

namespace PowerToysRunStandalone.Infrastructure.Ntfs;

/// <summary>
/// USN Journal incremental update. Reads FSCTL_READ_USN_JOURNAL since the
/// last baseline (administrator/SYSTEM only) and appends add/delete/rename/
/// update records into the delta file that overlays the base index.
/// </summary>
internal static class UsnUpdater
{
    public static bool TryUpdate(
        string indexDirectory,
        char drive,
        NtfsIndexMeta meta,
        out List<DeltaRecord> newRecords,
        out long nextUsn,
        out string error)
    {
        newRecords = new List<DeltaRecord>();
        nextUsn = meta.NextUsn;
        error = string.Empty;

        using var volume = NativeMethods.OpenVolume(drive, out var openError);
        if (volume is null)
        {
            error = openError ?? "打开卷失败";
            return false;
        }

        // ---- query current journal ----
        var journalBuffer = new byte[64];
        if (!NativeMethods.DeviceIoControl(volume, NativeMethods.FSCTL_QUERY_USN_JOURNAL,
                Array.Empty<byte>(), 0, journalBuffer, (uint)journalBuffer.Length, out var jb, IntPtr.Zero)
            || jb < 56)
        {
            error = "USN journal 不可用: " + NativeMethods.Win32Message(System.Runtime.InteropServices.Marshal.GetLastWin32Error());
            return false;
        }

        var currentJournalId = BitConverter.ToUInt64(journalBuffer, 0);
        var currentNextUsn = BitConverter.ToInt64(journalBuffer, 16);

        if (meta.JournalId == 0)
        {
            // Walk-built index has no baseline: calibrate, don't try to replay history.
            meta.JournalId = currentJournalId;
            meta.NextUsn = currentNextUsn;
            meta.LastScanTimeUtcTicks = DateTime.UtcNow.Ticks;
            return true;
        }

        if (meta.JournalId != currentJournalId)
        {
            error = "USN journal 已被重建，需要全量重建索引";
            return false;
        }

        // ---- read journal since baseline ----
        var pending = new Dictionary<ulong, DeltaRecord>();
        long usn = meta.NextUsn;
        const int bufferSize = 1024 * 1024;
        while (true)
        {
            var input = new byte[40];
            BinaryPrimitives.WriteUInt64LittleEndian(input.AsSpan(0), (ulong)usn);
            BinaryPrimitives.WriteUInt32LittleEndian(input.AsSpan(8), 0xFFFFFFFF); // ReasonMask
            BinaryPrimitives.WriteUInt32LittleEndian(input.AsSpan(12), 0);         // ReturnOnlyOnClose
            BinaryPrimitives.WriteUInt64LittleEndian(input.AsSpan(16), 0);         // Timeout
            BinaryPrimitives.WriteUInt64LittleEndian(input.AsSpan(24), 0);         // BytesToWaitFor
            BinaryPrimitives.WriteUInt64LittleEndian(input.AsSpan(32), currentJournalId);

            var output = new byte[bufferSize];
            if (!NativeMethods.DeviceIoControl(volume, NativeMethods.FSCTL_READ_USN_JOURNAL,
                    input, (uint)input.Length, output, (uint)output.Length, out var bytes, IntPtr.Zero))
            {
                var code = (uint)System.Runtime.InteropServices.Marshal.GetLastWin32Error();
                error = code switch
                {
                    NativeMethods.ERROR_JOURNAL_DELETED => "USN journal 已被删除，需要全量重建索引",
                    NativeMethods.ERROR_JOURNAL_NOT_ACTIVE => "USN journal 未启用",
                    _ => "读取 USN journal 失败: " + NativeMethods.Win32Message((int)code),
                };
                return false;
            }

            if (bytes < sizeof(long))
            {
                break;
            }

            var next = BitConverter.ToInt64(output, 0);
            if (next == usn)
            {
                break;
            }

            var offset = sizeof(long);
            while (offset + 8 <= bytes)
            {
                var recordLength = BitConverter.ToUInt32(output, offset);
                if (recordLength == 0 || offset + recordLength > bytes)
                {
                    break;
                }

                var majorVersion = BitConverter.ToUInt16(output, offset + 4);
                if (majorVersion >= 2)
                {
                    ApplyRecord(output, offset, pending);
                }

                offset += (int)recordLength;
            }

            usn = next;
        }

        // ---- persist ----
        var deltaPath = DeltaPath(indexDirectory, drive);
        var existing = LoadDelta(indexDirectory, drive, meta.Epoch);
        var pendingRecords = pending.Values.ToList();
        existing.Records.AddRange(pendingRecords);
        SaveDelta(deltaPath, existing);

        newRecords = pendingRecords;
        nextUsn = usn;
        meta.NextUsn = nextUsn;
        meta.LastScanTimeUtcTicks = DateTime.UtcNow.Ticks;
        return true;
    }

    public static DeltaData LoadDelta(string indexDirectory, char drive, long expectEpoch)
    {
        var path = DeltaPath(indexDirectory, drive);
        try
        {
            if (!File.Exists(path))
            {
                return new DeltaData { Epoch = expectEpoch };
            }

            var data = JsonSerializer.Deserialize<DeltaData>(File.ReadAllText(path));
            if (data is null || data.Epoch != expectEpoch)
            {
                return new DeltaData { Epoch = expectEpoch };
            }

            return data;
        }
        catch
        {
            return new DeltaData { Epoch = expectEpoch };
        }
    }

    public static void SaveDelta(string path, DeltaData data)
    {
        var directory = Path.GetDirectoryName(path)!;
        Directory.CreateDirectory(directory);
        var temporary = path + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(data));
        File.Move(temporary, path, overwrite: true);
    }

    public static string DeltaPath(string indexDirectory, char drive)
    {
        return Path.Combine(indexDirectory, $"{drive}-delta.json");
    }

    private static void ApplyRecord(byte[] buffer, int offset, Dictionary<ulong, DeltaRecord> pending)
    {
        var fileRef = BitConverter.ToUInt64(buffer, offset + 8);
        var parentRef = BitConverter.ToUInt64(buffer, offset + 16);
        var timestamp = BitConverter.ToUInt64(buffer, offset + 32);
        var reason = BitConverter.ToUInt32(buffer, offset + 40);
        var attributes = BitConverter.ToUInt32(buffer, offset + 52);
        var nameLength = BitConverter.ToUInt16(buffer, offset + 56);
        var nameOffset = BitConverter.ToUInt16(buffer, offset + 58);
        var name = nameLength > 0 && offset + nameOffset + nameLength <= buffer.Length
            ? Encoding.Unicode.GetString(buffer, offset + nameOffset, nameLength)
            : string.Empty;

        var key = fileRef & 0xFFFFFFFFFFFFUL;
        var isDirectory = (attributes & NativeMethods.FILE_ATTRIBUTE_DIRECTORY) != 0;

        if ((reason & NativeMethods.USN_REASON_FILE_DELETE) != 0)
        {
            pending[key] = new DeltaRecord
            {
                Op = DeltaOp.Delete,
                IsDirectory = isDirectory,
                Ref = fileRef,
                ParentRef = parentRef,
                ModifiedTime = timestamp,
                Name = name,
            };
        }
        else if ((reason & NativeMethods.USN_REASON_FILE_CREATE) != 0)
        {
            pending[key] = new DeltaRecord
            {
                Op = DeltaOp.Add,
                IsDirectory = isDirectory,
                Ref = fileRef,
                ParentRef = parentRef,
                Size = 0,
                ModifiedTime = timestamp,
                Name = name,
            };
        }
        else if ((reason & NativeMethods.USN_REASON_RENAME_NEW_NAME) != 0)
        {
            pending[key] = new DeltaRecord
            {
                Op = DeltaOp.Rename,
                IsDirectory = isDirectory,
                Ref = fileRef,
                ParentRef = parentRef,
                ModifiedTime = timestamp,
                Name = name,
            };
        }
        else if ((reason & NativeMethods.USN_REASON_CLOSE) != 0)
        {
            if (pending.TryGetValue(key, out var existing))
            {
                existing.ModifiedTime = timestamp;
            }
            else
            {
                pending[key] = new DeltaRecord
                {
                    Op = DeltaOp.Update,
                    IsDirectory = isDirectory,
                    Ref = fileRef,
                    ParentRef = parentRef,
                    ModifiedTime = timestamp,
                    Name = string.Empty, // merge keeps the base name
                };
            }
        }
    }
}
