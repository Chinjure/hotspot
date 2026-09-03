using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using KeyEventArgs = System.Windows.Input.KeyEventArgs;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.UI;

/// <summary>
/// Main borderless launcher window. Implements the same interaction model as
/// PowerToys Run: type to search, Enter to execute, Esc to hide, global hotkey
/// to show/hide.
/// </summary>
public partial class MainWindow : Window
{
    private readonly SettingsService _settings;
    private readonly HistoryService _history;
    private readonly PluginManager _plugins;
    private readonly HotkeyService _hotkey = new();

    private CancellationTokenSource? _queryCts;
    private IReadOnlyList<ResultItem> _currentResults = Array.Empty<ResultItem>();
    private bool _isSettingsOpen;

    public bool AutoHideOnDeactivate { get; set; } = true;

    public MainWindow(SettingsService settings, HistoryService history, PluginManager plugins)
    {
        _settings = settings;
        _history = history;
        _plugins = plugins;

        InitializeComponent();

        ApplyPluginSettings();
        ResultList.ItemsSource = _currentResults;
        StatusText.Text = "Type to search · Alt+Space to toggle";
    }

    private void ApplyPluginSettings()
    {
        foreach (var plugin in _plugins.Plugins)
        {
            plugin.IsEnabled = _settings.IsPluginEnabled(plugin.Name);
        }
    }

    private void Window_SourceInitialized(object? sender, EventArgs e)
    {
        var ok = _hotkey.Register(this, _settings.Current.Hotkey);
        if (!ok)
        {
            StatusText.Text = $"Global hotkey {_settings.Current.Hotkey} is unavailable (in use?)";
        }
    }

    private void Window_Loaded(object? sender, RoutedEventArgs e)
    {
        CenterWindow();
        if (_settings.Current.ShowOnStartup)
        {
            ShowLauncher();
        }
        else
        {
            Hide();
        }
    }

