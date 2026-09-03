using System.IO;
using System.Text.Json;

namespace PowerToysRunStandalone.Infrastructure;

public sealed record HistoryEntry(string Query, string Title, string PluginName, DateTimeOffset UsedAt);

/// <summary>
/// Stores the most recently used launcher results so the History plugin
/// (like PowerToys Run's History plugin) can surface them again.
/// </summary>
public sealed class HistoryService
{
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    private readonly string _directory;
    private readonly string _path;

    public HistoryService()
    {
        _directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PowerToysRunStandalone");
        _path = Path.Combine(_directory, "history.json");
        Entries = Load();
    }

    public List<HistoryEntry> Entries { get; private set; }

    public void Add(string query, ResultItem item)
    {
        Entries.RemoveAll(e =>
            string.Equals(e.Title, item.Title, StringComparison.OrdinalIgnoreCase) &&
            string.Equals(e.PluginName, item.PluginName, StringComparison.OrdinalIgnoreCase));

        Entries.Insert(0, new HistoryEntry(query, item.Title, item.PluginName, DateTimeOffset.Now));
        if (Entries.Count > 50)
        {
            Entries.RemoveRange(50, Entries.Count - 50);
        }

        Save();
    }

    private List<HistoryEntry> Load()
    {
        try
        {
            if (File.Exists(_path))
            {
                return JsonSerializer.Deserialize<List<HistoryEntry>>(File.ReadAllText(_path), JsonOptions) ?? new();
            }
        }
        catch
        {
            // Ignore corrupt history.
        }

        return new();
    }

    private void Save()
    {
        try
        {
            Directory.CreateDirectory(_directory);
            File.WriteAllText(_path, JsonSerializer.Serialize(Entries, JsonOptions));
        }
        catch
        {
            // Ignore write failures.
        }
    }
}
