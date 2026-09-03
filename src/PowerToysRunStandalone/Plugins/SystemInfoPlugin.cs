using System.Diagnostics;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// System plugin — equivalent to PowerToys Run's Microsoft.Plugin.System.
/// Usage: "system" or "sys" shows system information.
/// </summary>
public sealed class SystemInfoPlugin : IPlugin
{
    public string Name => "System";

    public string Description => "Show operating system and machine information (system/sys).";

    public string ActionKeyword => "system";

    public int Priority => 13;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        var search = query.Search.Trim();
        if (!search.Equals("system", StringComparison.OrdinalIgnoreCase) &&
            !search.Equals("sys", StringComparison.OrdinalIgnoreCase) &&
            !search.Equals("uptime", StringComparison.OrdinalIgnoreCase))
        {
            return Array.Empty<ResultItem>();
        }

        var text = BuildInfo();
        return new[]
        {
            new ResultItem
            {
                Title = "System information",
                Subtitle = text,
                Icon = "🖥",
                PluginName = Name,
                CopyToClipboard = true,
                ClipboardText = text,
                Execute = () => Task.CompletedTask,
            },
        };
    }

    private static string BuildInfo()
    {
        var os = Environment.OSVersion;
        var user = Environment.UserName;
        var machine = Environment.MachineName;
        var dotnet = Environment.Version;
        var cores = Environment.ProcessorCount;
        var uptime = DateTime.Now - SystemInformationBootTime();

        return $"OS: {os}\nMachine: {machine}\nUser: {user}\nCPU cores: {cores}\n.NET: {dotnet}\nUptime: {uptime}";
    }

    private static DateTime SystemInformationBootTime()
    {
        try
        {
            var ticks = Environment.TickCount64;
            return DateTime.Now - TimeSpan.FromMilliseconds(ticks);
        }
        catch
        {
            return DateTime.Now;
        }
    }
}
