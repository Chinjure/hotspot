using System.Diagnostics;
using System.IO;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// PowerToys plugin — equivalent to PowerToys Run's
/// Microsoft.PowerToys.Run.Plugin.PowerToys. Opens the installed PowerToys
/// app when available; otherwise explains the standalone build is independent.
/// </summary>
public sealed class PowerToysPlugin : IPlugin
{
    public string Name => "PowerToys";

    public string Description => "Open PowerToys settings/utilities (@ [module]).";

    public string ActionKeyword => "@";

    public int Priority => 18;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        if (!query.StartsWithKeyword("@"))
        {
            return Array.Empty<ResultItem>();
        }

        var search = query.Search[1..].Trim();
        return new[]
        {
            new ResultItem
            {
                Title = string.IsNullOrEmpty(search) ? "Open PowerToys" : $"Open PowerToys ({search})",
                Subtitle = FindPowerToysExe() ?? "PowerToys not found — this standalone build does not require it",
                Icon = "🧰",
                PluginName = Name,
                Execute = () =>
                {
                    var exe = FindPowerToysExe();
                    if (exe is null)
                    {
                        Process.Start(new ProcessStartInfo("ms-settings:appsfeatures") { UseShellExecute = true });
                        return Task.CompletedTask;
                    }

                    Process.Start(new ProcessStartInfo(exe) { UseShellExecute = true });
                    return Task.CompletedTask;
                },
            },
        };
    }

    private static string? FindPowerToysExe()
    {
        var candidates = new[]
        {
            @"C:\Program Files\PowerToys\PowerToys.exe",
            @"C:\Program Files (x86)\PowerToys\PowerToys.exe",
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"Microsoft\PowerToys\PowerToys.exe"),
        };

        foreach (var candidate in candidates)
        {
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        return null;
    }
}
