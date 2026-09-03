using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using PowerToysRunStandalone.Infrastructure;
using PowerToysRunStandalone.Infrastructure.Ntfs;

namespace PowerToysRunStandalone.UI;

/// <summary>
/// Simple settings window for the standalone launcher. It replaces the
/// PowerToys Settings app's launcher page for this extracted build.
/// </summary>
public partial class SettingsWindow : Window
{
    private readonly SettingsService _settings;
    private readonly PluginManager _plugins;
    private readonly NtfsIndexService _ntfs;

    public SettingsWindow(SettingsService settings, PluginManager plugins, NtfsIndexService ntfs)
    {
        InitializeComponent();

        _settings = settings;
        _plugins = plugins;
        _ntfs = ntfs;

        LoadSettings();
        PopulatePluginPanel();
        RefreshIndexStatus();
    }

    private void LoadSettings()
    {
        HotkeyBox.Text = _settings.Current.Hotkey;
        AutoStartCheck.IsChecked = _settings.Current.AutoStart;
        ShowOnStartupCheck.IsChecked = _settings.Current.ShowOnStartup;
        NtfsIndexCheck.IsChecked = _settings.Current.NtfsIndexEnabled;
        NtfsLiveSearchCheck.IsChecked = _settings.Current.NtfsLiveSearchEnabled;
        PlainTextSearchCheck.IsChecked = _settings.Current.PlainTextFileSearch;
        LiveTimeoutBox.Text = _settings.Current.NtfsLiveSearchTimeoutMs.ToString();

        SearchEngineCombo.Items.Clear();
        SearchEngineCombo.Items.Add(new ComboBoxItem { Content = "Bing", Tag = "https://www.bing.com/search?q={0}" });
        SearchEngineCombo.Items.Add(new ComboBoxItem { Content = "Google", Tag = "https://www.google.com/search?q={0}" });
        SearchEngineCombo.Items.Add(new ComboBoxItem { Content = "DuckDuckGo", Tag = "https://duckduckgo.com/?q={0}" });

        foreach (ComboBoxItem item in SearchEngineCombo.Items)
        {
            if (string.Equals(item.Tag as string, _settings.Current.WebSearchUrl, StringComparison.OrdinalIgnoreCase))
            {
                SearchEngineCombo.SelectedItem = item;
                break;
            }
        }

        SearchEngineCombo.SelectedItem ??= SearchEngineCombo.Items[0];
    }

    private void PopulatePluginPanel()
    {
        PluginPanel.Children.Clear();
        foreach (var descriptor in _plugins.Descriptors)
        {
            var check = new CheckBox
            {
                Content = $"{descriptor.Name} — {descriptor.Description}",
                IsChecked = descriptor.Enabled,
                Tag = descriptor.Name,
                Margin = new Thickness(0, 0, 0, 8),
            };
            PluginPanel.Children.Add(check);
        }
    }

    private void RefreshIndexStatus()
    {
        var states = _ntfs.DriveStates();
        if (states.Count == 0)
        {
            NtfsStatusText.Text = "未检测到固定 NTFS 卷。";
            return;
        }

        var lines = states.Select(s =>
        {
            var status = s.Ready
                ? $"{s.RecordCount:N0} 项 ({s.Source}) · 上次 {s.LastScanTimeUtc?.ToLocalTime():yyyy-MM-dd HH:mm}"
                : s.Building
                    ? $"构建中 {s.Processed:N0} 条记录"
                    : s.Failed
                        ? "失败"
                        : "等待";
            var message = string.IsNullOrEmpty(s.Message) ? string.Empty : $" — {s.Message}";
            return $"{s.Drive}: {status}{message}";
        });
        NtfsStatusText.Text = string.Join(Environment.NewLine, lines);
    }

    private void RebuildIndex_Click(object sender, RoutedEventArgs e)
    {
        _ntfs.RebuildAll();
        RebuildIndexButton.IsEnabled = false;
        RefreshIndexStatus();

        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1500) };
        timer.Tick += (_, _) =>
        {
            RefreshIndexStatus();
            var states = _ntfs.DriveStates();
            if (states.All(s => !s.Building))
            {
                timer.Stop();
                RebuildIndexButton.IsEnabled = true;
            }
        };
        timer.Start();
    }

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        var hotkey = HotkeyBox.Text.Trim();
        if (!HotkeyService.TryParseShortcut(hotkey, out _, out _))
        {
            MessageBox.Show(this, "Invalid hotkey. Use a format like Alt+Space or Ctrl+Shift+Space.", "PowerToys Run Standalone");
            return;
        }

        _settings.Current.Hotkey = hotkey;
        _settings.Current.AutoStart = AutoStartCheck.IsChecked == true;
        _settings.Current.ShowOnStartup = ShowOnStartupCheck.IsChecked == true;
        _settings.Current.NtfsIndexEnabled = NtfsIndexCheck.IsChecked == true;
        _settings.Current.NtfsLiveSearchEnabled = NtfsLiveSearchCheck.IsChecked == true;
        _settings.Current.PlainTextFileSearch = PlainTextSearchCheck.IsChecked == true;
        if (int.TryParse(LiveTimeoutBox.Text.Trim(), out var timeoutMs))
        {
            _settings.Current.NtfsLiveSearchTimeoutMs = Math.Clamp(timeoutMs, 300, 5000);
        }

        var search = SearchEngineCombo.SelectedItem as ComboBoxItem;
        if (search?.Tag is string url)
        {
            _settings.Current.WebSearchUrl = url;
        }

        foreach (var check in PluginPanel.Children.OfType<CheckBox>())
        {
            if (check.Tag is string name)
            {
                _settings.SetPluginEnabled(name, check.IsChecked == true);
            }
        }

        _settings.Save();
        DialogResult = true;
    }

    private void Cancel_Click(object sender, RoutedEventArgs e)
    {
        DialogResult = false;
    }
}
