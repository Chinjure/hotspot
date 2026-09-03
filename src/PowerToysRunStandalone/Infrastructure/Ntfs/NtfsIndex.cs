using System.Collections.Concurrent;
using System.IO;
using System.IO.MemoryMappedFiles;
using Microsoft.Win32.SafeHandles;

namespace PowerToysRunStandalone.Infrastructure.Ntfs;

/// <summary>
/// Read-only in-memory view of one volume index (memory-mapped). Supports
/// allocation-free substring search over all file names and lazy full-path
/// reconstruction from the NTFS parent-reference chains. USN delta records
/// are applied as an overlay so results reflect recent changes without
/// rewriting the base index.
/// </summary>
internal sealed unsafe class NtfsIndex : IDisposable
{
    private readonly MemoryMappedFile _mappedFile;
    private readonly MemoryMappedViewAccessor _accessor;
    private readonly SafeMemoryMappedViewHandle _viewHandle;
    private byte* _base = null;

    private readonly NtfsIndexMeta _meta;
    private long _recordCount;
    private long _dirCount;
    private long _nameSectionOffset;
    private readonly string _driveRoot;
    private readonly ulong _rootRef;
    private readonly Dictionary<ulong, DirEntry> _dirs;
    private readonly ConcurrentDictionary<ulong, string> _pathMemo = new();
    private readonly DeltaState? _delta;

    private NtfsIndex(string indexPath, NtfsIndexMeta meta, DeltaData? delta)
    {
        _meta = meta;
        _mappedFile = MemoryMappedFile.CreateFromFile(indexPath, FileMode.Open, null, 0, MemoryMappedFileAccess.Read);
        _accessor = _mappedFile.CreateViewAccessor(0, 0, MemoryMappedFileAccess.Read);
        _viewHandle = _accessor.SafeMemoryMappedViewHandle;
        _viewHandle.AcquirePointer(ref _base);

        if (!TryReadHeader())
        {
            _viewHandle.ReleasePointer();
            _accessor.Dispose();
            _mappedFile.Dispose();
            throw new InvalidDataException("索引头无效: " + indexPath);
        }

        _driveRoot = $"{meta.Drive}:\\";
        _rootRef = meta.RootRef;
        _dirs = new Dictionary<ulong, DirEntry>((int)Math.Min(_dirCount, 1_000_000));
        BuildDirectoryMap();
        _delta = delta is { Records.Count: > 0 } ? DeltaState.Build(delta.Records) : null;
    }

    public static NtfsIndex? TryLoad(string indexPath, NtfsIndexMeta meta, DeltaData? delta)
    {
        try
        {
            return new NtfsIndex(indexPath, meta, delta);
        }
        catch (Exception ex)
        {
            try
            {
                var directory = Path.GetDirectoryName(indexPath) ?? Path.GetTempPath();
                File.AppendAllText(Path.Combine(directory, "load-error.log"),
                    $"{DateTime.Now:O} {indexPath}: {ex}{Environment.NewLine}");
            }
            catch
            {
                // ignore
            }

            return null;
        }
    }

    public NtfsIndexMeta Meta => _meta;

    public long RecordCount => _recordCount;

    public bool HasDelta => _delta is not null;

