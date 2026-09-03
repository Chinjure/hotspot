using System.Diagnostics;
using System.IO;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Folder plugin — equivalent to PowerToys Run's Microsoft.Plugin.Folder.
/// Usage: "folder" or "folder desktop" opens a common user folder.
/// </summary>
public sealed class FolderPlugin : IPlugin
{
    public string Name => "Folder";

    public string Description => "Open common user folders (folder <name>).";

    public string ActionKeyword => "folder";

    public int Priority => 5;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        var search = query.Search;
        if (search.Equals("folder", StringComparison.OrdinalIgnoreCase) ||
            search.StartsWith("folder ", StringComparison.OrdinalIgnoreCase) ||
            search.StartsWith("folders", StringComparison.OrdinalIgnoreCase))
        {
            if (search.StartsWith("folder ", StringComparison.OrdinalIgnoreCase))
            {
                search = search[7..].Trim();
            }
            else
            {
                search = string.Empty;
            }
        }
        else
        {
            return Array.Empty<ResultItem>();
        }

        return GetFolders()
            .Where(f => search.Length == 0 || f.Name.Contains(search, StringComparison.OrdinalIgnoreCase))
            .Select(f => new ResultItem
            {
                Title = f.Name,
                Subtitle = f.Path,
                Icon = "🗀",
                PluginName = Name,
                Execute = () =>
                {
                    Process.Start(new ProcessStartInfo("explorer.exe", $"\"{f.Path}\"") { UseShellExecute = true });
                    return Task.CompletedTask;
                },
            })
            .ToArray();
    }

    private static IEnumerable<(string Name, string Path)> GetFolders()
    {
        var folders = new (string Name, Environment.SpecialFolder Special)[]
        {
            ("Desktop", Environment.SpecialFolder.Desktop),
            ("Documents", Environment.SpecialFolder.MyDocuments),
            ("Pictures", Environment.SpecialFolder.MyPictures),
            ("Music", Environment.SpecialFolder.MyMusic),
            ("Videos", Environment.SpecialFolder.MyVideos),
            ("User profile", Environment.SpecialFolder.UserProfile),
        };

        foreach (var (name, special) in folders)
        {
            var path = Environment.GetFolderPath(special);
            if (!string.IsNullOrEmpty(path) && Directory.Exists(path))
            {
                yield return (name, path);
            }
        }

        var downloads = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
        if (Directory.Exists(downloads))
        {
            yield return ("Downloads", downloads);
        }
    }
}
