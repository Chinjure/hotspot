# Launch the published EXE with --stay, type a calculator query, capture the
# window/desktop to verify search results render, then stop the process.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32Capture {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
}
"@

$exe = "C:\Users\Mayn\Desktop\hotspot\PowerToysRunStandalone\publish\hotspot.exe"
$out = "C:\Users\Mayn\Desktop\hotspot\PowerToysRunStandalone\artifacts\launcher-search.png"

if (!(Test-Path $exe)) { throw "EXE not found: $exe" }
if (!(Test-Path (Split-Path $out))) { New-Item -ItemType Directory -Path (Split-Path $out) | Out-Null }

$p = Start-Process -FilePath $exe -ArgumentList '--stay' -PassThru
Start-Sleep -Seconds 4

# Make sure the launcher window gets focus, then type an arithmetic expression.
$p.Refresh()
if ($p.MainWindowHandle -ne 0) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win32Focus {
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
"@
    [Win32Focus]::ShowWindow($p.MainWindowHandle, 5) | Out-Null
    [Win32Focus]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
}

[System.Windows.Forms.Clipboard]::SetText("1+2*3")
[System.Windows.Forms.SendKeys]::SendWait("^v")
Start-Sleep -Seconds 4

$width = 0
$height = 0
$left = 0
$top = 0

if ($p.MainWindowHandle -ne 0) {
    $rect = New-Object Win32Capture+RECT
    [Win32Capture]::GetWindowRect($p.MainWindowHandle, [ref]$rect) | Out-Null
    $left = $rect.Left
    $top = $rect.Top
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
}

if ($width -le 0 -or $height -le 0) {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $left = $bounds.Left
    $top = $bounds.Top
    $width = $bounds.Width
    $height = $bounds.Height
}

$bmp = New-Object System.Drawing.Bitmap($width, $height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($left, $top, 0, 0, $bmp.Size)
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()

Write-Output "CAPTURED $out ($width x $height)"

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
exit 0
