param(
    [string]$Mode = "history",   # history | search | both
    [string]$Query = "steam",
    [string]$OutDir = ""
)

# Launches the freshly built hotspot-cpp.exe with --stay, types a query, and
# captures the launcher window so icon rendering can be inspected offline.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Win32VerifyX {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr hWnd, EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hWnd, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    public static void ForceFocus(IntPtr hwnd, IntPtr target) {
        uint mine = GetCurrentThreadId();
        uint theirs = (uint)GetWindowThreadProcessId(hwnd, IntPtr.Zero);
        bool attached = mine != theirs && AttachThreadInput(mine, theirs, true);
        SetForegroundWindow(hwnd);
        SetFocus(target);
        if (attached) AttachThreadInput(mine, theirs, false);
    }

    public static IntPtr FindForProcess(uint pid, string contains) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, lp) => {
            uint p = 0; GetWindowThreadProcessId(h, out p);
            if (p == pid && IsWindowVisible(h)) {
                StringBuilder sb = new StringBuilder(256);
                GetWindowTextW(h, sb, 256);
                if (contains.Length == 0 || sb.ToString().Contains(contains)) { found = h; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr FindEdit(IntPtr parent) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, delegate(IntPtr h, IntPtr lp) {
            StringBuilder sb = new StringBuilder(64);
            GetClassNameW(h, sb, 64);
            if (sb.ToString().ToUpperInvariant() == "EDIT") { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static string TextOf(IntPtr h) { StringBuilder sb = new StringBuilder(512); GetWindowTextW(h, sb, 512); return sb.ToString(); }
}
"@

$exe = "C:\Users\Mayn\Desktop\hotspot\PowerToysRunStandalone\publish\hotspot-cpp.exe"
$rootOut = "C:\Users\Mayn\Desktop\hotspot\PowerToysRunStandalone\artifacts"
if (-not $OutDir) { $OutDir = $rootOut }
if (!(Test-Path $exe)) { throw "EXE not found: $exe" }
if (!(Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

function Capture($p, $query, $out) {
    $p.Refresh()
    $hwnd = [Win32VerifyX]::FindForProcess([uint32]$p.Id, "hotspot")
    if ($hwnd -eq [IntPtr]::Zero) { throw "window not found" }
    [Win32VerifyX]::ShowWindow($hwnd, 5) | Out-Null
    [Win32VerifyX]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Milliseconds 600

    $edit = [Win32VerifyX]::FindEdit($hwnd)
    if ($edit -eq [IntPtr]::Zero) { throw "search box not found" }
    [Win32VerifyX]::ForceFocus($hwnd, $edit)
    Start-Sleep -Milliseconds 300

    # Ctrl+V into the focused search box; retry until the text lands.
    [System.Windows.Forms.Clipboard]::SetText($query)
    for ($i = 0; $i -lt 6; $i++) {
        [System.Windows.Forms.SendKeys]::SendWait("^a")
        [System.Windows.Forms.SendKeys]::SendWait("^v")
        Start-Sleep -Milliseconds 700
        if ([Win32VerifyX]::TextOf($edit) -eq $query) { break }
    }
    $actual = [Win32VerifyX]::TextOf($edit)
    Write-Output "QUERYBOX '$actual'"
    if ($actual -ne $query) { throw "query text did not land in the search box" }
    Start-Sleep -Seconds 5

    $rect = New-Object Win32VerifyX+RECT
    [Win32VerifyX]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bmp.Size)
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output "CAPTURED $out ($w x $h)"
}

$p = Start-Process -FilePath $exe -ArgumentList "--stay" -PassThru
Start-Sleep -Seconds 4
try {
    if ($Mode -eq "history" -or $Mode -eq "both") {
        Capture $p "history" (Join-Path $OutDir "verify-history.png")
    }
    if ($Mode -eq "search" -or $Mode -eq "both") {
        Capture $p $Query (Join-Path $OutDir "verify-search.png")
    }
} finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
exit 0
