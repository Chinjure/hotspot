using System.IO;
using System.Windows;
using Application = System.Windows.Application;
using PowerToysRunStandalone.Infrastructure;
using PowerToysRunStandalone.Infrastructure.Ntfs;
using PowerToysRunStandalone.Plugins;
using PowerToysRunStandalone.UI;

namespace PowerToysRunStandalone;

public partial class App : Application
{
    private MainWindow? _mainWindow;
    private TrayIconService? _trayIcon;

    public static SettingsService Settings { get; private set; } = null!;

    public static HistoryService History { get; private set; } = null!;

    public static PluginManager Plugins { get; private set; } = null!;

    public static NtfsIndexService Ntfs { get; private set; } = null!;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        Settings = new SettingsService();
        History = new HistoryService();
        Ntfs = new NtfsIndexService();

        if (e.Args.Any(a => a.Equals("--index-build", StringComparison.OrdinalIgnoreCase)))
        {
            RunIndexBuildCli(e.Args);
            return;
        }

        if (e.Args.Any(a => a.Equals("--live-search", StringComparison.OrdinalIgnoreCase)))
        {
            RunLiveSearchCli(e.Args);
            return;
        }

        if (e.Args.Any(a => a.Equals("--index-search", StringComparison.OrdinalIgnoreCase)))
        {
            RunIndexSearchCli(e.Args);
            return;
        }

        if (e.Args.Any(a => a.Equals("--index-status", StringComparison.OrdinalIgnoreCase)))
        {
            RunIndexStatusCli();
            return;
        }

        Plugins = new PluginManager(CreatePlugins());

        // Seed plugin defaults so a brand-new config keeps every bundled
        // plugin enabled, exactly like a fresh PowerToys install.
        foreach (var descriptor in Plugins.Descriptors)
        {
            Settings.Current.Plugins.TryAdd(descriptor.Name, true);
        }

        // Load existing NTFS indexes; missing ones are built in the background.
        Ntfs.Initialize();

        _mainWindow = new MainWindow(Settings, History, Plugins);
        _mainWindow.AutoHideOnDeactivate = !e.Args.Any(a => a.Equals("--stay", StringComparison.OrdinalIgnoreCase));
        MainWindow = _mainWindow;
        _mainWindow.Show();

        // Always keep the app reachable: taskbar while visible + tray icon
        // while hidden, so a hidden launcher can always be brought back.
        _trayIcon = new TrayIconService(
            () => Dispatcher.BeginInvoke(() => _mainWindow?.ShowLauncher()),
            () => Dispatcher.BeginInvoke(() => _mainWindow?.ShowSettingsWindow()),
            () => Dispatcher.BeginInvoke(() =>
            {
                _trayIcon?.Dispose();
                _trayIcon = null;
                Shutdown();
            }));

