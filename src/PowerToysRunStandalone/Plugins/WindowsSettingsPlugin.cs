using System.Diagnostics;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Windows Settings plugin — equivalent to PowerToys Run's
/// Microsoft.Plugin.WindowsSettings. Usage: "win" or "settings" plus a keyword.
/// </summary>
public sealed class WindowsSettingsPlugin : IPlugin
{
    public string Name => "Windows Settings";

    public string Description => "Open Windows settings pages (win <page>).";

    public string ActionKeyword => "$";

    public int Priority => 9;

    public bool IsEnabled { get; set; } = true;

    private static readonly SettingEntry[] Entries =
    {
        new("Display", "ms-settings:display"),
        new("Network", "ms-settings:network"),
        new("Notifications", "ms-settings:notifications"),
        new("Power & sleep", "ms-settings:powersleep"),
        new("Personalization", "ms-settings:personalization"),
        new("Apps", "ms-settings:appsfeatures"),
        new("Accounts", "ms-settings:accounts"),
        new("Windows Update", "ms-settings:windowsupdate"),
        new("Bluetooth", "ms-settings:bluetooth"),
        new("Sound", "ms-settings:sound"),
        new("Storage", "ms-settings:storagesense"),
        new("About", "ms-settings:about"),
    };

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        var search = query.Search;
        var useSettings = false;

        if (search.StartsWith('$'))
        {
            search = search[1..].Trim();
            useSettings = true;
        }
        else if (search.StartsWith("win ", StringComparison.OrdinalIgnoreCase) ||
                 search.StartsWith("settings ", StringComparison.OrdinalIgnoreCase))
        {
            var index = search.IndexOf(' ');
            search = index >= 0 ? search[(index + 1)..].Trim() : string.Empty;
            useSettings = true;
        }
        else if (search.Equals("win", StringComparison.OrdinalIgnoreCase) ||
                 search.Equals("settings", StringComparison.OrdinalIgnoreCase))
        {
            useSettings = true;
        }

        if (!useSettings)
        {
            return Array.Empty<ResultItem>();
        }

        return Entries
            .Where(e => search.Length == 0 || e.Name.Contains(search, StringComparison.OrdinalIgnoreCase))
            .Select(e => new ResultItem
            {
                Title = e.Name,
                Subtitle = e.Uri,
                Icon = "⚙",
                PluginName = Name,
                Execute = () =>
                {
                    Process.Start(new ProcessStartInfo(e.Uri) { UseShellExecute = true });
                    return Task.CompletedTask;
                },
            })
            .ToArray();
    }

    private sealed record SettingEntry(string Name, string Uri);
}
