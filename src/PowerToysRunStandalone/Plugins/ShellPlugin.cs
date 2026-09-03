using System.Diagnostics;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Shell plugin — equivalent to PowerToys Run's Microsoft.Plugin.Shell.
/// Usage: "> <command>" runs the command in a console window.
/// </summary>
public sealed class ShellPlugin : IPlugin
{
    public string Name => "Shell";

    public string Description => "Run a command line ( > <command> ).";

    public string ActionKeyword => ">";

    public int Priority => 8;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        if (query.ActionKeyword != ">")
        {
            return Array.Empty<ResultItem>();
        }

        var command = query.Search[1..].Trim();
        if (command.Length == 0)
        {
            return new[]
            {
                new ResultItem
                {
                    Title = "Enter a command",
                    Subtitle = "Example: > dir C:\\",
                    Icon = "⌨",
                    PluginName = Name,
                    CloseAfterExecute = false,
                    Execute = () => Task.CompletedTask,
                },
            };
        }

        return new[]
        {
            new ResultItem
            {
                Title = $"Run: {command}",
                Subtitle = "cmd.exe /c",
                Icon = "⌨",
                PluginName = Name,
                Execute = () =>
                {
                    Process.Start(new ProcessStartInfo("cmd.exe", $"/c {command}") { UseShellExecute = true });
                    return Task.CompletedTask;
                },
            },
            new ResultItem
            {
                Title = $"Run as administrator: {command}",
                Subtitle = "UAC prompt will be shown",
                Icon = "⌨",
                PluginName = Name,
                Execute = () =>
                {
                    var psi = new ProcessStartInfo("cmd.exe", $"/c {command}")
                    {
                        UseShellExecute = true,
                        Verb = "runas",
                    };
                    Process.Start(psi);
                    return Task.CompletedTask;
                },
            },
        };
    }
}
