using System.Diagnostics;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Web Search plugin — equivalent to PowerToys Run's Microsoft.Plugin.WebSearch.
/// Usage: "? something" opens the configured search engine.
/// </summary>
public sealed class WebSearchPlugin : IPlugin
{
    private readonly SettingsService _settings;

    public WebSearchPlugin(SettingsService settings)
    {
        _settings = settings;
    }

    public string Name => "Web Search";

    public string Description => "Search the web with \"?? query\" using the configured engine.";

    public string ActionKeyword => "??";

    public int Priority => 1;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        string search;
        if (query.StartsWithKeyword("??"))
        {
            search = query.Search[2..].Trim();
        }
        else if (query.StartsWithKeyword("web "))
        {
            search = query.AfterKeyword("web ");
        }
        else
        {
            return Array.Empty<ResultItem>();
        }

        if (search.Length == 0)
        {
            return new[]
            {
                new ResultItem
                {
                    Title = "Enter a search term",
                    Subtitle = "Example: ? PowerToys Run",
                    Icon = "🔍",
                    PluginName = Name,
                    CloseAfterExecute = false,
                    Execute = () => Task.CompletedTask,
                },
            };
        }

        var url = _settings.Current.GetWebSearchUrl(search);
        return new[]
        {
            new ResultItem
            {
                Title = $"Search web for \"{search}\"",
                Subtitle = url,
                Icon = "🔍",
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
