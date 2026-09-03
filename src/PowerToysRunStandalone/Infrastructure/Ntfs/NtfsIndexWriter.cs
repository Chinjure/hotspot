using System.Buffers.Binary;
using System.IO;
using System.Runtime.InteropServices;

namespace PowerToysRunStandalone.Infrastructure.Ntfs;

/// <summary>
/// Writes the compact binary index used by the launcher.
/// Layout (little-endian):
/// <code>
///   index.dat = Header(64B) + Record[40B]*count + NameSection[u16 len + UTF-16LE]*
/// </code>
/// Records are written in probe order (MFT order or walk order); lookups use
/// an in-memory directory map instead of binary search, so no sorting is
/// required.
/// </summary>
internal sealed class NtfsIndexWriter : IDisposable
{
    private readonly string _directory;
    private readonly FileStream _records;
    private readonly FileStream _names;
    private long _fileCount;
    private long _dirCount;
    private long _namePosition;
    private bool _committed;

    public NtfsIndexWriter(string directory)
    {
        _directory = directory;
        Directory.CreateDirectory(directory);
        _records = new FileStream(Path.Combine(directory, "index-records.tmp"), FileMode.Create, FileAccess.ReadWrite, FileShare.None, 1 << 20);
        _names = new FileStream(Path.Combine(directory, "index-names.tmp"), FileMode.Create, FileAccess.ReadWrite, FileShare.None, 1 << 20);
    }

    public long RecordCount => _fileCount + _dirCount;

    public long FileCount => _fileCount;

    public long DirectoryCount => _dirCount;

    public void Append(in NtfsEntry entry)
    {
        var nameBytes = MemoryMarshal.AsBytes(entry.Name.AsSpan());

        // Fixed-size record.
        Span<byte> record = stackalloc byte[IndexHeader.RecordSize];
        BinaryPrimitives.WriteUInt64LittleEndian(record, entry.Ref);
        BinaryPrimitives.WriteUInt64LittleEndian(record.Slice(8), entry.ParentRef);
        BinaryPrimitives.WriteUInt64LittleEndian(record.Slice(16), entry.Size);
        BinaryPrimitives.WriteUInt64LittleEndian(record.Slice(24), entry.ModifiedTime);
        BinaryPrimitives.WriteUInt32LittleEndian(record.Slice(32), (uint)_namePosition);
        BinaryPrimitives.WriteUInt16LittleEndian(record.Slice(36), (ushort)nameBytes.Length);
        record[38] = entry.IsDirectory ? (byte)1 : (byte)0;
        record[39] = 0;
        _records.Write(record);

        // Name section: u16 byte length + UTF-16LE characters.
        Span<byte> length = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16LittleEndian(length, (ushort)nameBytes.Length);
        _names.Write(length);
        _names.Write(nameBytes);

        _namePosition += 2 + nameBytes.Length;
        if (entry.IsDirectory)
        {
            _dirCount++;
        }
        else
        {
            _fileCount++;
        }
    }

    /// <summary>Atomically publishes the index and returns the final path.</summary>
    public void Commit(NtfsIndexMeta meta, string indexPath)
    {
        _records.Flush();
        _names.Flush();

        var header = BuildHeader(meta);
        var temporary = indexPath + ".tmp";
        using (var final = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20, FileOptions.WriteThrough))
        {
            final.Write(header);
            _records.Position = 0;
            _records.CopyTo(final);
            _names.Position = 0;
            _names.CopyTo(final);
            final.Flush();
        }

        File.Move(temporary, indexPath, overwrite: true);
        _committed = true;
    }

    public void Abort()
    {
        _records.Dispose();
        _names.Dispose();
        TryDelete(Path.Combine(_directory, "index-records.tmp"));
        TryDelete(Path.Combine(_directory, "index-names.tmp"));
    }

    public void Dispose()
    {
        if (_committed)
        {
            _records.Dispose();
            _names.Dispose();
            TryDelete(Path.Combine(_directory, "index-records.tmp"));
            TryDelete(Path.Combine(_directory, "index-names.tmp"));
        }
        else
        {
            Abort();
        }
    }

    private static byte[] BuildHeader(NtfsIndexMeta meta)
    {
        var header = new byte[IndexHeader.Size];
        Array.Copy(new byte[] { (byte)'P', (byte)'R', (byte)'S', (byte)'I', (byte)'D', (byte)'X', (byte)'0', (byte)'1' }, 0, header, 0, 8);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(8), (uint)IndexHeader.Version);
        BinaryPrimitives.WriteUInt64LittleEndian(header.AsSpan(16), (ulong)meta.Epoch);
        BinaryPrimitives.WriteUInt64LittleEndian(header.AsSpan(24), (ulong)(meta.FileCount + meta.DirectoryCount));
        BinaryPrimitives.WriteUInt64LittleEndian(header.AsSpan(32), (ulong)(IndexHeader.Size + (meta.FileCount + meta.DirectoryCount) * IndexHeader.RecordSize));
        BinaryPrimitives.WriteUInt64LittleEndian(header.AsSpan(40), (ulong)meta.DirectoryCount);
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(48), meta.Drive);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(52), meta.BytesPerRecord);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(56), (uint)meta.Source);
        return header;
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch
        {
            // best effort
        }
    }
}