    private void Window_Closing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        _queryCts?.Cancel();
        _hotkey.Dispose();
        _settings.Save();
    }

    private void Window_Deactivated(object? sender, EventArgs e)
    {
        if (AutoHideOnDeactivate && !_isSettingsOpen && IsVisible && !IsActive)
        {
            Hide();
        }
    }

    private void CenterWindow()
    {
        Left = Math.Max(0, (SystemParameters.PrimaryScreenWidth - Width) / 2);
        Top = Math.Max(40, SystemParameters.PrimaryScreenHeight * 0.12);
    }

    public void ShowLauncher()
    {
        CenterWindow();
        if (WindowState == WindowState.Minimized)
        {
            WindowState = WindowState.Normal;
        }

        Show();
        Activate();
        QueryBox.Focus();
        QueryBox.SelectAll();
    }

    private void ToggleLauncher()
    {
        if (IsVisible && IsActive)
        {
            Hide();
        }
        else
        {
            ShowLauncher();
        }
    }

    private async void QueryBox_TextChanged(object sender, System.Windows.Controls.TextChangedEventArgs e)
    {
        _queryCts?.Cancel();
        _queryCts?.Dispose();
        _queryCts = new CancellationTokenSource();

        var text = QueryBox.Text;
        var token = _queryCts.Token;
        try
        {
            var results = await _plugins.QueryAsync(text, token);
            if (token.IsCancellationRequested || !IsVisible)
            {
                return;
            }

            _currentResults = results;
            ResultList.ItemsSource = results;
            if (results.Count > 0)
            {
                ResultList.SelectedIndex = 0;
                ResultList.ScrollIntoView(results[0]);
            }

            StatusText.Text = results.Count switch
            {
                0 => "No results",
                1 => "1 result",
                _ => $"{results.Count} results",
            };
        }
        catch (OperationCanceledException)
        {
            // Newer query superseded this one.
        }
        catch (Exception ex)
        {
            StatusText.Text = "Search error: " + ex.Message;
        }
    }

    private void QueryBox_KeyDown(object sender, KeyEventArgs e)
    {
        switch (e.Key)
        {
            case Key.Escape:
                Hide();
                e.Handled = true;
                break;
            case Key.Down:
                MoveSelection(1);
                e.Handled = true;
                break;
            case Key.Up:
                MoveSelection(-1);
                e.Handled = true;
                break;
            case Key.Enter:
                e.Handled = true;
                _ = ExecuteSelectedAsync();
                break;
        }
    }

    private void ResultList_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter)
        {
            e.Handled = true;
            _ = ExecuteSelectedAsync();
        }
        else if (e.Key == Key.Escape)
        {
            e.Handled = true;
            Hide();
        }
    }

    private void ResultList_PreviewMouseDown(object sender, MouseButtonEventArgs e)
    {
        // Clicking the list should not steal focus from the query box.
        QueryBox.Focus();
    }

    private void ResultList_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        // Single-click a result to open it (file → open containing folder with
        // it selected; folder → open the folder). Enter on the keyboard works too.
        if (e.OriginalSource is not DependencyObject source)
        {
            return;
        }

        if (ItemsControl.ContainerFromElement(ResultList, source) is not ListBoxItem { DataContext: ResultItem item })
        {
            return;
        }

        e.Handled = true;
        _ = ExecuteItemAsync(item);
    }

    private void ResultList_PreviewMouseRightButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (e.OriginalSource is not DependencyObject source)
        {
            return;
        }

        if (ItemsControl.ContainerFromElement(ResultList, source) is not ListBoxItem { DataContext: ResultItem item })
        {
            return;
        }

        e.Handled = true;
        ResultList.SelectedItem = item;

        var path = item.ClipboardText;
        var hasPath = !string.IsNullOrEmpty(path) && (File.Exists(path) || Directory.Exists(path));

        var menu = new ContextMenu();
        var openFile = new MenuItem { Header = "打开文件", IsEnabled = hasPath && File.Exists(path) };
        openFile.Click += (_, _) => OpenDirectly(item);
        var openFolder = new MenuItem { Header = "打开所在路径", IsEnabled = hasPath };
        openFolder.Click += (_, _) => OpenContainingPath(item);
        var copyPath = new MenuItem { Header = "复制完整路径", IsEnabled = hasPath };
        copyPath.Click += (_, _) => CopyPath(item);
        menu.Items.Add(openFile);
        menu.Items.Add(openFolder);
        menu.Items.Add(new Separator());
        menu.Items.Add(copyPath);

        menu.Placement = PlacementMode.MousePoint;
        menu.IsOpen = true;
    }

    private static void OpenDirectly(ResultItem item)
    {
        var path = item.ClipboardText;
        if (string.IsNullOrEmpty(path))
        {
            return;
        }

        try
        {
            if (Directory.Exists(path))
            {
                Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true });
            }
            else if (File.Exists(path))
            {
                Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
            }
        }
        catch
        {
            // ignore
        }
    }

    private static void OpenContainingPath(ResultItem item)
    {
        var path = item.ClipboardText;
        if (string.IsNullOrEmpty(path))
        {
            return;
        }

        try
        {
            if (Directory.Exists(path))
            {
                Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true });
            }
            else if (File.Exists(path))
            {
                Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"") { UseShellExecute = true });
            }
        }
        catch
        {
            // ignore
        }
    }

    private void CopyPath(ResultItem item)
    {
        var path = item.ClipboardText;
        if (string.IsNullOrEmpty(path))
        {
            return;
        }

        try
        {
            Clipboard.SetText(path);
            StatusText.Text = "完整路径已复制到剪贴板";
        }
        catch
        {
            // Clipboard may be locked by another process.
        }
    }

    private void MoveSelection(int delta)
    {
        if (_currentResults.Count == 0)
        {
            return;
        }

        var index = ResultList.SelectedIndex;
        if (index < 0)
        {
            index = 0;
        }
        else
        {
            index += delta;
            if (index < 0)
            {
                index = 0;
            }
            else if (index >= _currentResults.Count)
            {
                index = _currentResults.Count - 1;
            }
        }

        ResultList.SelectedIndex = index;
        ResultList.ScrollIntoView(_currentResults[index]);
    }

    private async Task ExecuteSelectedAsync()
    {
        await ExecuteItemAsync(ResultList.SelectedItem as ResultItem ?? _currentResults.FirstOrDefault());
    }

    private async Task ExecuteItemAsync(ResultItem? item)
    {
        if (item is null)
        {
            return;
        }

        _history.Add(QueryBox.Text, item);

        if (item.CopyToClipboard && !string.IsNullOrEmpty(item.ClipboardText))
        {
            try
            {
                Clipboard.SetText(item.ClipboardText);
            }
            catch
            {
                // Clipboard may be locked by another process.
            }
        }

        try
        {
            if (item.Execute is not null)
            {
                await item.Execute();
            }
        }
        catch
        {
            StatusText.Text = "Action failed";
        }

        if (item.CloseAfterExecute)
        {
            Hide();
        }
        else
        {
            QueryBox.SelectAll();
        }
    }

    private void SettingsButton_Click(object sender, RoutedEventArgs e)
    {
        OpenSettings();
    }

    /// <summary>Opens the settings window (used by the taskbar/tray menu too).</summary>
    public void ShowSettingsWindow()
    {
        OpenSettings();
    }

    private void OpenSettings()
    {
        _isSettingsOpen = true;
        try
        {
            var window = new SettingsWindow(_settings, _plugins, App.Ntfs) { Owner = this };
            window.ShowDialog();
        }
        finally
        {
            _isSettingsOpen = false;
        }

        ApplyPluginSettings();
        if (!_hotkey.Register(this, _settings.Current.Hotkey))
        {
            StatusText.Text = $"Global hotkey {_settings.Current.Hotkey} is unavailable (in use?)";
        }
    }

    private void CloseButton_Click(object sender, RoutedEventArgs e)
    {
        // Hide to tray instead of exiting; right-click the tray icon to quit.
        _settings.Save();
        Hide();
    }
}
