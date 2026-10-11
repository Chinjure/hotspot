Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class CaretBaseX {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
}
"@
# Native EDIT control baseline: WinForms TextBox wraps the Win32 EDIT class, so
# this reports what the OS itself does with the caret keys.
$f = New-Object System.Windows.Forms.Form
$f.Width = 320; $f.Height = 120
$t = New-Object System.Windows.Forms.TextBox
$t.Text = "foo bar baz"
$t.Width = 260
$f.Controls.Add($t)
$f.Show()

# Foreground lock: borrow the current foreground thread so SetForegroundWindow sticks.
$fg = [CaretBaseX]::GetForegroundWindow()
$fgTid = [CaretBaseX]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
$myTid = [CaretBaseX]::GetCurrentThreadId()
[CaretBaseX]::AttachThreadInput($myTid, $fgTid, $true) | Out-Null
[CaretBaseX]::ShowWindow($f.Handle, 5) | Out-Null
[CaretBaseX]::SetForegroundWindow($f.Handle) | Out-Null
[CaretBaseX]::SetFocus($t.Handle) | Out-Null
[CaretBaseX]::AttachThreadInput($myTid, $fgTid, $false) | Out-Null
Start-Sleep -Milliseconds 800
"foreground is form: " + ([CaretBaseX]::GetForegroundWindow() -eq $f.Handle)

function Report([string]$name, [string]$keys, [int]$start) {
    if ([CaretBaseX]::GetForegroundWindow() -ne $f.Handle) { return "$name -> SKIPPED (no focus)" }
    $t.SelectionStart = $start
    $t.SelectionLength = 0
    [System.Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 150
    [System.Windows.Forms.SendKeys]::SendWait($keys)
    # No Application.Run here, so pump the queue ourselves or the posted key
    # messages are never dispatched to the EDIT control.
    [System.Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 150
    [System.Windows.Forms.Application]::DoEvents()
    "{0,-22} -> sel {1}..{2}" -f $name, $t.SelectionStart, ($t.SelectionStart + $t.SelectionLength)
}

Report "Left from end" "{LEFT}" 11
Report "Right from 0" "{RIGHT}" 0
Report "Home" "{HOME}" 11
Report "End" "{END}" 0
Report "Ctrl+Right from 0" "^{RIGHT}" 0
Report "Ctrl+Right from 4" "^{RIGHT}" 4
Report "Ctrl+Left from end" "^{LEFT}" 11
Report "Delete at 0" "{DELETE}" 0
"text after delete: '" + $t.Text + "'"
$f.Close()
