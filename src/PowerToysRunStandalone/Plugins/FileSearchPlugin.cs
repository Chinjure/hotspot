using System.Diagnostics;
using System.IO;
using PowerToysRunStandalone.Infrastructure;
using PowerToysRunStandalone.Infrastructure.Ntfs;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// File Search plugin — PowerToys Run's "? / fs" file search, reworked on top
/// of the built-in NTFS record index (undex-style $MFT reconstruction).
/// Covers every file on every fixed NTFS volume, not just a handful of common
/// user folders.
///
/// To also catch files that are not (yet) in the index — new files after the
/// last scan, files on volumes without an index, access-denied skips — the
/// plugin merges a time-boxed live directory scan into the results. The live
/// scan can be disabled in settings.
/// </summary>
public sealed class FileSearchPlugin : IPlugin
{
    private readonly SettingsService _settings;
    private readonly NtfsIndexService _ntfs;

    public FileSearchPlugin(SettingsService settings, NtfsIndexService ntfs)
    {
        _settings = settings;
        _ntfs = ntfs;
    }

    public string Name => "File Search";

    public string Description => "NTFS $MFT 全盘索引 + 索引外实时搜索 (fs <name> / ? <name>).";

    public string ActionKeyword => "fs";

    public int Priority => 5;

    public bool IsEnabled { get; set; } = true;

    private static readonly string[] CommandPrefixes =
    {
        "fs ", "ww ", "kill ", "env ", "reg ", "folder ", "uri ", "win ",
        "vsc ", "u ", "o:", "??", "!!", "//", "mailto:", "http://", "https://",
    };

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        var raw = query.Search;
        var useIndexer = raw.StartsWith("?", StringComparison.OrdinalIgnoreCase) &&
                         !raw.StartsWith("??", StringComparison.OrdinalIgnoreCase);
        var useFileSearch = raw.StartsWith("fs ", StringComparison.OrdinalIgnoreCase);
        var plainSearch = !useIndexer && !useFileSearch &&
                          _settings.Current.PlainTextFileSearch &&
                          !StartsWithCommandPrefix(raw);
        if (!useIndexer && !useFileSearch && !plainSearch)
        {
            return Array.Empty<ResultItem>();
        }

        var search = useIndexer
            ? raw[1..].Trim()
            : useFileSearch
                ? query.AfterKeyword("fs ")
                : raw.Trim();
        if (search.Length < 2)
        {
            if (plainSearch)
            {
                return Array.Empty<ResultItem>();
            }

            return new[]
            {
                new ResultItem
                {
                    Title = "Enter a file name",
                    Subtitle = "Example: fs report.docx  ·  ? 2024 会议纪要  ·  直接输入文件名",
                    Icon = "📁",
                    PluginName = Name,
                    CloseAfterExecute = false,
                    Execute = () => Task.CompletedTask,
                },
            };
        }

        var max = Math.Max(1, Math.Min(_settings.Current.FileSearchMaxResults, 2000));
        if (!_settings.Current.NtfsIndexEnabled)
        {
            return LegacyCommonFolderSearch(search, max, cancellationToken);
        }

        var status = _ntfs.Status();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var results = new List<ResultItem>(Math.Min(max, 2048));

        // 1) Indexed hits (whole NTFS records, all indexed volumes).
        foreach (var hit in _ntfs.Search(search, max, cancellationToken))
        {
            seen.Add(hit.Path);
            results.Add(ToResultItem(hit));
        }

        // 2) Time-boxed live scan for everything beyond the index:
        //    newly created files, unindexed volumes, skipped paths.
        //    For plain (non-prefixed) queries only run it when the index found
        //    nothing, so app/calculator search stays snappy.
        var runLive = _settings.Current.NtfsLiveSearchEnabled &&
                      (!plainSearch || results.Count == 0) &&
                      results.Count < max;
        if (runLive)
        {
            foreach (var item in LiveFileSearch(search, max - results.Count, seen, cancellationToken))
            {
                results.Add(item);
                if (results.Count >= max)
                {
                    break;
                }
            }
        }