        if (e.Args.Any(a => a.Equals("--settings", StringComparison.OrdinalIgnoreCase)))
        {
            _mainWindow.Dispatcher.BeginInvoke(() =>
            {
                var settingsWindow = new UI.SettingsWindow(Settings, Plugins, Ntfs) { Owner = _mainWindow };
                settingsWindow.ShowDialog();
            });
        }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _trayIcon?.Dispose();
        _trayIcon = null;
        Ntfs?.Dispose();
        base.OnExit(e);
    }

    private void RunLiveSearchCli(string[] args)
    {
        var query = args.SkipWhile(a => !a.Equals("--live-search", StringComparison.OrdinalIgnoreCase))
            .Skip(1)
            .FirstOrDefault() ?? string.Empty;
        var logPath = Path.Combine(Ntfs.IndexDirectory, "live-search.log");
        var log = new List<string>();
        _ = Task.Run(async () =>
        {
            Ntfs.Initialize();
            for (var i = 0; i < 240 && !Ntfs.Status().AnyReady; i++)
            {
                await Task.Delay(500);
            }

            // Exercise the exact same code path as typing "fs <query>".
            var plugin = new FileSearchPlugin(Settings, Ntfs);
            var results = plugin.Query(new PowerToysRunStandalone.Infrastructure.Query("fs " + query), CancellationToken.None);
            log.Add($"[live] {DateTime.Now:yyyy-MM-dd HH:mm:ss} query={query} results={results.Count}");
            log.Add($"[live] status: {Ntfs.Status().Summary}");
            foreach (var hit in results.Take(50))
            {
                log.Add($"{hit.Title} | {hit.Subtitle}");
            }

            try
            {
                Directory.CreateDirectory(Ntfs.IndexDirectory);
                File.WriteAllLines(logPath, log);
            }
            catch
            {
                // ignore
            }

            Environment.Exit(0);
        });
    }

    private void RunIndexSearchCli(string[] args)
    {
        var query = args.SkipWhile(a => !a.Equals("--index-search", StringComparison.OrdinalIgnoreCase))
            .Skip(1)
            .FirstOrDefault() ?? string.Empty;
        var logPath = Path.Combine(Ntfs.IndexDirectory, "search.log");
        var log = new List<string>();
        _ = Task.Run(async () =>
        {
            Ntfs.Initialize();
            // Wait (up to ~2 min) for a ready index if one is being built.
            for (var i = 0; i < 240 && !Ntfs.Status().AnyReady; i++)
            {
                await Task.Delay(500);
            }

            var status = Ntfs.Status();
            log.Add($"[search] {DateTime.Now:yyyy-MM-dd HH:mm:ss} query={query}");
            log.Add($"[search] status: {status.Summary}");
            var results = Ntfs.Search(query, 30, CancellationToken.None);
            log.Add($"[search] results={results.Count}");
            foreach (var hit in results)
            {
                log.Add($"{hit.Name} | {hit.Path}");
            }

            try
            {
                Directory.CreateDirectory(Ntfs.IndexDirectory);
                File.WriteAllLines(logPath, log);
            }
            catch
            {
                // ignore
            }

            Environment.Exit(0);
        });
    }

    private void RunIndexStatusCli()
    {
        var logPath = Path.Combine(Ntfs.IndexDirectory, "status.log");
        var log = new List<string>();
        _ = Task.Run(async () =>
        {
            Ntfs.Initialize();
            for (var i = 0; i < 240 && !Ntfs.Status().AnyReady; i++)
            {
                await Task.Delay(500);
            }

            log.Add($"[status] {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
            log.Add(Ntfs.Status().Summary);
            foreach (var state in Ntfs.DriveStates())
            {
                log.Add($"{state.Drive}: ready={state.Ready} building={state.Building} failed={state.Failed} count={state.RecordCount} source={state.Source} message={state.Message}");
            }

            try
            {
                Directory.CreateDirectory(Ntfs.IndexDirectory);
                File.WriteAllLines(logPath, log);
            }
            catch
            {
                // ignore
            }

            Environment.Exit(0);
        });
    }

    private void RunIndexBuildCli(string[] args)
    {
        var driveArg = args.SkipWhile(a => !a.Equals("--index-build", StringComparison.OrdinalIgnoreCase)).Skip(1).FirstOrDefault();
        var drives = !string.IsNullOrEmpty(driveArg) && driveArg.Length == 1 && char.IsLetter(driveArg[0])
            ? new[] { char.ToUpperInvariant(driveArg[0]) }
            : NtfsScanner.GetFixedNtfsDrives().ToArray();

        var logPath = Path.Combine(Ntfs.IndexDirectory, "build.log");
        var log = new List<string> { $"[build] {DateTime.Now:yyyy-MM-dd HH:mm:ss} drives={string.Join(",", drives)}" };
        _ = Task.Run(async () =>
        {
            foreach (var drive in drives)
            {
                var ok = await Ntfs.RebuildDriveAndWait(drive, line => log.Add(line));
                log.Add($"[build] {drive}: {(ok ? "OK" : "FAILED")}");
                log.Add($"[build] status: {Ntfs.Status().Summary}");
            }

            try
            {
                Directory.CreateDirectory(Ntfs.IndexDirectory);
                File.WriteAllLines(logPath, log);
            }
            catch
            {
                // ignore
            }

            Environment.Exit(0);
        });
    }

    private static IEnumerable<IPlugin> CreatePlugins()
    {
        return new IPlugin[]
        {
            new ProgramPlugin(),
            new WebSearchPlugin(Settings),
            new CalculatorPlugin(),
            new UnitConverterPlugin(),
            new WindowWalkerPlugin(),
            new FileSearchPlugin(Settings, Ntfs),
            new FolderPlugin(),
            new RegistryPlugin(),
            new ProcessKillerPlugin(),
            new ShellPlugin(),
            new WindowsSettingsPlugin(),
            new TimeDatePlugin(),
            new EnvironmentVariablePlugin(),
            new UriPlugin(),
            new SystemInfoPlugin(),
            new HistoryPlugin(History),
            new ValueGeneratorPlugin(),
            new WindowsTerminalPlugin(),
            new OneNotePlugin(),
            new PowerToysPlugin(),
            new ServicePlugin(),
            new VSCodeWorkspacesPlugin(),
        };
    }
}
