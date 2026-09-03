using System.Diagnostics;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Process Killer plugin — equivalent to PowerToys Run's
/// Microsoft.Plugin.ProcessKiller. Usage: "kill notepad" terminates the process.
/// </summary>
public sealed class ProcessKillerPlugin : IPlugin
{
    public string Name => "Process Killer";

    public string Description => "Find and kill running processes (kill <name>).";

    public string ActionKeyword => "kill";

    public int Priority => 7;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        if (!query.StartsWithKeyword("kill "))
        {
            return Array.Empty<ResultItem>();
        }

        var search = query.AfterKeyword("kill ");
        if (search.Length < 2)
        {
            return new[]
            {
                new ResultItem
                {
                    Title = "Enter a process name",
                    Subtitle = "Example: kill notepad",
                    Icon = "🛑",
                    PluginName = Name,
                    CloseAfterExecute = false,
                    Execute = () => Task.CompletedTask,
                },
            };
        }

        return Process.GetProcesses()
            .Where(p =>
            {
                try
                {
                    return p.ProcessName.Contains(search, StringComparison.OrdinalIgnoreCase) ||
                           p.MainWindowTitle.Contains(search, StringComparison.OrdinalIgnoreCase);
                }
                catch
                {
                    return false;
                }
            })
            .OrderBy(p => p.ProcessName, StringComparer.OrdinalIgnoreCase)
            .Take(30)
            .Select(p => new ResultItem
            {
                Title = $"{p.ProcessName} (PID {p.Id})",
                Subtitle = p.MainWindowTitle,
                Icon = "🛑",
                PluginName = Name,
                Execute = () =>
                {
                    try
                    {
                        using var process = Process.GetProcessById(p.Id);
                        process.Kill();
                    }
                    catch
                    {
                        // Process already exited or access denied.
                    }

                    return Task.CompletedTask;
                },
            })
            .ToArray();
    }
}
