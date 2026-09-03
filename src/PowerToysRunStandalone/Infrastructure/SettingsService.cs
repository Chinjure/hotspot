using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace PowerToysRunStandalone.Infrastructure;

/// <summary>
/// Persistent settings for the standalone launcher. Stored as JSON under
/// %LocalAppData%\PowerToysRunStandalone\settings.json, replacing the
/// PowerToys Settings app that normally hosts PowerToys Run options.
/// </summary>
public sealed class AppSettings
{
    public string Hotkey { get; set; } = "Alt+Space";

    public bool AutoStart { get; set; } = true;

    public string WebSearchUrl { get; set; } = "https://www.bing.com/search?q={0}";

    public bool ShowOnStartup { get; set; } = true;

    public int FileSearchMaxResults { get; set; } = 200;

    public bool NtfsIndexEnabled { get; set; } = true;

    /// <summary>Time-boxed live directory search for files outside the index.</summary>
    public bool NtfsLiveSearchEnabled { get; set; } = true;

    /// <summary>Everything-style: plain text also searches files (no fs/? prefix needed).</summary>
    public bool PlainTextFileSearch { get; set; } = true;

    /// <summary>Budget in milliseconds for the live fallback scan (300-5000).</summary>
    public int NtfsLiveSearchTimeoutMs { get; set; } = 800;

    public Dictionary<string, bool> Plugins { get; set; } = new(StringComparer.OrdinalIgnoreCase);

    public string GetWebSearchUrl(string query)
    {
        if (string.IsNullOrEmpty(WebSearchUrl))
        {
            return "https://www.bing.com/search?q=" + Uri.EscapeDataString(query);
        }

        return string.Format(WebSearchUrl, Uri.EscapeDataString(query));
    }
}

/// <summary>
/// Loads and saves <see cref="AppSettings"/>. Plugin defaults are seeded with
/// all bundled plugins enabled, matching PowerToys Run's out-of-box behavior.
/// </summary>
public sealed class SettingsService
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
    };

    private readonly string _directory;
    private readonly string _path;

    public SettingsService()
    {
        _directory = AppPaths.DataDir;
        _path = Path.Combine(_directory, "settings.json");
        Current = Load();
    }

    public AppSettings Current { get; private set; }

    public string ConfigPath => _path;

    public bool IsPluginEnabled(string pluginName, bool defaultEnabled = true)
    {
        return Current.Plugins.TryGetValue(pluginName, out var value) ? value : defaultEnabled;
    }

    public void SetPluginEnabled(string pluginName, bool enabled)
    {
        Current.Plugins[pluginName] = enabled;
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(_directory);
            File.WriteAllText(_path, JsonSerializer.Serialize(Current, JsonOptions));
        }
        catch
        {
            // A read-only profile or disk issue must not crash the launcher.
        }
    }

    private AppSettings Load()
    {
        try
        {
            if (File.Exists(_path))
            {
                var json = File.ReadAllText(_path);
                var loaded = JsonSerializer.Deserialize<AppSettings>(json, JsonOptions);
                if (loaded is not null)
                {
                    loaded.Plugins ??= new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
                    return loaded;
                }
            }
        }
        catch
        {
            // Fall through to defaults if the file is corrupt.
        }

        return new AppSettings();
    }
}
