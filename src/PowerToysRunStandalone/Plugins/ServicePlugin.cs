using System.Diagnostics;
using System.Text;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Service plugin — equivalent to PowerToys Run's
/// Microsoft.PowerToys.Run.Plugin.Service.
/// Usage: "!" lists services, "! start <name>" / "! stop <name>" controls them.
/// </summary>
public sealed class ServicePlugin : IPlugin
{
    public string Name => "Service";

    public string Description => "List, start or stop Windows services (! [start|stop <name>]).";

    public string ActionKeyword => "!";

    public int Priority => 19;

    public bool IsEnabled { get; set; } = true;

    private static List<ServiceEntry>? _cache;
    private static DateTime _cacheTime = DateTime.MinValue;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        if (!query.StartsWithKeyword("!"))
        {
            return Array.Empty<ResultItem>();
        }

        var search = query.Search[1..].Trim();

        // "!!" belongs to the History plugin, not the Service plugin.
        if (search.StartsWith("!", StringComparison.Ordinal))
        {
            return Array.Empty<ResultItem>();
        }

        var services = GetServices();
        if (search.Length == 0)
        {
            return services
                .Take(30)
                .OrderBy(s => s.Name, StringComparer.OrdinalIgnoreCase)
                .Select(CreateResult)
                .ToArray();
        }

        var lower = search.ToLowerInvariant();
        var start = lower.StartsWith("start ", StringComparison.Ordinal);
        var stop = lower.StartsWith("stop ", StringComparison.Ordinal);
        var restart = lower.StartsWith("restart ", StringComparison.Ordinal);
        var matches = services
            .Where(s => s.Name.Contains(search, StringComparison.OrdinalIgnoreCase) ||
                        s.DisplayName.Contains(search, StringComparison.OrdinalIgnoreCase))
            .Take(20)
            .ToList();

        if (start || stop || restart)
        {
            var command = start ? "start" : stop ? "stop" : "restart";
            return matches
                .Select(s => new ResultItem
                {
                    Title = $"{command} {s.Name}",
                    Subtitle = $"{s.DisplayName} · current: {s.State}",
                    Icon = "⚙",
                    PluginName = Name,
                    Execute = () =>
                    {
                        var output = RunSc($"{command} {s.Name}");
                        return Task.CompletedTask;
                    },
                })
                .ToArray();
        }

        return matches.Select(CreateResult).ToArray();
    }

    private static ResultItem CreateResult(ServiceEntry service)
    {
        return new ResultItem
        {
            Title = $"{service.Name} — {service.DisplayName}",
            Subtitle = $"State: {service.State} · ! start/stop {service.Name}",
            Icon = "⚙",
            PluginName = "Service",
            CopyToClipboard = true,
            ClipboardText = service.Name,
            CloseAfterExecute = false,
            Execute = () => Task.CompletedTask,
        };
    }

    private static List<ServiceEntry> GetServices()
    {
        lock (ServicePluginLock)
        {
            if (_cache is not null && (DateTime.UtcNow - _cacheTime).TotalMinutes < 1)
            {
                return _cache;
            }

            var output = RunSc("query state= all");
            var entries = new List<ServiceEntry>();
            string name = string.Empty;
            string display = string.Empty;
            string state = string.Empty;

            foreach (var rawLine in output.Split('\n'))
            {
                var line = rawLine.Trim();
                if (line.StartsWith("SERVICE_NAME:", StringComparison.OrdinalIgnoreCase))
                {
                    if (name.Length > 0)
                    {
                        entries.Add(new ServiceEntry(name, display, state));
                    }

                    name = line[(line.IndexOf(':') + 1)..].Trim();
                    display = string.Empty;
                    state = string.Empty;
                }
                else if (line.StartsWith("DISPLAY_NAME:", StringComparison.OrdinalIgnoreCase))
                {
                    display = line[(line.IndexOf(':') + 1)..].Trim();
                }
                else if (line.StartsWith("STATE", StringComparison.OrdinalIgnoreCase))
                {
                    var statePart = line[(line.IndexOf(':') + 1)..].Trim();
                    if (statePart.Contains("RUNNING", StringComparison.OrdinalIgnoreCase))
                    {
                        state = "RUNNING";
                    }
                    else if (statePart.Contains("STOPPED", StringComparison.OrdinalIgnoreCase))
                    {
                        state = "STOPPED";
                    }
                    else if (statePart.Contains("START_PENDING", StringComparison.OrdinalIgnoreCase))
                    {
                        state = "START_PENDING";
                    }
                    else if (statePart.Contains("STOP_PENDING", StringComparison.OrdinalIgnoreCase))
                    {
                        state = "STOP_PENDING";
                    }
                    else
                    {
                        state = statePart;
                    }
                }
            }

            if (name.Length > 0)
            {
                entries.Add(new ServiceEntry(name, display, state));
            }

            _cache = entries;
            _cacheTime = DateTime.UtcNow;
            return entries;
        }
    }

    private static readonly object ServicePluginLock = new();

    private static string RunSc(string arguments)
    {
        try
        {
            var psi = new ProcessStartInfo("sc.exe", arguments)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            using var process = Process.Start(psi);
            if (process is null)
            {
                return string.Empty;
            }

            var output = process.StandardOutput.ReadToEnd();
            var error = process.StandardError.ReadToEnd();
            process.WaitForExit(15000);
            return output + error;
        }
        catch
        {
            return string.Empty;
        }
    }

    private sealed record ServiceEntry(string Name, string DisplayName, string State);
}