        if (results.Count > 0)
        {
            return results;
        }

        if (status.BuildingCount > 0)
        {
            return new[]
            {
                new ResultItem
                {
                    Title = "NTFS 索引构建中，实时搜索未命中…",
                    Subtitle = status.Summary + " · 索引完成后将全盘秒搜",
                    Icon = "⏳",
                    PluginName = Name,
                    CloseAfterExecute = false,
                    Execute = () => Task.CompletedTask,
                },
            };
        }

        if (status.FailedCount > 0)
        {
            return new[]
            {
                new ResultItem
                {
                    Title = "NTFS 索引不可用",
                    Subtitle = status.Summary,
                    Icon = "⚠",
                    PluginName = Name,
                    CloseAfterExecute = false,
                    Execute = () => Task.CompletedTask,
                },
            };
        }

        return LegacyCommonFolderSearch(search, max, cancellationToken);
    }

    // ================= live fallback (beyond the index) =================

    private static bool StartsWithCommandPrefix(string raw)
    {
        if (raw.Length == 0)
        {
            return false;
        }

        // Symbol-led commands belong to other plugins (shell, registry, URI...).
        if ("? > $ = ~ ! : # / _ @ < )".IndexOf(raw[0]) >= 0)
        {
            return true;
        }

        foreach (var prefix in CommandPrefixes)
        {
            if (raw.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }

        return false;
    }

    private IReadOnlyList<ResultItem> LiveFileSearch(
        string search,
        int max,
        HashSet<string> seen,
        CancellationToken cancellationToken)
    {
        var results = new List<ResultItem>();
        var timeout = Math.Clamp(_settings.Current.NtfsLiveSearchTimeoutMs, 300, 5000);
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        cts.CancelAfter(timeout);

        try
        {
            foreach (var root in LiveSearchRoots())
            {
                foreach (var path in EnumerateLiveEntries(root, cts.Token))
                {
                    if (results.Count >= max)
                    {
                        return results;
                    }

                    cts.Token.ThrowIfCancellationRequested();
                    var name = Path.GetFileName(path.TrimEnd('\\'));
                    if (string.IsNullOrEmpty(name) || !name.Contains(search, StringComparison.OrdinalIgnoreCase))
                    {
                        continue;
                    }

                    if (!seen.Add(path))
                    {
                        continue;
                    }

                    var isDirectory = Directory.Exists(path);
                    results.Add(new ResultItem
                    {
                        Title = name,
                        Subtitle = path + "　· 实时命中（索引外）",
                        Icon = isDirectory ? "📂" : "📄",
                        PluginName = Name,
                        CopyToClipboard = true,
                        ClipboardText = path,
                        Execute = () =>
                        {
                            // 左键 = 打开文件（默认程序）；文件夹直接打开。
                            if (isDirectory)
                            {
                                Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true });
                            }
                            else
                            {
                                Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
                            }

                            return Task.CompletedTask;
                        },
                    });
                }
            }
        }
        catch (OperationCanceledException)
        {
            // Time budget reached (or the user typed again): return what we have.
        }

        return results;
    }

    private static IEnumerable<string> LiveSearchRoots()
    {
        var roots = new List<string>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        void Add(string? path)
        {
            if (string.IsNullOrEmpty(path) || !Directory.Exists(path))
            {
                return;
            }

            if (seen.Add(path))
            {
                roots.Add(path);
            }
        }

        // Fast, likely locations first.
        Add(Environment.GetFolderPath(Environment.SpecialFolder.Desktop));
        Add(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments));
        Add(Environment.GetFolderPath(Environment.SpecialFolder.MyPictures));
        Add(Environment.GetFolderPath(Environment.SpecialFolder.MyMusic));
        Add(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos));
        Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads"));

        // Then every ready volume (fixed, USB/removable, mapped network) so
        // unindexed drives also work within the time budget.
        try
        {
            foreach (var drive in DriveInfo.GetDrives())
            {
                if (!drive.IsReady)
                {
                    continue;
                }

                if (drive.DriveType is not (DriveType.Fixed or DriveType.Removable or DriveType.Network))
                {
                    continue;
                }

                Add(drive.Name);
            }
        }
        catch
        {
            // best effort
        }

        return roots;
    }

    private static IEnumerable<string> EnumerateLiveEntries(string root, CancellationToken cancellationToken)
    {
        var options = new EnumerationOptions
        {
            RecurseSubdirectories = true,
            IgnoreInaccessible = true,
            AttributesToSkip = FileAttributes.ReparsePoint,
            ReturnSpecialDirectories = false,
        };

        foreach (var entry in Directory.EnumerateFileSystemEntries(root, "*", options))
        {
            cancellationToken.ThrowIfCancellationRequested();
            yield return entry;
        }
    }

    // ================= index result conversion =================

    private static ResultItem ToResultItem(NtfsSearchResult hit)
    {
        var path = hit.Path;
        return new ResultItem
        {
            Title = hit.Name,
            Subtitle = path,
            Icon = hit.IsDirectory ? "📂" : "📄",
            PluginName = "File Search",
            CopyToClipboard = true,
            ClipboardText = path,
            Execute = () =>
            {
                // 左键 = 打开文件（默认程序）；文件夹直接打开。
                if (hit.IsDirectory)
                {
                    Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true });
                }
                else
                {
                    Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
                }

                return Task.CompletedTask;
            },
        };
    }

    // ================= legacy fallback =================

    private static IReadOnlyList<ResultItem> LegacyCommonFolderSearch(string search, int max, CancellationToken cancellationToken)
    {
        var results = new List<ResultItem>();
        foreach (var root in GetSearchRoots())
        {
            if (results.Count >= max)
            {
                break;
            }

            foreach (var file in EnumerateFilesSafe(root))
            {
                cancellationToken.ThrowIfCancellationRequested();
                var name = Path.GetFileName(file);
                if (!name.Contains(search, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                results.Add(new ResultItem
                {
                    Title = name,
                    Subtitle = file,
                    Icon = "📄",
                    PluginName = "File Search",
                    CopyToClipboard = true,
                    ClipboardText = file,
                    Execute = () =>
                    {
                        // 左键 = 打开文件（默认程序）。
                        Process.Start(new ProcessStartInfo(file) { UseShellExecute = true });
                        return Task.CompletedTask;
                    },
                });

                if (results.Count >= max)
                {
                    break;
                }
            }
        }

        return results;
    }

    private static IEnumerable<string> GetSearchRoots()
    {
        var folders = new[]
        {
            Environment.SpecialFolder.Desktop,
            Environment.SpecialFolder.MyDocuments,
            Environment.SpecialFolder.MyPictures,
            Environment.SpecialFolder.MyMusic,
            Environment.SpecialFolder.MyVideos,
        };

        foreach (var special in folders)
        {
            var path = Environment.GetFolderPath(special);
            if (!string.IsNullOrEmpty(path) && Directory.Exists(path))
            {
                yield return path;
            }
        }

        var downloads = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
        if (Directory.Exists(downloads))
        {
            yield return downloads;
        }
    }

    private static IEnumerable<string> EnumerateFilesSafe(string root)
    {
        var pending = new Stack<string>();
        pending.Push(root);

        while (pending.Count > 0)
        {
            var current = pending.Pop();
            if (current.Length > 300)
            {
                continue;
            }

            string[] directories;
            string[] files;
            try
            {
                directories = Directory.EnumerateDirectories(current).ToArray();
                files = Directory.EnumerateFiles(current).ToArray();
            }
            catch
            {
                // Ignore access denied / broken links.
                continue;
            }

            foreach (var directory in directories)
            {
                pending.Push(directory);
            }

            foreach (var file in files)
            {
                yield return file;
            }
        }
    }
}
