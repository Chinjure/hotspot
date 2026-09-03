using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;

namespace PowerToysRunStandalone.Infrastructure;

/// <summary>
/// Global hotkey registration using Win32 RegisterHotKey. This is the same
/// mechanism PowerToys Run uses for its default Alt+Space activation.
/// </summary>
public sealed class HotkeyService : IDisposable
{
    private const int WmHotkey = 0x0312;

    private const uint ModAlt = 0x0001;
    private const uint ModControl = 0x0002;
    private const uint ModShift = 0x0004;
    private const uint ModWin = 0x0008;

    private readonly int _hotkeyId = 0xB00F;

    private HwndSource? _source;
    private IntPtr _hwnd;
    private bool _registered;
    private string? _current;

    /// <summary>Raised whenever the registered global hotkey is pressed.</summary>
    public event Action? HotkeyPressed;

    public string? CurrentShortcut => _current;

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool UnregisterHotKey(IntPtr hWnd, int id);

    public bool Register(Window window, string shortcut)
    {
        Unregister();

        _hwnd = new WindowInteropHelper(window).Handle;
        _source = HwndSource.FromHwnd(_hwnd);

        if (!TryParseShortcut(shortcut, out var modifiers, out var key))
        {
            return false;
        }

        _current = shortcut;
        _source?.AddHook(WndProc);
        _registered = RegisterHotKey(_hwnd, _hotkeyId, modifiers, key);
        return _registered;
    }

    public void Unregister()
    {
        if (_registered && _hwnd != IntPtr.Zero)
        {
            UnregisterHotKey(_hwnd, _hotkeyId);
        }

        if (_source is not null)
        {
            _source.RemoveHook(WndProc);
            _source = null;
        }

        _registered = false;
        _current = null;
    }

    public static bool TryParseShortcut(string shortcut, out uint modifiers, out uint key)
    {
        modifiers = 0;
        key = 0;

        if (string.IsNullOrWhiteSpace(shortcut))
        {
            return false;
        }

        var parts = shortcut.Split('+', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length == 0)
        {
            return false;
        }

        for (var i = 0; i < parts.Length - 1; i++)
        {
            modifiers |= parts[i].ToLowerInvariant() switch
            {
                "alt" => ModAlt,
                "ctrl" or "control" => ModControl,
                "shift" => ModShift,
                "win" or "windows" => ModWin,
                _ => 0,
            };
        }

        var last = parts[^1].ToLowerInvariant();
        key = last switch
        {
            "space" => 0x20,
            "enter" or "return" => 0x0D,
            "esc" or "escape" => 0x1B,
            "tab" => 0x09,
            "backspace" => 0x08,
            "delete" => 0x2E,
            "insert" => 0x2D,
            "up" => 0x26,
            "down" => 0x28,
            "left" => 0x25,
            "right" => 0x27,
            "home" => 0x24,
            "end" => 0x23,
            "pageup" => 0x21,
            "pagedown" => 0x22,
            _ => ParseKeyOrFunctionKey(last),
        };

        return key != 0;
    }

    private static uint ParseKeyOrFunctionKey(string token)
    {
        if (token.Length == 1 && token[0] is (>= 'a' and <= 'z') or (>= '0' and <= '9'))
        {
            return (uint)char.ToUpperInvariant(token[0]);
        }

        if (token.StartsWith('f') && token.Length > 1 && int.TryParse(token.AsSpan(1), out var f) && f is >= 1 and <= 24)
        {
            return (uint)(0x70 + f - 1);
        }

        return 0;
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WmHotkey && wParam.ToInt32() == _hotkeyId)
        {
            HotkeyPressed?.Invoke();
            handled = true;
        }

        return IntPtr.Zero;
    }

    public void Dispose()
    {
        Unregister();
    }
}
