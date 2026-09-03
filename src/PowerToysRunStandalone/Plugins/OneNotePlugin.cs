using System.Diagnostics;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// OneNote plugin — equivalent to PowerToys Run's
/// Microsoft.PowerToys.Run.Plugin.OneNote.
/// Usage: "o:" opens OneNote, "o: <text>" opens OneNote search.
/// </summary>
public sealed class OneNotePlugin : IPlugin
{
    public string Name => "OneNote";

    public string Description => "Open Microsoft OneNote (o: [search text]).";

    public string ActionKeyword => "o:";

    public int Priority => 17;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        if (!query.StartsWithKeyword("o:"))
        {
            return Array.Empty<ResultItem>();
        }

        var search = query.Search[2..].Trim();
        if (search.Length == 0)
        {
            return new[]
            {
                new ResultItem
                {
                    Title = "Open OneNote",
                    Subtitle = "onenote:",
                    Icon = "📔",
                    PluginName = Name,
                    Execute = () =>
                    {
                        Process.Start(new ProcessStartInfo("onenote:") { UseShellExecute = true });
                        return Task.CompletedTask;
                    },
                },
            };
        }

        return new[]
        {
            new ResultItem
            {
                Title = $"Search OneNote for \"{search}\"",
                Subtitle = "onenote:search?q=" + Uri.EscapeDataString(search),
                Icon = "📔",
                PluginName = Name,
                Execute = () =>
                {
                    Process.Start(new ProcessStartInfo("onenote:search?q=" + Uri.EscapeDataString(search)) { UseShellExecute = true });
                    return Task.CompletedTask;
                },
            },
            new ResultItem
            {
                Title = "Open OneNote",
                Subtitle = "onenote:",
                Icon = "📔",
                PluginName = Name,
                Execute = () =>
                {
                    Process.Start(new ProcessStartInfo("onenote:") { UseShellExecute = true });
                    return Task.CompletedTask;
                },
            },
        };
    }
}
