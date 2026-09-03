using System.Diagnostics;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Windows Terminal plugin — equivalent to PowerToys Run's
/// Microsoft.PowerToys.Run.Plugin.WindowsTerminal.
/// Usage: "_" opens Windows Terminal, "_ <command>" runs it in a new tab.
/// </summary>
public sealed class WindowsTerminalPlugin : IPlugin
{
    public string Name => "Windows Terminal";

    public string Description => "Open Windows Terminal or run a command in a new tab (_ ...).";

    public string ActionKeyword => "_";

    public int Priority => 16;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        if (!query.StartsWithKeyword("_"))
        {
            return Array.Empty<ResultItem>();
        }

        var command = query.Search[1..].Trim();
        return new[]
        {
            new ResultItem
            {
                Title = command.Length == 0 ? "Open Windows Terminal" : $"Run in Windows Terminal: {command}",
                Subtitle = command.Length == 0 ? "wt.exe" : "wt.exe new-tab -- cmd /c <command>",
                Icon = "▮",
                PluginName = Name,
                Execute = () =>
                {
                    try
                    {
                        if (command.Length == 0)
                        {
                            Process.Start(new ProcessStartInfo("wt.exe") { UseShellExecute = true });
                        }
                        else
                        {
                            Process.Start(new ProcessStartInfo("wt.exe", $"new-tab -- cmd /c {command}") { UseShellExecute = true });
                        }
                    }
                    catch
                    {
                        // Windows Terminal is not installed; fall back to cmd.exe.
                        Process.Start(new ProcessStartInfo("cmd.exe", command.Length == 0 ? "/k" : $"/c {command}") { UseShellExecute = true });
                    }

                    return Task.CompletedTask;
                },
            },
        };
    }
}
