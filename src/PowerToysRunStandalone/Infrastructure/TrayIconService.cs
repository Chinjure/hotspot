using System.Drawing;
using System.Windows.Forms;

namespace PowerToysRunStandalone.Infrastructure;

/// <summary>
/// System-tray presence for the launcher. Because the main window is a
/// borderless, auto-hiding launcher (no taskbar button while hidden), a tray
/// icon keeps the app reachable: left-click or "显示启动器" brings it back,
/// "设置" opens settings, "退出" really quits.
/// </summary>
public sealed class TrayIconService : IDisposable
{
    private readonly NotifyIcon _notifyIcon;

    public TrayIconService(Action showLauncher, Action showSettings, Action exit)
    {
        var menu = new ContextMenuStrip();
        var showItem = new ToolStripMenuItem("显示启动器");
        showItem.Click += (_, _) => showLauncher();
        var settingsItem = new ToolStripMenuItem("设置");
        settingsItem.Click += (_, _) => showSettings();
        var exitItem = new ToolStripMenuItem("退出");
        exitItem.Click += (_, _) => exit();
        menu.Items.AddRange(new ToolStripItem[] { showItem, settingsItem, exitItem });

        _notifyIcon = new NotifyIcon
        {
            Icon = SystemIcons.Application,
            Text = "PowerToys Run Standalone — 左键显示 · 右键菜单",
            Visible = true,
            ContextMenuStrip = menu,
        };
        _notifyIcon.DoubleClick += (_, _) => showLauncher();
        _notifyIcon.MouseClick += (_, e) =>
        {
            if (e.Button == MouseButtons.Left)
            {
                showLauncher();
            }
        };
    }

    public void Dispose()
    {
        _notifyIcon.Visible = false;
        _notifyIcon.Dispose();
    }
}
