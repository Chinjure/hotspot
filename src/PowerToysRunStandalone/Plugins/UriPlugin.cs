using System.Diagnostics;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// URI plugin — equivalent to PowerToys Run's Microsoft.Plugin.Uri.
/// Usage: "uri https://github.com" or paste a URL directly.
/// </summary>
public sealed class UriPlugin : IPlugin
{
    public string Name => "URI";

    public string Description => "Open a URL or application URI (uri <address>).";

    public string ActionKeyword => "uri";

    public int Priority => 12;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        var search = query.Search.Trim();

        string? url = null;

        if (search.StartsWith("uri ", StringComparison.OrdinalIgnoreCase))
        {
            url = search[4..].Trim();
        }
        else if (search.StartsWith("//", StringComparison.Ordinal))
        {
            url = search[2..].Trim();
        }
        else if (search.StartsWith("http://", StringComparison.OrdinalIgnoreCase) ||
                 search.StartsWith("https://", StringComparison.OrdinalIgnoreCase) ||
                 search.StartsWith("file://", StringComparison.OrdinalIgnoreCase) ||
                 search.StartsWith("mailto:", StringComparison.OrdinalIgnoreCase) ||
                 search.StartsWith("ms-settings:", StringComparison.OrdinalIgnoreCase))
        {
            url = search;
        }

        if (string.IsNullOrWhiteSpace(url))
        {
            return Array.Empty<ResultItem>();
        }

        return new[]
        {
            new ResultItem
            {
                Title = $"Open: {url}",
                Subtitle = "URL / URI",
                Icon = "🌐",
                PluginName = Name,
                Execute = () =>
                {
                    Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
                    return Task.CompletedTask;
                },
            },
        };
    }
}
