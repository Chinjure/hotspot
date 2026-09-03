using System.Diagnostics;
using System.IO;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Programs plugin — equivalent to PowerToys Run's Microsoft.Plugin.Program.
/// Indexes Start Menu shortcuts and launches them with the shell.
/// </summary>
public sealed class ProgramPlugin : IPlugin
{
    public string Name => "Program";

    public string Description => "Search and launch installed applications from the Start Menu.";

    public string ActionKeyword => string.Empty;

    public int Priority => 0;

    public bool IsEnabled { get; set; } = true;

    private static readonly List<ProgramEntry> Cache = new();
    private static readonly object CacheLock = new();
    private static DateTime CacheBuiltAt = DateTime.MinValue;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        var search = query.Search;

        // Avoid cluttering the shell/web/action results with application names
        // when the user is explicitly using another plugin prefix.
        if (search.Length == 0 || search[0] is '?' or '>' or '$' or '=' or '!' or '#' or '@' or '_' or ':' or ')' or '<' or '/')
        {
            return Array.Empty<ResultItem>();
        }

        var programs = GetPrograms();
        return programs
            .Where(p => p.Name.Contains(search, StringComparison.OrdinalIgnoreCase)
                        || p.Path.Contains(search, StringComparison.OrdinalIgnoreCase))
            .Take(30)
            .Select(p => new ResultItem
            {
                Title = p.Name,
                Subtitle = p.Path,
                Icon = "🗔",
                PluginName = Name,
                Execute = () =>
                {
                    try
                    {
                        Process.Start(new ProcessStartInfo(p.Path) { UseShellExecute = true });
                    }
                    catch (Exception ex)
                    {
                        Debug.WriteLine($"Failed to launch {p.Path}: {ex.Message}");
                        throw;
                    }

                    return Task.CompletedTask;
                },
            })
            .ToArray();
    }

    private static IReadOnlyList<ProgramEntry> GetPrograms()
    {
        lock (CacheLock)
        {
            if (Cache.Count > 0 && (DateTime.UtcNow - CacheBuiltAt).TotalMinutes < 10)
            {
                return Cache.ToArray();
            }

            Cache.Clear();
            foreach (var root in GetStartMenuRoots().Distinct(StringComparer.OrdinalIgnoreCase))
            {
                if (!Directory.Exists(root))
                {
                    continue;
                }

                try
                {
                    foreach (var file in Directory.EnumerateFiles(root, "*.lnk", SearchOption.AllDirectories))
                    {
                        if (Cache.Count >= 2000)
                        {
                            break;
                        }

                        var name = Path.GetFileNameWithoutExtension(file);
                        if (!string.IsNullOrWhiteSpace(name))
                        {
                            Cache.Add(new ProgramEntry(name, file));
                        }
                    }
                }
                catch
                {
                    // Ignore protected or missing directories.
                }
            }

            CacheBuiltAt = DateTime.UtcNow;
            return Cache.ToArray();
        }
    }

    private static IEnumerable<string> GetStartMenuRoots()
    {
        yield return Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms);
        yield return Environment.GetFolderPath(Environment.SpecialFolder.Programs);
        yield return Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData) + @"\Microsoft\Windows\Start Menu\Programs";
    }

    private sealed record ProgramEntry(string Name, string Path);
}
