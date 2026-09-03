using System.Diagnostics;
using Microsoft.Win32;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Registry plugin — equivalent to PowerToys Run's Microsoft.Plugin.Registry.
/// Usage: "reg <text>" searches registry key/value names and opens regedit.
/// </summary>
public sealed class RegistryPlugin : IPlugin
{
    public string Name => "Registry";

    public string Description => "Search Windows Registry keys and values (reg <text>).";

    public string ActionKeyword => ":";

    public int Priority => 6;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        string search;
        if (query.StartsWithKeyword("reg "))
        {
            search = query.AfterKeyword("reg ");
        }
        else if (query.StartsWithKeyword(":"))
        {
            search = query.Search[1..].Trim();
        }
        else
        {
            return Array.Empty<ResultItem>();
        }
        if (search.Length < 2)
        {
            return new[]
            {
                new ResultItem
                {
                    Title = "Enter a registry search term",
                    Subtitle = "Example: reg Notepad",
                    Icon = "🗄",
                    PluginName = Name,
                    CloseAfterExecute = false,
                    Execute = () => Task.CompletedTask,
                },
            };
        }

        var results = new List<ResultItem>();
        foreach (var hive in GetHives())
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!RegistryHiveKeyExists(hive))
            {
                continue;
            }

            using var baseKey = hive.OpenBaseKey();
            Walk(baseKey, hive.RootName, search, results, cancellationToken, 0);
            if (results.Count >= 100)
            {
                break;
            }
        }

        return results;
    }

    private static void Walk(RegistryKey key, string displayPath, string search, List<ResultItem> results, CancellationToken cancellationToken, int depth)
    {
        if (depth > 5 || results.Count >= 100)
        {
            return;
        }

        cancellationToken.ThrowIfCancellationRequested();

        try
        {
            foreach (var subName in key.GetSubKeyNames())
            {
                cancellationToken.ThrowIfCancellationRequested();
                var childPath = $@"{displayPath}\{subName}";
                if (subName.Contains(search, StringComparison.OrdinalIgnoreCase))
                {
                    results.Add(CreateResult(childPath, "Key"));
                }

                using var child = key.OpenSubKey(subName);
                if (child is not null)
                {
                    Walk(child, childPath, search, results, cancellationToken, depth + 1);
                }
            }

            foreach (var valueName in key.GetValueNames())
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (valueName.Contains(search, StringComparison.OrdinalIgnoreCase))
                {
                    results.Add(CreateResult($@"{displayPath}\{valueName}", "Value"));
                }
            }
        }
        catch
        {
            // Access denied or volatile keys are ignored.
        }
    }

    private static ResultItem CreateResult(string path, string kind)
    {
        return new ResultItem
        {
            Title = path,
            Subtitle = $"Registry {kind}",
            Icon = "🗄",
            PluginName = "Registry",
            CopyToClipboard = true,
            ClipboardText = path,
            Execute = () =>
            {
                Process.Start(new ProcessStartInfo("regedit.exe", $"\"{path}\"") { UseShellExecute = true });
                return Task.CompletedTask;
            },
        };
    }

    private static bool RegistryHiveKeyExists(Hive hive)
    {
        try
        {
            using var _ = hive.OpenBaseKey();
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static IEnumerable<Hive> GetHives()
    {
        yield return Hive.Hkcu;
        yield return Hive.Hklm;
        yield return Hive.Hkcr;
        yield return Hive.Hku;
    }

    private sealed class Hive
    {
        private Hive(string rootName, RegistryHive registryHive)
        {
            RootName = rootName;
            _registryHive = registryHive;
        }

        public static Hive Hkcu { get; } = new(@"HKEY_CURRENT_USER", RegistryHive.CurrentUser);
        public static Hive Hklm { get; } = new(@"HKEY_LOCAL_MACHINE", RegistryHive.LocalMachine);
        public static Hive Hkcr { get; } = new(@"HKEY_CLASSES_ROOT", RegistryHive.ClassesRoot);
        public static Hive Hku { get; } = new(@"HKEY_USERS", RegistryHive.Users);

        public string RootName { get; }
        private readonly RegistryHive _registryHive;

        public RegistryKey OpenBaseKey()
        {
            return RegistryKey.OpenBaseKey(_registryHive, RegistryView.Default);
        }
    }
}
