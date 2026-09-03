using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Environment Variable plugin — equivalent to PowerToys Run's
/// Microsoft.Plugin.EnvironmentVariable. Usage: "env PATH" or "$ PATH".
/// </summary>
public sealed class EnvironmentVariablePlugin : IPlugin
{
    public string Name => "Environment Variable";

    public string Description => "Show environment variables (env <NAME> or $ <NAME>).";

    public string ActionKeyword => "$";

    public int Priority => 11;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        var search = query.Search;
        if (search.StartsWith("env ", StringComparison.OrdinalIgnoreCase))
        {
            search = search[4..].Trim();
        }
        else
        {
            return Array.Empty<ResultItem>();
        }

        if (search.Length == 0)
        {
            return Environment.GetEnvironmentVariables()
                .Cast<System.Collections.DictionaryEntry>()
                .OrderBy(e => e.Key?.ToString(), StringComparer.OrdinalIgnoreCase)
                .Take(30)
                .Select(CreateEntry)
                .ToArray();
        }

        return Environment.GetEnvironmentVariables()
            .Cast<System.Collections.DictionaryEntry>()
            .Where(e => e.Key?.ToString()?.Contains(search, StringComparison.OrdinalIgnoreCase) == true)
            .Take(30)
            .Select(CreateEntry)
            .ToArray();
    }

    private static ResultItem CreateEntry(System.Collections.DictionaryEntry entry)
    {
        var name = entry.Key?.ToString() ?? string.Empty;
        var value = entry.Value?.ToString() ?? string.Empty;
        return new ResultItem
        {
            Title = $"{name} = {value}",
            Subtitle = "Environment variable",
            Icon = "λ",
            PluginName = "Environment Variable",
            CopyToClipboard = true,
            ClipboardText = value,
            Execute = () => Task.CompletedTask,
        };
    }
}
