using System.Collections.ObjectModel;
using System.Runtime.CompilerServices;

namespace PowerToysRunStandalone.Infrastructure;

/// <summary>
/// A normalized query passed to every plugin. Mirrors the Wox.Plugin.Query
/// concept used by PowerToys Run.
/// </summary>
public sealed class Query
{
    public Query(string raw)
    {
        Raw = raw;
        Search = raw.Trim();
        ActionKeyword = string.Empty;
        Arguments = string.Empty;

        // A plugin keyword is the first token when it starts with one of the
        // typical action characters (?, >, $, ...). Plugins also do their own
        // matching, so this is only a convenient helper.
        if (Search.Length > 0 && Search[0] is '?' or '>' or '$' or '=' or '~' or '!')
        {
            ActionKeyword = Search[0].ToString();
        }
    }

    public string Raw { get; }

    public string Search { get; }

    public string ActionKeyword { get; }

    public string Arguments { get; init; } = string.Empty;

    public bool StartsWithKeyword(string keyword)
    {
        return Search.StartsWith(keyword, StringComparison.OrdinalIgnoreCase);
    }

    public string AfterKeyword(string keyword)
    {
        if (!StartsWithKeyword(keyword))
        {
            return string.Empty;
        }

        return Search[keyword.Length..].Trim();
    }

    public override string ToString() => Search;
}

/// <summary>
/// One search result shown in the launcher's list. This is the standalone
/// equivalent of PowerToys Run's Result model (Wox.Plugin.Result).
/// </summary>
public sealed class ResultItem
{
    public string Title { get; set; } = string.Empty;

    public string Subtitle { get; set; } = string.Empty;

    public string Icon { get; set; } = "▫";

    public string PluginName { get; set; } = string.Empty;

    public bool CloseAfterExecute { get; set; } = true;

    public bool CopyToClipboard { get; set; }

    public string ClipboardText { get; set; } = string.Empty;

    public Func<Task>? Execute { get; set; }

    public object? Tag { get; set; }
}

/// <summary>
/// Contract implemented by every built-in plugin. Equivalent to
/// Wox.Plugin.IPlugin in the PowerToys Run plugin infrastructure.
/// </summary>
public interface IPlugin
{
    string Name { get; }

    string Description { get; }

    string ActionKeyword { get; }

    int Priority { get; }

    bool IsEnabled { get; set; }

    IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken);
}

/// <summary>
/// A lightweight plugin descriptor used by the settings window.
/// </summary>
public sealed record PluginDescriptor(string Name, string Description, bool Enabled);

/// <summary>
/// Runs all enabled plugins and merges their results, keeping plugin priority
/// and a small relevance boost for exact title matches. The behavior mirrors
/// PowerToys Run's PluginManager + main window query pipeline.
/// </summary>
public sealed class PluginManager
{
    private readonly IReadOnlyList<IPlugin> _plugins;

    public PluginManager(IEnumerable<IPlugin> plugins)
    {
        _plugins = plugins.OrderBy(p => p.Priority).ToArray();
    }

    public IReadOnlyList<IPlugin> Plugins => _plugins;

    public IReadOnlyList<PluginDescriptor> Descriptors =>
        _plugins.Select(p => new PluginDescriptor(p.Name, p.Description, p.IsEnabled)).ToArray();

    public IEnumerable<IPlugin> EnabledPlugins => _plugins.Where(p => p.IsEnabled);

    public async Task<IReadOnlyList<ResultItem>> QueryAsync(string raw, CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(raw))
        {
            return Array.Empty<ResultItem>();
        }

        var query = new Query(raw);
        var tasks = EnabledPlugins.Select(p => Task.Run(() => SafeQuery(p, query, cancellationToken), cancellationToken)).ToArray();

        try
        {
            await Task.WhenAll(tasks).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            return Array.Empty<ResultItem>();
        }

        var results = new List<ResultItem>(256);
        foreach (var task in tasks)
        {
            if (task.Status == TaskStatus.RanToCompletion)
            {
                results.AddRange(task.Result);
            }
        }

        return results
            .OrderBy(r =>
            {
                var title = r.Title;
                var search = query.Search;
                var exact = title.Equals(search, StringComparison.OrdinalIgnoreCase) ? 0 : 1;
                var starts = title.StartsWith(search, StringComparison.OrdinalIgnoreCase) ? 2 : 3;
                var contains = title.Contains(search, StringComparison.OrdinalIgnoreCase) ? 4 : 5;
                return (exact, starts, contains);
            })
            .ToArray();
    }

    private static IReadOnlyList<ResultItem> SafeQuery(IPlugin plugin, Query query, CancellationToken cancellationToken)
    {
        try
        {
            return plugin.Query(query, cancellationToken) ?? Array.Empty<ResultItem>();
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch
        {
            return Array.Empty<ResultItem>();
        }
    }

    public void SetEnabled(string pluginName, bool enabled)
    {
        var plugin = _plugins.FirstOrDefault(p => string.Equals(p.Name, pluginName, StringComparison.OrdinalIgnoreCase));
        if (plugin is not null)
        {
            plugin.IsEnabled = enabled;
        }
    }
}
