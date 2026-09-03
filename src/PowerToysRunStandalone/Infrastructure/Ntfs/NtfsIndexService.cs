using System.IO;
using System.Text.Json;

namespace PowerToysRunStandalone.Infrastructure.Ntfs;

/// <summary>Per-drive indexing state shown to the user.</summary>
public sealed class NtfsDriveState
{
    public char Drive { get; init; }

    public bool Ready { get; set; }

    public bool Building { get; set; }

    public bool Failed { get; set; }

    public long Processed { get; set; }

    public string Source { get; set; } = "-";

    public string Message { get; set; } = string.Empty;

    public long RecordCount { get; set; }

    public DateTime? LastScanTimeUtc { get; set; }
}

/// <summary>Aggregate status for the file-search plugin.</summary>
public sealed class NtfsServiceStatus
{
    public bool AnyReady { get; init; }

    public int ReadyCount { get; init; }

    public int BuildingCount { get; init; }

    public int FailedCount { get; init; }

    public long TotalRecords { get; init; }

    public string Summary { get; init; } = string.Empty;
}

/// <summary>
/// Background orchestrator for NTFS indexes: builds a compact index per
/// fixed NTFS volume (MFT-first, walk fallback), memory-maps them at startup
/// for instant full-drive search, and applies USN delta overlays when the
/// journal is readable.
/// </summary>
public sealed class NtfsIndexService : IDisposable
{
    private readonly object _gate = new();
    private readonly string _indexDirectory;
    private readonly Dictionary<char, NtfsIndex> _indexes = new();
    private readonly Dictionary<char, NtfsIndexMeta> _metas = new();
    private readonly Dictionary<char, NtfsDriveState> _states = new();
    private readonly CancellationTokenSource _lifetime = new();
    private bool _disposed;

    public NtfsIndexService()
    {
        _indexDirectory = Path.Combine(AppPaths.DataDir, "ntfs");
        Directory.CreateDirectory(_indexDirectory);
        CleanupTemporaryFiles();
    }

    private void CleanupTemporaryFiles()
    {
        try
        {
            foreach (var file in Directory.EnumerateFiles(_indexDirectory, "*.tmp"))
            {
                File.Delete(file);
            }
        }
        catch
        {
            // best effort
        }
    }

    public string IndexDirectory => _indexDirectory;

    public IReadOnlyList<NtfsDriveState> DriveStates()
    {
        lock (_gate)
        {
            return _states.Values.OrderBy(s => s.Drive).ToList();
        }
    }

    public NtfsServiceStatus Status()
    {
        lock (_gate)
        {
            var ready = _states.Values.Count(s => s.Ready);
            var building = _states.Values.Count(s => s.Building);
            var failed = _states.Values.Count(s => s.Failed);
            long records = 0;
            foreach (var index in _indexes.Values)
            {
                records += index.RecordCount;
            }

            var parts = _states.Values.OrderBy(s => s.Drive)
                .Select(s => $"{s.Drive}: {(s.Ready ? $"{s.RecordCount:N0} 项" : s.Building ? $"索引中 {s.Processed:N0}" : s.Failed ? "失败" : "等待")}")
                .ToList();
            return new NtfsServiceStatus
            {
                AnyReady = ready > 0,
                ReadyCount = ready,
                BuildingCount = building,
                FailedCount = failed,
                TotalRecords = records,
                Summary = parts.Count == 0 ? "未找到固定 NTFS 卷" : string.Join(" · ", parts),
            };
        }
    }

    /// <summary>Load existing indexes and kick off background builds for missing ones.</summary>
    public void Initialize()
    {
        foreach (var drive in NtfsScanner.GetFixedNtfsDrives())
        {
            var metaPath = MetaPath(drive);
            if (File.Exists(metaPath) && TryLoadIndex(drive, out var index, out var meta))
            {
                lock (_gate)
                {
                    _indexes[drive] = index;
                    _metas[drive] = meta;
                    _states[drive] = new NtfsDriveState
                    {
                        Drive = drive,
                        Ready = true,
                        RecordCount = index.RecordCount,
                        Source = meta.Source == 0 ? "$MFT" : "walk",
                        LastScanTimeUtc = meta.LastScanTimeUtc,
                    };
                }
            }
            else
            {
                lock (_gate)
                {
                    _states[drive] = new NtfsDriveState { Drive = drive, Message = "索引尚未建立" };
                }

                _ = BuildDriveAsync(drive);
            }
        }

        _ = Task.Run(TryUpdateAll);
    }

    /// <summary>Rebuild every fixed NTFS volume index in the background.</summary>
    public void RebuildAll()
    {
        foreach (var drive in NtfsScanner.GetFixedNtfsDrives())
        {
            _ = BuildDriveAsync(drive);
        }
    }

