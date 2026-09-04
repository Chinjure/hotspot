param(
    [string]$Mode = "main",   # main | search | settings
    [string]$Out = "",
    [string]$Query = "the church",
    [switch]$Hover
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Win32CaptureX {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    public static IntPtr FindByTitle(string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, lp) => {
            StringBuilder sb = new StringBuilder(256);
            GetWindowTextW(h, sb, 256);
            if (sb.ToString() == title && IsWindowVisible(h)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
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
}
"@

$exe = "C:\Users\Mayn\Desktop\hotspot\PowerToysRunStandalone\publish\hotspot-cpp.exe"
$rootOut = "C:\Users\Mayn\Desktop\hotspot\PowerToysRunStandalone\artifacts"
if (-not $Out) {
    switch ($Mode) {
        "search"   { $Out = Join-Path $rootOut "cpp-launcher-search.png" }
        "settings" { $Out = Join-Path $rootOut "cpp-settings-window.png" }
        default    { $Out = Join-Path $rootOut "cpp-launcher-window.png" }
    }
}
if (!(Test-Path $exe)) { throw "EXE not found: $exe" }
if (!(Test-Path (Split-Path $Out))) { New-Item -ItemType Directory -Path (Split-Path $Out) -Force | Out-Null }

$args = "--stay"
if ($Mode -eq "settings") { $args = "--settings --stay" }
$p = Start-Process -FilePath $exe -ArgumentList $args -PassThru
Start-Sleep -Seconds 5

if ($Hover) {
    $p.Refresh()
    $hoverHwnd = [Win32CaptureX]::FindForProcess([uint32]$p.Id, "hotspot")
    if ($hoverHwnd -ne [IntPtr]::Zero) {
        $rect = New-Object Win32CaptureX+RECT
        [Win32CaptureX]::GetWindowRect($hoverHwnd, [ref]$rect) | Out-Null
        [Win32CaptureX]::SetCursorPos(($rect.Right - 10), ($rect.Top + 34)) | Out-Null
        Start-Sleep -Seconds 1
    }
}

if ($Mode -eq "search") {
    $p.Refresh()
    $mainHwnd = [Win32CaptureX]::FindForProcess([uint32]$p.Id, "hotspot")
    if ($mainHwnd -ne [IntPtr]::Zero) {
        Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32FocusX {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
"@
        [Win32FocusX]::ShowWindow($mainHwnd, 5) | Out-Null
        [Win32FocusX]::SetForegroundWindow($mainHwnd) | Out-Null
    }
    [System.Windows.Forms.Clipboard]::SetText($Query)
    [System.Windows.Forms.SendKeys]::SendWait("^v")
    Start-Sleep -Seconds 5
}

$hwnd = [IntPtr]::Zero
$p.Refresh()
if ($Mode -eq "settings") {
    $hwnd = [Win32CaptureX]::FindForProcess([uint32]$p.Id, "Settings")
}
if ($hwnd -eq [IntPtr]::Zero) {
    $hwnd = [Win32CaptureX]::FindForProcess([uint32]$p.Id, "hotspot")
}
if ($hwnd -eq [IntPtr]::Zero) {
    $hwnd = $p.MainWindowHandle
}
if ($hwnd -eq [IntPtr]::Zero) {
    $hwnd = [Win32CaptureX]::FindByTitle("hotspot")
}

$width = 0; $height = 0; $left = 0; $top = 0
if ($hwnd -ne [IntPtr]::Zero) {
    $rect = New-Object Win32CaptureX+RECT
    [Win32CaptureX]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $left = $rect.Left; $top = $rect.Top
    $width = $rect.Right - $rect.Left; $height = $rect.Bottom - $rect.Top
}
if ($width -le 0 -or $height -le 0) {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $left = $bounds.Left; $top = $bounds.Top
    $width = $bounds.Width; $height = $bounds.Height
}

$bmp = New-Object System.Drawing.Bitmap($width, $height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($left, $top, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()

Write-Output "CAPTURED $Out ($width x $height)"
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
exit 0
