using System.Globalization;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Time and Date plugin — equivalent to PowerToys Run's
/// Microsoft.Plugin.TimeDate. Shows the current time, date, UTC and Unix time.
/// </summary>
public sealed class TimeDatePlugin : IPlugin
{
    public string Name => "Time & Date";

    public string Description => "Show current time, date and Unix timestamp (time/date/now).";

    public string ActionKeyword => ")";

    public int Priority => 10;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        var search = query.Search.Trim();
        if (search.StartsWith(')'))
        {
            search = search[1..].Trim();
        }

        if (!search.Equals("time", StringComparison.OrdinalIgnoreCase) &&
            !search.Equals("date", StringComparison.OrdinalIgnoreCase) &&
            !search.Equals("now", StringComparison.OrdinalIgnoreCase) &&
            !search.StartsWith("today", StringComparison.OrdinalIgnoreCase) &&
            search.Length != 0)
        {
            return Array.Empty<ResultItem>();
        }

        if (query.Search.Trim().Length == 0)
        {
            return Array.Empty<ResultItem>();
        }

        var now = DateTimeOffset.Now;
        var local = now.ToString("yyyy-MM-dd HH:mm:ss zzz", CultureInfo.InvariantCulture);
        var utc = now.UtcDateTime.ToString("yyyy-MM-dd HH:mm:ss 'UTC'", CultureInfo.InvariantCulture);
        var unix = now.ToUnixTimeSeconds().ToString(CultureInfo.InvariantCulture);

        return new[]
        {
            new ResultItem
            {
                Title = "Local time: " + local,
                Subtitle = now.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture),
                Icon = "🕐",
                PluginName = Name,
                CopyToClipboard = true,
                ClipboardText = local,
                Execute = () => Task.CompletedTask,
            },
            new ResultItem
            {
                Title = "UTC time: " + utc,
                Subtitle = "Copy to clipboard",
                Icon = "🕐",
                PluginName = Name,
                CopyToClipboard = true,
                ClipboardText = utc,
                Execute = () => Task.CompletedTask,
            },
            new ResultItem
            {
                Title = "Unix timestamp: " + unix,
                Subtitle = "Copy to clipboard",
                Icon = "🕐",
                PluginName = Name,
                CopyToClipboard = true,
                ClipboardText = unix,
                Execute = () => Task.CompletedTask,
            },
        };
    }
}
