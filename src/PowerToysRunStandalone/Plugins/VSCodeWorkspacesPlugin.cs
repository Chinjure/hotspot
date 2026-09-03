using System.Diagnostics;
using System.IO;
using System.Text.Json;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// VSCode Workspaces plugin — equivalent to PowerToys Run's
/// Community.PowerToys.Run.Plugin.VSCodeWorkspaces.
/// Usage: "vsc <text>" finds recent Visual Studio Code workspaces.
/// </summary>
public sealed class VSCodeWorkspacesPlugin : IPlugin
{
    public string Name => "VSCode Workspaces";

    public string Description => "Find and open recent Visual Studio Code workspaces (vsc <text>).";

    public string ActionKeyword => "vsc";

    public int Priority => 20;

    public bool IsEnabled { get; set; } = true;

    private static readonly List<WorkspaceEntry> Cache = new();
    private static DateTime CacheAt = DateTime.MinValue;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        var search = query.Search;
        var enabled = search.StartsWith("vsc ", StringComparison.OrdinalIgnoreCase) ||
                      search.StartsWith("code ", StringComparison.OrdinalIgnoreCase) ||
                      search.StartsWith("workspaces", StringComparison.OrdinalIgnoreCase);
        if (!enabled)
        {
            return Array.Empty<ResultItem>();
        }

        if (search.StartsWith("vsc ", StringComparison.OrdinalIgnoreCase))
        {
            search = search[4..].Trim();
        }
        else if (search.StartsWith("code ", StringComparison.OrdinalIgnoreCase))
        {
            search = search[5..].Trim();
        }
        else
        {
            search = string.Empty;
        }

        return GetWorkspaces()
            .Where(w => search.Length == 0 ||
                        w.Name.Contains(search, StringComparison.OrdinalIgnoreCase) ||
                        w.Path.Contains(search, StringComparison.OrdinalIgnoreCase))
            .Take(20)
            .Select(w => new ResultItem
            {
                Title = w.Name,
                Subtitle = w.Path,
                Icon = "💻",
                PluginName = Name,
                Execute = () =>
                {
                    try
                    {
                        Process.Start(new ProcessStartInfo("code.exe", $"\"{w.Path}\"") { UseShellExecute = true });
                    }
                    catch
                    {
                        Process.Start(new ProcessStartInfo("code", $"\"{w.Path}\"") { UseShellExecute = true });
                    }

                    return Task.CompletedTask;
                },
            })
            .ToArray();
    }

    private static IReadOnlyList<WorkspaceEntry> GetWorkspaces()
    {
        lock (Cache)
        {
            if (Cache.Count > 0 && (DateTime.UtcNow - CacheAt).TotalMinutes < 5)
            {
                return Cache.ToArray();
            }

            Cache.Clear();
            foreach (var root in new[]
                     {
                         Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Code", "User", "workspaceStorage"),
                         Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Code - Insiders", "User", "workspaceStorage"),
                     })
            {
                if (!Directory.Exists(root))
                {
                    continue;
                }

                try
                {
                    foreach (var file in Directory.EnumerateFiles(root, "workspace.json", SearchOption.AllDirectories))
                    {
                        try
                        {
                            using var doc = JsonDocument.Parse(File.ReadAllText(file));
                            var path = ExtractLatestFolder(doc.RootElement);
                            if (!string.IsNullOrEmpty(path))
                            {
                                var name = Path.GetFileName(Path.GetDirectoryName(path)) ?? Path.GetFileName(path) ?? path;
                                Cache.Add(new WorkspaceEntry(name, path));
                            }
                        }
                        catch
                        {
                            // Ignore malformed workspace metadata.
                        }
                    }
                }
                catch
                {
                    // Ignore locked/removed folders.
                }
            }

            // Also pick up .code-workspace files in the user profile.
            var profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            if (Directory.Exists(profile))
            {
                try
                {
                    foreach (var file in Directory.EnumerateFiles(profile, "*.code-workspace", SearchOption.AllDirectories))
                    {
                        if (Cache.Count >= 100)
                        {
                            break;
                        }

                        Cache.Add(new WorkspaceEntry(Path.GetFileNameWithoutExtension(file), file));
                    }
                }
                catch
                {
                    // Ignore access denied.
                }
            }

            CacheAt = DateTime.UtcNow;
            return Cache.ToArray();
        }
    }

    private static string ExtractLatestFolder(JsonElement root)
    {
        if (root.TryGetProperty("folder", out var folder))
        {
            return UriToPath(folder.GetString());
        }

        if (root.TryGetProperty("path", out var path))
        {
            return path.GetString() ?? string.Empty;
        }

        return string.Empty;
    }

    private static string UriToPath(string? uriText)
    {
        if (string.IsNullOrEmpty(uriText))
        {
            return string.Empty;
        }

        if (Uri.TryCreate(uriText, UriKind.Absolute, out var uri) && uri.IsFile)
        {
            return uri.LocalPath;
        }

        return uriText;
    }

    private sealed record WorkspaceEntry(string Name, string Path);
}
