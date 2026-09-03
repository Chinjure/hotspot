using System.Text.Json.Serialization;

namespace PowerToysRunStandalone.Infrastructure.Ntfs;

/// <summary>One logical file-system object produced by a scan or USN update.</summary>
public readonly struct NtfsEntry
{
    public ulong Ref { get; init; }

    public ulong ParentRef { get; init; }

    public ulong Size { get; init; }

    public ulong ModifiedTime { get; init; }

    public bool IsDirectory { get; init; }

    public string Name { get; init; }
}

/// <summary>Volume-level data collected before a scan.</summary>
public readonly struct NtfsVolumeInfo
{
    public char Drive { get; init; }

    public uint VolumeSerial { get; init; }

    public ulong BytesPerRecord { get; init; }

    public ulong JournalId { get; init; }

    public long FirstUsn { get; init; }

    public long NextUsn { get; init; }

    public ulong JournalMaxSize { get; init; }
}

/// <summary>A search hit from the NTFS index.</summary>
public sealed record NtfsSearchResult(string Name, string Path, bool IsDirectory, ulong Size, ulong ModifiedTime, ulong Ref);

/// <summary>Persisted metadata for one volume index.</summary>
public sealed class NtfsIndexMeta
{
    public int Version { get; set; } = 1;

    public char Drive { get; set; }

    public uint VolumeSerial { get; set; }

    public long Epoch { get; set; }

    public long FileCount { get; set; }

    public long DirectoryCount { get; set; }

    public long LastScanTimeUtcTicks { get; set; }

    public ulong RootRef { get; set; }

    public uint BytesPerRecord { get; set; }

    /// <summary>0 = $MFT scan, 1 = directory walk.</summary>
    public int Source { get; set; }

    public ulong JournalId { get; set; }

    public long NextUsn { get; set; }

    [JsonIgnore]
    public DateTime LastScanTimeUtc => new(LastScanTimeUtcTicks, DateTimeKind.Utc);
}

/// <summary>USN delta operations (mirrors undex's delta semantics).</summary>
public enum DeltaOp : byte
{
    Add = 1,
    Delete = 2,
    Rename = 3,
    Update = 4,
}

/// <summary>One change record produced by a USN journal read.</summary>
public sealed class DeltaRecord
{
    public DeltaOp Op { get; set; }

    public bool IsDirectory { get; set; }

    public ulong Ref { get; set; }

    public ulong ParentRef { get; set; }

    public ulong Size { get; set; }

    public ulong ModifiedTime { get; set; }

    public string Name { get; set; } = string.Empty;
}

/// <summary>The delta file content persisted next to a base index.</summary>
public sealed class DeltaData
{
    public long Epoch { get; set; }

    public List<DeltaRecord> Records { get; set; } = new();
}

/// <summary>
/// In-memory view of a delta file: overlays applied on top of a base index
/// while searching, exactly like undex's ForEachEffective/DirMap.
/// </summary>
public sealed class DeltaState
{
    public Dictionary<ulong, DeltaRecord> FileOverlay { get; } = new();

    public HashSet<ulong> DeletedFiles { get; } = new();

    public List<DeltaRecord> Added { get; } = new();

    public Dictionary<ulong, DeltaRecord> DirOverlay { get; } = new();

    public HashSet<ulong> DeletedDirectories { get; } = new();

    public static DeltaState Build(IEnumerable<DeltaRecord> records)
    {
        var state = new DeltaState();
        foreach (var d in records)
        {
            var key = d.Ref & 0xFFFFFFFFFFFFUL;
            if (d.Op == DeltaOp.Delete)
            {
                if (d.IsDirectory)
                {
                    state.DeletedDirectories.Add(key);
                }
                else
                {
                    state.DeletedFiles.Add(key);
                }

                continue;
            }

            if (d.Op == DeltaOp.Add)
            {
                state.Added.Add(d);
                if (d.IsDirectory)
                {
                    // New directories must be resolvable so files under them
                    // can rebuild their full path.
                    state.DirOverlay[key] = d;
                }

                continue;
            }

            if (d.IsDirectory)
            {
                state.DirOverlay[key] = d;
            }
            else
            {
                state.FileOverlay[key] = d;
            }
        }

        return state;
    }
}

/// <summary>Binary header of index.dat (64 bytes, little-endian).</summary>
internal sealed class IndexHeader
{
    public const int Size = 64;
    public const int RecordSize = 40;
    public const long Version = 1;

    public byte[] Magic { get; } = { (byte)'P', (byte)'R', (byte)'S', (byte)'I', (byte)'D', (byte)'X', (byte)'0', (byte)'1' };

    public long Epoch { get; set; }

    public long RecordCount { get; set; }

    public long NameSectionOffset { get; set; }

    public long DirectoryCount { get; set; }

    public char Drive { get; set; }

    public uint BytesPerRecord { get; set; }

    public int Source { get; set; }
}
