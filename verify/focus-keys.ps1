# Focus guard for the launcher frame:
#   * the search box owns the keyboard focus when the launcher appears
#   * a no-op click on the frame's own background (status strip) keeps it there
#   * the right-click context menu gives it back when it closes
#   * typing keeps landing in the search box and Esc still closes the window
#
# The launcher shortcuts (Esc/Enter/arrows) live in the EDIT subclass, so a frame
# that keeps the focus silently kills the keyboard. `MainWindow::handleMessage`
# answers WM_SETFOCUS by handing the focus back to the search box
# (guarded by GetActiveWindow(), so the settings window keeps its own focus).
#
# Runs the real publish\hotspot-cpp.exe with --stay, moves the real mouse and
# types real keys; the cursor position is restored afterwards.
# Usage: powershell -ExecutionPolicy Bypass -File verify\focus-keys.ps1
param(
    [string]$Exe = "",
    [int]$WaitSeconds = 8
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms

$projectRoot = Split-Path -Parent $PSScriptRoot
$exeCandidates = @(
    (Join-Path $projectRoot "publish\hotspot-cpp.exe"),
    (Join-Path (Split-Path -Parent $projectRoot) "publish\hotspot-cpp.exe")
)
if (-not $Exe) { $Exe = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1 }
if (-not $Exe) { $Exe = $exeCandidates[0] }
if (!(Test-Path $Exe)) { throw "EXE not found: $Exe (build first: .\build.ps1)" }

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class FocusKeysX {
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO gti);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, StringBuilder lp);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, string lp);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, IntPtr extra);
    [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool altTab);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    public delegate bool EnumProc(IntPtr h, IntPtr p);

    [StructLayout(LayoutKind.Sequential)]
    public struct GUITHREADINFO {
        public int cbSize; public int flags;
        public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret;
        public int rcCaretLeft, rcCaretTop, rcCaretRight, rcCaretBottom;
    }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }

    public const uint WM_GETTEXT = 0x000D, WM_SETTEXT = 0x000C;
    public const uint EM_SETSEL = 0x00B1;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002, MOUSEEVENTF_LEFTUP = 0x0004;
    public const uint MOUSEEVENTF_RIGHTDOWN = 0x0008, MOUSEEVENTF_RIGHTUP = 0x0010;

    public static IntPtr FindWindow(uint pid, string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp != pid || !IsWindowVisible(h)) return true;
            StringBuilder sb = new StringBuilder(256); GetWindowTextW(h, sb, 256);
            if (sb.ToString() == title) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static IntPtr FindEdit(IntPtr parent) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (h, p) => {
            StringBuilder sb = new StringBuilder(64); GetClassNameW(h, sb, 64);
            if (sb.ToString().Equals("Edit", StringComparison.OrdinalIgnoreCase)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    // Cross-process safe (WM_GETTEXT and EM_SETSEL are marshalled by the system).
    public static string Text(IntPtr h) {
        StringBuilder sb = new StringBuilder(512);
        SendMessageW(h, WM_GETTEXT, (IntPtr)sb.Capacity, sb);
        return sb.ToString();
    }

    public static void SetText(IntPtr h, string text) {
        SendMessageW(h, WM_SETTEXT, IntPtr.Zero, text);
        // WM_SETTEXT leaves the caret at the start; put it at the end so typed
        // characters append (a launcher types into the box, after all).
        int len = Text(h).Length;
        SendMessageW(h, EM_SETSEL, (IntPtr)len, (IntPtr)len);
    }

    // Focus owner of the target thread (a cross-thread GetFocus() does not work).
    public static IntPtr FocusOf(IntPtr window) {
        uint pid;
        uint tid = GetWindowThreadProcessId(window, out pid);
        GUITHREADINFO gti = new GUITHREADINFO();
        gti.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
        if (GetGUIThreadInfo(tid, ref gti)) return gti.hwndFocus;
        return IntPtr.Zero;
    }

    // Click a point given in the window's client coordinates, with the real mouse.
    public static void ClickClient(IntPtr h, int x, int y) {
        PointAt(h, x, y);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(60);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, IntPtr.Zero);
    }

    public static void RightClickClient(IntPtr h, int x, int y) {
        PointAt(h, x, y);
        mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(60);
        mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, IntPtr.Zero);
    }

    private static void PointAt(IntPtr h, int x, int y) {
        POINT p; p.x = x; p.y = y;
        ClientToScreen(h, ref p);
        SetCursorPos(p.x, p.y);
        System.Threading.Thread.Sleep(150);
    }

    // The Windows foreground lock rejects a plain SetForegroundWindow from a
    // background script; borrow the foreground thread's input queue and fall back
    // to the legacy SwitchToThisWindow until the window really is in front, since
    // only real input carries the keyboard state.
    public static bool ForceForeground(IntPtr h) {
        for (int attempt = 0; attempt < 10; attempt++) {
            if (GetForegroundWindow() == h) return true;
            IntPtr fg = GetForegroundWindow();
            uint pidDummy;
            uint fgTid = GetWindowThreadProcessId(fg, out pidDummy);
            uint myTid = GetCurrentThreadId();
            bool attached = fgTid != 0 && AttachThreadInput(myTid, fgTid, true);
            ShowWindow(h, 5);   // SW_SHOW
            BringWindowToTop(h);
            SetForegroundWindow(h);
            if (attached) AttachThreadInput(myTid, fgTid, false);
            if (GetForegroundWindow() == h) return true;
            SwitchToThisWindow(h, false);
            if (GetForegroundWindow() == h) return true;
            System.Threading.Thread.Sleep(300);
        }
        return GetForegroundWindow() == h;
    }
}
"@