    public Task RebuildDriveAsync(char drive)
    {
        return BuildDriveAsync(drive);
    }

    public async Task<bool> RebuildDriveAndWait(char drive, Action<string>? log = null)
    {
        var task = BuildDriveAsync(drive);
        try
        {
            await task.ConfigureAwait(false);
            lock (_gate)
            {
                return _states.TryGetValue(drive, out var state) && state.Ready;
            }
        }
        catch (OperationCanceledException)
        {
            log?.Invoke($"\t[{drive}] 已取消");
            return false;
        }
        finally
        {
            if (log is not null)
            {
                NtfsDriveState snapshot;
                lock (_gate)
                {
                    snapshot = _states.TryGetValue(drive, out var s) ? s : new NtfsDriveState { Drive = drive };
                }

                log?.Invoke($"\t[{drive}] {snapshot.Message}");
            }
        }
    }

    public IReadOnlyList<NtfsSearchResult> Search(string needle, int maxResults, CancellationToken cancellationToken)
    {
        var results = new List<NtfsSearchResult>();
        lock (_gate)
        {
            var indexes = _indexes.Values.ToList();
            if (indexes.Count == 0)
            {
                return results;
            }

            // Fair share per volume so a large C: index can never starve D:
            // results out of the result list.
            var perDrive = Math.Max(1, (int)Math.Ceiling((double)maxResults / indexes.Count));
            foreach (var index in indexes)
            {
                cancellationToken.ThrowIfCancellationRequested();
                results.AddRange(index.Search(needle, perDrive, cancellationToken));
            }

            if (results.Count > maxResults)
            {
                return results.Take(maxResults).ToList();
            }
        }

        return results;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _lifetime.Cancel();
        List<NtfsIndex> indexes;
        lock (_gate)
        {
            indexes = _indexes.Values.ToList();
            _indexes.Clear();
        }

        foreach (var index in indexes)
        {
            try
            {
                index.Dispose();
            }
            catch
            {
                // best effort
            }
        }
    }

    // ================= internals =================

    private Task BuildDriveAsync(char drive)
    {
        lock (_gate)
        {
            if (_states.TryGetValue(drive, out var state) && state.Building)
            {
                return Task.CompletedTask;
            }

            _states[drive] = new NtfsDriveState { Drive = drive, Building = true, Message = "正在建立索引…" };
        }

        return Task.Run(() => BuildDriveCore(drive, _lifetime.Token));
    }

    private void BuildDriveCore(char drive, CancellationToken cancellationToken)
    {
        var metaPath = MetaPath(drive);
        var indexPath = IndexPath(drive);
        NtfsIndexWriter? writer = null;
        string? failure = null;
        try
        {
            NtfsScanner.TryGetVolumeInfo(drive, out var volumeInfo, out _);
            var meta = new NtfsIndexMeta
            {
                Drive = drive,
                VolumeSerial = volumeInfo.VolumeSerial,
                BytesPerRecord = (uint)volumeInfo.BytesPerRecord,
                RootRef = 5, // $MFT root directory record
                Source = 0,
                Epoch = DateTime.UtcNow.Ticks,
                LastScanTimeUtcTicks = DateTime.UtcNow.Ticks,
                JournalId = volumeInfo.JournalId,
                NextUsn = volumeInfo.NextUsn,
            };

            writer = new NtfsIndexWriter(_indexDirectory);
            NtfsScanner.ScanMft(drive, e => writer.Append(e), p =>
            {
                lock (_gate)
                {
                    if (_states.TryGetValue(drive, out var s))
                    {
                        s.Processed = p;
                    }
                }
            }, cancellationToken, out var mftError);

            if (!string.IsNullOrEmpty(mftError) || writer.RecordCount == 0)
            {
                // $MFT blocked (no admin / 24H2 protection): fall back to walk.
                writer.Dispose();
                writer = new NtfsIndexWriter(_indexDirectory);
                meta.Source = 1;
                meta.RootRef = NtfsScanner.TryGetRootRef(drive);
                NtfsScanner.WalkVolume(drive, e => writer.Append(e), p =>
                {
                    lock (_gate)
                    {
                        if (_states.TryGetValue(drive, out var s))
                        {
                            s.Processed = p;
                        }
                    }
                }, cancellationToken, out var walkError);

                if (!string.IsNullOrEmpty(walkError))
                {
                    failure = "walk 模式失败: " + walkError;
                }
            }

            meta.FileCount = writer.FileCount;
            meta.DirectoryCount = writer.DirectoryCount;
            meta.LastScanTimeUtcTicks = DateTime.UtcNow.Ticks;
            writer.Commit(meta, indexPath);
            SaveMeta(metaPath, meta);

            // Swap in the fresh index (with any existing delta overlay).
            var delta = UsnUpdater.LoadDelta(_indexDirectory, meta.Drive, meta.Epoch);
            if (NtfsIndex.TryLoad(indexPath, meta, delta) is { } loaded)
            {
                lock (_gate)
                {
                    if (_indexes.TryGetValue(drive, out var old))
                    {
                        old.Dispose();
                    }

                    _indexes[drive] = loaded;
                    _metas[drive] = meta;
                    _states[drive] = new NtfsDriveState
                    {
                        Drive = drive,
                        Ready = true,
                        RecordCount = loaded.RecordCount,
                        Source = meta.Source == 0 ? "$MFT" : "walk",
                        LastScanTimeUtc = meta.LastScanTimeUtc,
                        Message = $"索引完成: {loaded.RecordCount:N0} 项 ({(meta.Source == 0 ? "$MFT" : "walk")})",
                    };
                }

                // A fresh baseline makes the journal updatable from now on.
                _ = Task.Run(() => TryUpdateDrive(drive));
            }
            else
            {
                failure = "新索引加载失败";
            }
        }
        catch (OperationCanceledException)
        {
            failure = "已取消";
        }
        catch (Exception ex)
        {
            failure = ex.Message;
        }
        finally
        {
            writer?.Dispose();
            if (failure is not null)
            {
                lock (_gate)
                {
                    if (_states.TryGetValue(drive, out var s))
                    {
                        s.Ready = false;
                        s.Building = false;
                        s.Failed = true;
                        s.Message = failure;
                    }
                }
            }
            else
            {
                lock (_gate)
                {
                    if (_states.TryGetValue(drive, out var s))
                    {
                        s.Building = false;
                    }
                }
            }
        }
    }

