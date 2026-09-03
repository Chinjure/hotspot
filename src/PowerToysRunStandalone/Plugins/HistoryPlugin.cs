using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// History plugin — equivalent to PowerToys Run's Microsoft.Plugin.History.
/// Usage: "history" shows recently selected launcher results.
/// </summary>
public sealed class HistoryPlugin : IPlugin
{
    private readonly HistoryService _history;

    public HistoryPlugin(HistoryService history)
    {
        _history = history;
    }

    public string Name => "History";

    public string Description => "Show recently used launcher results (history).";

    public string ActionKeyword => "!!";

    public int Priority => 14;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        if (!query.Search.Equals("history", StringComparison.OrdinalIgnoreCase) &&
            !query.Search.Equals("hist", StringComparison.OrdinalIgnoreCase) &&
            !query.Search.StartsWith("!!", StringComparison.Ordinal))
        {
            return Array.Empty<ResultItem>();
        }

        return _history.Entries
            .Take(30)
            .Select(e => new ResultItem
            {
                Title = e.Title,
                Subtitle = $"{e.PluginName} · {e.UsedAt:yyyy-MM-dd HH:mm}",
                Icon = "🕘",
                PluginName = Name,
                CopyToClipboard = true,
                ClipboardText = e.Title,
                CloseAfterExecute = false,
                Execute = () => Task.CompletedTask,
            })
            .ToArray();
    }
}