$failures = 0
function Assert([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "ok    $what" -ForegroundColor Green }
    else { Write-Host "FAIL  $what" -ForegroundColor Red; $script:failures++ }
}

$originalCursor = New-Object FocusKeysX+POINT
[FocusKeysX]::GetCursorPos([ref]$originalCursor) | Out-Null

$proc = Start-Process -FilePath $Exe -ArgumentList "--stay" -PassThru
try {
    $procId = [uint32]$proc.Id
    $main = [IntPtr]::Zero
    for ($i = 0; $i -lt ($WaitSeconds * 4); $i++) {
        Start-Sleep -Milliseconds 250
        $main = [FocusKeysX]::FindWindow($procId, "hotspot")
        if ($main -ne [IntPtr]::Zero) { break }
    }
    if ($main -eq [IntPtr]::Zero) { throw "launcher window not found for pid $procId" }
    $edit = [FocusKeysX]::FindEdit($main)
    if ($edit -eq [IntPtr]::Zero) { throw "search EDIT control not found" }
    Start-Sleep -Milliseconds 800

    if (-not [FocusKeysX]::ForceForeground($main)) {
        throw "cannot bring the launcher to the foreground; aborting so no keystroke leaks"
    }
    Start-Sleep -Milliseconds 400
    [FocusKeysX]::SetText($edit, "")
    Start-Sleep -Milliseconds 300

    Assert ([FocusKeysX]::FocusOf($main) -eq $edit) "focus starts on the search box"

    # A no-op click on the frame's own background (status strip: no row, no
    # button) must not take the keyboard away from the search box.
    $rect = New-Object FocusKeysX+RECT
    [FocusKeysX]::GetClientRect($main, [ref]$rect) | Out-Null
    $clickX = [int](($rect.right - $rect.left) / 2)
    $clickY = [int]($rect.bottom - 20)
    [FocusKeysX]::ClickClient($main, $clickX, $clickY)
    Start-Sleep -Milliseconds 500

    Assert ([FocusKeysX]::IsWindowVisible($main)) "the no-op click does not close the window"
    Assert ([FocusKeysX]::FocusOf($main) -eq $edit) "focus stays on the search box after clicking the background"

    [System.Windows.Forms.SendKeys]::SendWait("ab")
    Start-Sleep -Milliseconds 600
    $typed = [FocusKeysX]::Text($edit)
    Assert ($typed -eq "ab") "typing after the click still reaches the search box (got '$typed')"

    # A popup menu owns the activation while it is open: closing it must give the
    # focus back to the search box, not to the frame.
    [FocusKeysX]::SetText($edit, "fs hotspot")
    Start-Sleep -Milliseconds 1800
    Assert ([FocusKeysX]::Text($edit) -eq "fs hotspot") "the query text is in the search box"
    [FocusKeysX]::RightClickClient($main, 300, 90)   # row 0 (the list starts at y=68)
    Start-Sleep -Milliseconds 900
    [FocusKeysX]::ClickClient($main, $clickX, $clickY)   # dismiss the menu
    Start-Sleep -Milliseconds 900
    Assert ([FocusKeysX]::IsWindowVisible($main)) "the context menu did not close the window"
    Assert ([FocusKeysX]::FocusOf($main) -eq $edit) "focus returns to the search box after the context menu"

    [System.Windows.Forms.SendKeys]::SendWait("x")
    Start-Sleep -Milliseconds 700
    $typed = [FocusKeysX]::Text($edit)
    Assert ($typed.EndsWith("x")) "typing after the context menu still reaches the search box (got '$typed')"

    Assert ([FocusKeysX]::GetForegroundWindow() -eq $main) "launcher keeps the foreground through the clicks"
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    Start-Sleep -Milliseconds 700
    Assert (-not [FocusKeysX]::IsWindowVisible($main)) "Esc still closes the window after the clicks"
} finally {
    [FocusKeysX]::SetCursorPos($originalCursor.x, $originalCursor.y) | Out-Null
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host ""
if ($failures -eq 0) {
    Write-Host "PASS  focus-keys" -ForegroundColor Green
    exit 0
}
Write-Host "FAIL  focus-keys ($failures failure(s))" -ForegroundColor Red
exit 1