    private void TryUpdateAll()
    {
        lock (_gate)
        {
            foreach (var drive in _states.Keys.Where(d => _states[d].Ready).ToList())
            {
                _ = Task.Run(() => TryUpdateDrive(drive));
            }
        }
    }

    private void TryUpdateDrive(char drive)
    {
        lock (_gate)
        {
            if (!_metas.ContainsKey(drive) || !_indexes.ContainsKey(drive))
            {
                return;
            }
        }

        NtfsIndexMeta meta;
        lock (_gate)
        {
            meta = _metas[drive];
        }

        if (!UsnUpdater.TryUpdate(_indexDirectory, drive, meta, out var newRecords, out _, out var error))
        {
            lock (_gate)
            {
                if (_states.TryGetValue(drive, out var s) && s.Ready)
                {
                    s.Message = "增量更新不可用: " + error;
                }
            }

            return;
        }

        SaveMeta(MetaPath(drive), meta);
        var delta = UsnUpdater.LoadDelta(_indexDirectory, meta.Drive, meta.Epoch);
        if (NtfsIndex.TryLoad(IndexPath(drive), meta, delta) is { } reloaded)
        {
            lock (_gate)
            {
                if (_indexes.TryGetValue(drive, out var old))
                {
                    old.Dispose();
                }

                _indexes[drive] = reloaded;
                if (_states.TryGetValue(drive, out var s))
                {
                    s.Message = newRecords.Count == 0
                        ? "已是最新"
                        : $"增量更新: {newRecords.Count:N0} 条变更";
                }
            }
        }
    }

    private bool TryLoadIndex(char drive, out NtfsIndex index, out NtfsIndexMeta meta)
    {
        index = null!;
        meta = null!;
        var metaPath = MetaPath(drive);
        try
        {
            var loadedMeta = JsonSerializer.Deserialize<NtfsIndexMeta>(File.ReadAllText(metaPath));
            if (loadedMeta is null)
            {
                return false;
            }

            var delta = UsnUpdater.LoadDelta(_indexDirectory, loadedMeta.Drive, loadedMeta.Epoch);
            var loaded = NtfsIndex.TryLoad(IndexPath(drive), loadedMeta, delta);
            if (loaded is null)
            {
                return false;
            }

            index = loaded;
            meta = loadedMeta;
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static void SaveMeta(string path, NtfsIndexMeta meta)
    {
        try
        {
            var temporary = path + ".tmp";
            File.WriteAllText(temporary, JsonSerializer.Serialize(meta, new JsonSerializerOptions { WriteIndented = true }));
            File.Move(temporary, path, overwrite: true);
        }
        catch
        {
            // metadata write failure should not crash the service
        }
    }

    private string MetaPath(char drive)
    {
        return Path.Combine(_indexDirectory, $"{drive}-meta.json");
    }

    private string IndexPath(char drive)
    {
        return Path.Combine(_indexDirectory, $"{drive}-index.dat");
    }
}