    public IReadOnlyList<NtfsSearchResult> Search(string needle, int maxResults, CancellationToken cancellationToken)
    {
        var found = new List<(int Rank, NtfsSearchResult Item)>();
        if (string.IsNullOrEmpty(needle) || maxResults <= 0)
        {
            return Array.Empty<NtfsSearchResult>();
        }

        // Everything-style relevance: exact name > prefix > substring, then
        // shorter names first. Candidates are bounded to keep broad queries
        // cheap while still ranking folder hits like "The Church" to the top.
        var candidateLimit = Math.Max(2000, maxResults * 10);
        var needleSpan = needle.AsSpan();
        byte* records = _base + IndexHeader.Size;
        byte* names = _base + _nameSectionOffset;

        for (long i = 0; i < _recordCount && found.Count < candidateLimit; i++)
        {
            if ((i & 0x3FFF) == 0)
            {
                cancellationToken.ThrowIfCancellationRequested();
            }

            byte* record = records + i * IndexHeader.RecordSize;
            var refValue = *(ulong*)record;
            var parentRef = *(ulong*)(record + 8);
            var size = *(ulong*)(record + 16);
            var modifiedTime = *(ulong*)(record + 24);
            var nameOffset = *(uint*)(record + 32);
            var nameLenBytes = *(ushort*)(record + 36);
            var isDirectory = record[38] != 0;
            var key = refValue & 0xFFFFFFFFFFFFUL;

            string? effectiveName = null;
            if (_delta is not null)
            {
                if (isDirectory)
                {
                    if (_delta.DeletedDirectories.Contains(key))
                    {
                        continue;
                    }

                    if (_delta.DirOverlay.TryGetValue(key, out var dirMod))
                    {
                        // Rename/Add carry a fresh name; plain Update keeps the base name.
                        if (dirMod.Op is DeltaOp.Rename or DeltaOp.Add)
                        {
                            effectiveName = dirMod.Name;
                        }

                        parentRef = dirMod.ParentRef;
                    }
                }
                else
                {
                    if (_delta.DeletedFiles.Contains(key))
                    {
                        continue;
                    }

                    if (_delta.FileOverlay.TryGetValue(key, out var fileMod))
                    {
                        parentRef = fileMod.ParentRef;
                        modifiedTime = fileMod.ModifiedTime;
                        if (fileMod.Op == DeltaOp.Rename)
                        {
                            effectiveName = fileMod.Name;
                        }
                    }
                }
            }

            bool matched;
            string? baseName = null;
            if (effectiveName is not null)
            {
                matched = effectiveName.Contains(needle, StringComparison.OrdinalIgnoreCase);
            }
            else
            {
                byte* namePointer = names + nameOffset;
                var storedLen = *(ushort*)namePointer;
                if (storedLen != nameLenBytes || (storedLen & 1) != 0)
                {
                    continue; // corrupt entry
                }

                var nameSpan = new ReadOnlySpan<char>((char*)(namePointer + 2), storedLen / 2);
                matched = nameSpan.IndexOf(needleSpan, StringComparison.OrdinalIgnoreCase) >= 0;
                if (matched)
                {
                    baseName = nameSpan.ToString();
                }
            }

            if (!matched)
            {
                continue;
            }

            var displayName = effectiveName ?? baseName!;
            string path;
            if (isDirectory)
            {
                path = ResolvePath(key);
            }
            else
            {
                var directory = ResolvePath(parentRef);
                path = directory.EndsWith('\\') ? directory + displayName : directory + "\\" + displayName;
            }

            found.Add((ComputeRank(displayName, needle), new NtfsSearchResult(displayName, path, isDirectory, size, modifiedTime, refValue)));
        }

        // New files/directories recorded after the base snapshot.
        if (_delta is not null)
        {
            foreach (var add in _delta.Added)
            {
                if (found.Count >= candidateLimit)
                {
                    break;
                }

                var addKey = add.Ref & 0xFFFFFFFFFFFFUL;
                if ((add.IsDirectory && _delta.DeletedDirectories.Contains(addKey)) ||
                    (!add.IsDirectory && _delta.DeletedFiles.Contains(addKey)))
                {
                    continue;
                }

                // Later rename/update deltas win over the original add record.
                var effectiveName = add.Name;
                var effectiveParent = add.ParentRef;
                var effectiveTime = add.ModifiedTime;
                if (add.IsDirectory)
                {
                    if (_delta.DirOverlay.TryGetValue(addKey, out var dirMod))
                    {
                        effectiveName = dirMod.Name;
                        effectiveParent = dirMod.ParentRef;
                        effectiveTime = dirMod.ModifiedTime;
                    }
                }
                else if (_delta.FileOverlay.TryGetValue(addKey, out var fileMod))
                {
                    effectiveName = fileMod.Name.Length > 0 ? fileMod.Name : add.Name;
                    effectiveParent = fileMod.ParentRef;
                    effectiveTime = fileMod.ModifiedTime;
                }

                if (!effectiveName.Contains(needle, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                string path;
                if (add.IsDirectory)
                {
                    path = ResolvePath(addKey);
                }
                else
                {
                    var directory = ResolvePath(effectiveParent);
                    path = directory.EndsWith('\\') ? directory + effectiveName : directory + "\\" + effectiveName;
                }

                found.Add((ComputeRank(effectiveName, needle), new NtfsSearchResult(effectiveName, path, add.IsDirectory, add.Size, effectiveTime, add.Ref)));
            }
        }

        found.Sort(static (a, b) =>
        {
            var rank = a.Rank.CompareTo(b.Rank);
            if (rank != 0)
            {
                return rank;
            }

            var length = a.Item.Name.Length.CompareTo(b.Item.Name.Length);
            if (length != 0)
            {
                return length;
            }

            return string.Compare(a.Item.Name, b.Item.Name, StringComparison.OrdinalIgnoreCase);
        });

        if (found.Count <= maxResults)
        {
            return found.Select(static x => x.Item).ToList();
        }

        return found.Take(maxResults).Select(static x => x.Item).ToList();
    }

    private static int ComputeRank(string name, string needle)
    {
        if (name.Equals(needle, StringComparison.OrdinalIgnoreCase))
        {
            return 0;
        }

        if (name.StartsWith(needle, StringComparison.OrdinalIgnoreCase))
        {
            return 1;
        }

        return 2;
    }

    public void Dispose()
    {
        try
        {
            if (_base is not null)
            {
                _viewHandle.ReleasePointer();
                _base = null;
            }
        }
        catch
        {
            // ignore
        }

        _accessor.Dispose();
        _mappedFile.Dispose();
    }

    // ================= internals =================

    private bool TryReadHeader()
    {
        if (_base is null || _accessor.Capacity < IndexHeader.Size + IndexHeader.RecordSize)
        {
            return false;
        }

        for (var i = 0; i < 8; i++)
        {
            if (_base[i] != (byte)"PRSIDX01"[i])
            {
                return false;
            }
        }

        var version = *(uint*)(_base + 8);
        if (version != IndexHeader.Version)
        {
            return false;
        }

        _recordCount = *(long*)(_base + 24);
        _nameSectionOffset = *(long*)(_base + 32);
        _dirCount = *(long*)(_base + 40);
        return _recordCount >= 0
            && _nameSectionOffset == IndexHeader.Size + _recordCount * IndexHeader.RecordSize
            && _nameSectionOffset <= _accessor.Capacity;
    }

    private void BuildDirectoryMap()
    {
        byte* records = _base + IndexHeader.Size;
        for (long i = 0; i < _recordCount; i++)
        {
            byte* record = records + i * IndexHeader.RecordSize;
            if (record[38] == 0)
            {
                continue;
            }

            var refValue = *(ulong*)record & 0xFFFFFFFFFFFFUL;
            var parentRef = *(ulong*)(record + 8);
            var nameOffset = *(uint*)(record + 32);
            var nameLen = *(ushort*)(record + 36);
            _dirs[refValue] = new DirEntry(parentRef, nameOffset, nameLen, null);
        }
    }

    private string ResolvePath(ulong refValue)
    {
        return ResolvePath(refValue, 0);
    }

    private string ResolvePath(ulong refValue, int depth)
    {
        if (depth > 256)
        {
            return _driveRoot; // cycle / corrupt chain protection
        }

        var key = refValue & 0xFFFFFFFFFFFFUL;
        if (key == _rootRef)
        {
            return _driveRoot;
        }

        if (_pathMemo.TryGetValue(key, out var cached))
        {
            return cached;
        }

        if (!TryGetDir(key, out var dir))
        {
            return _driveRoot;
        }

        var parent = ResolvePath(dir.ParentRef, depth + 1);
        var name = GetDirName(dir);
        var path = parent.EndsWith('\\') ? parent + name : parent + "\\" + name;
        _pathMemo.TryAdd(key, path);
        return path;
    }

    private bool TryGetDir(ulong key, out DirEntry dir)
    {
        if (_delta is not null)
        {
            if (_delta.DeletedDirectories.Contains(key))
            {
                dir = default;
                return false;
            }

            if (_delta.DirOverlay.TryGetValue(key, out var mod))
            {
                dir = new DirEntry(mod.ParentRef, 0, 0, mod.Name);
                return true;
            }
        }

        return _dirs.TryGetValue(key, out dir);
    }

    private string GetDirName(in DirEntry dir)
    {
        if (dir.OverlayName is not null)
        {
            return dir.OverlayName;
        }

        byte* namePointer = _base + _nameSectionOffset + dir.NameOffset;
        var storedLen = *(ushort*)namePointer;
        if (storedLen != dir.NameLenBytes || (storedLen & 1) != 0)
        {
            return string.Empty;
        }

        return new string((char*)(namePointer + 2), 0, storedLen / 2);
    }

    private readonly struct DirEntry
    {
        public readonly ulong ParentRef;
        public readonly uint NameOffset;
        public readonly ushort NameLenBytes;
        public readonly string? OverlayName;

        public DirEntry(ulong parentRef, uint nameOffset, ushort nameLenBytes, string? overlayName)
        {
            ParentRef = parentRef;
            NameOffset = nameOffset;
            NameLenBytes = nameLenBytes;
            OverlayName = overlayName;
        }
    }
}
