using System.Runtime.InteropServices;
using System.Text;
using PowerToysRunStandalone.Infrastructure;

namespace PowerToysRunStandalone.Plugins;

/// <summary>
/// Window Walker plugin — equivalent to PowerToys Run's
/// Microsoft.Plugin.WindowWalker. Enumerates visible top-level windows and
/// switches to the matching one. Usage: "ww notepad" or just type a title.
/// </summary>
public sealed class WindowWalkerPlugin : IPlugin
{
    public string Name => "Window Walker";

    public string Description => "Switch to an open window by its title (ww <text>).";

    public string ActionKeyword => "<";

    public int Priority => 4;

    public bool IsEnabled { get; set; } = true;

    public IReadOnlyList<ResultItem> Query(Query query, CancellationToken cancellationToken)
    {
        var search = query.Search;
        var useWindowWalker = search.StartsWith("ww ", StringComparison.OrdinalIgnoreCase)
                              || search.StartsWith("<", StringComparison.OrdinalIgnoreCase);
        if (search.StartsWith("ww ", StringComparison.OrdinalIgnoreCase))
        {
            search = search[3..].Trim();
        }
        else if (search.StartsWith("<", StringComparison.OrdinalIgnoreCase))
        {
            search = search[1..].Trim();
        }

        if (search.Length < 2)
        {
            return Array.Empty<ResultItem>();
        }

        // Don't fight with Shell/WebSearch/Calculator prefixes.
        if (!useWindowWalker && (search[0] is '?' or '>' or '$' or '=' or '!' or '#' or '@' or '_' or ':' or ')'))
        {
            return Array.Empty<ResultItem>();
        }

        var windows = WindowWalker.GetOpenWindows();
        return windows
            .Where(w => w.Title.Contains(search, StringComparison.OrdinalIgnoreCase))
            .Take(20)
            .Select(w => new ResultItem
            {
                Title = w.Title,
                Subtitle = $"{w.ProcessName} · HWND 0x{w.Handle.ToInt64():X}",
                Icon = "🪟",
                PluginName = Name,
                Execute = () =>
                {
                    WindowWalker.Activate(w.Handle);
                    return Task.CompletedTask;
                },
            })
            .ToArray();
    }

    private static class WindowWalker
    {
        private const uint SwRestore = 9;

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern IntPtr GetWindow(IntPtr hWnd, uint uCmd);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

        [DllImport("user32.dll")]
        private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

        [DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern bool IsIconic(IntPtr hWnd);

        private const uint GwOwner = 4;

        private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

        public static IReadOnlyList<WindowEntry> GetOpenWindows()
        {
            var windows = new List<WindowEntry>();
            EnumWindows((hWnd, _) =>
            {
                if (!IsWindowVisible(hWnd))
                {
                    return true;
                }

                var owner = GetWindow(hWnd, GwOwner);
                if (owner != IntPtr.Zero)
                {
                    return true;
                }

                var title = GetTitle(hWnd);
                if (string.IsNullOrWhiteSpace(title))
                {
                    return true;
                }

                GetWindowThreadProcessId(hWnd, out var pid);
                var processName = string.Empty;
                try
                {
                    using var process = System.Diagnostics.Process.GetProcessById((int)pid);
                    processName = process.ProcessName;
                }
                catch
                {
                    // Process may have exited.
                }

                windows.Add(new WindowEntry(hWnd, title, processName));
                return true;
            }, IntPtr.Zero);
            return windows;
        }

        public static void Activate(IntPtr hWnd)
        {
            if (IsIconic(hWnd))
            {
                ShowWindow(hWnd, (int)SwRestore);
            }

            SetForegroundWindow(hWnd);
        }

        private static string GetTitle(IntPtr hWnd)
        {
            var builder = new StringBuilder(512);
            GetWindowText(hWnd, builder, builder.Capacity);
            return builder.ToString();
        }
    }

    private sealed record WindowEntry(IntPtr Handle, string Title, string ProcessName);
}
