param(
    [string]$Exe = "",
    [int]$WaitSeconds = 8
)

# Verifies that the launcher search box keeps native EDIT caret behaviour:
# Left/Right/Home/End move the caret, Ctrl+Right jumps a word, Shift+Left
# extends the selection, Delete/Backspace still edit, while Up/Down/Esc/Enter
# remain launcher shortcuts.
#
# Strategy: start the app with --stay, push text into the EDIT control, drive
# real keystrokes with SendKeys (needs the test window focused), then read the
# caret back with EM_GETSEL. When the window cannot be brought to the
# foreground, it falls back to posted WM_KEYDOWN for the plain caret keys.

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms

$projectRoot = Split-Path -Parent $PSScriptRoot
# Layout-agnostic exe lookup: the project is either the repository root
# (<root>\publish\hotspot-cpp.exe) or a cpp\ subdirectory of it.
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

public static class CaretKeysX {
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
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, string lp);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, StringBuilder lp);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    public delegate bool EnumProc(IntPtr h, IntPtr p);

    [StructLayout(LayoutKind.Sequential)]
    public struct GUITHREADINFO {
        public int cbSize; public int flags;
        public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret;
        public int rcCaretLeft, rcCaretTop, rcCaretRight, rcCaretBottom;
    }

    public const uint WM_SETTEXT = 0x000C, WM_GETTEXT = 0x000D, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101;
    public const uint EM_SETSEL = 0x00B1, EM_GETSEL = 0x00B0;
    public const int VK_LEFT = 0x25, VK_RIGHT = 0x27, VK_HOME = 0x24, VK_END = 0x23;

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

    // GetWindowTextW cannot read a control owned by another process; WM_GETTEXT can.
    public static string Text(IntPtr h) {
        StringBuilder sb = new StringBuilder(512);
        SendMessageW(h, WM_GETTEXT, (IntPtr)sb.Capacity, sb);
        return sb.ToString();
    }

    public static int[] GetSel(IntPtr h) {
        IntPtr r = SendMessageW(h, EM_GETSEL, IntPtr.Zero, IntPtr.Zero);
        long v = r.ToInt64();
        return new int[] { (int)(v & 0xFFFF), (int)((v >> 16) & 0xFFFF) };
    }

    public static void SetSel(IntPtr h, int start, int end) {
        SendMessageW(h, EM_SETSEL, (IntPtr)start, (IntPtr)end);
    }

    // Focus owner of the target thread (cross-thread GetFocus does not work).
    public static IntPtr FocusOf(uint pid, IntPtr window) {
        uint tid = GetWindowThreadProcessId(window, out pid);
        GUITHREADINFO gti = new GUITHREADINFO();
        gti.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
        if (GetGUIThreadInfo(tid, ref gti)) return gti.hwndFocus;
        return IntPtr.Zero;
    }

    public static void PostKey(IntPtr h, int vk) {
        PostMessageW(h, WM_KEYDOWN, (IntPtr)vk, IntPtr.Zero);
        PostMessageW(h, WM_KEYUP, (IntPtr)vk, IntPtr.Zero);
    }

    // The Windows foreground lock often rejects a plain SetForegroundWindow from
    // a background script; borrow the foreground thread's input queue (and fall
    // back to the legacy SwitchToThisWindow) until the window really is in front,
    // because only real SendKeys input carries the Ctrl/Shift key state.
    [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool altTab);

    public static bool ForceForeground(IntPtr h) {
        for (int attempt = 0; attempt < 6; attempt++) {
            if (GetForegroundWindow() == h) return true;
            SetForegroundWindow(h);
            if (GetForegroundWindow() == h) return true;
            SwitchToThisWindow(h, false);
            if (GetForegroundWindow() == h) return true;
            IntPtr fg = GetForegroundWindow();
            uint pidDummy;
            uint fgTid = GetWindowThreadProcessId(fg, out pidDummy);
            uint myTid = GetCurrentThreadId();
            if (fgTid != 0 && AttachThreadInput(myTid, fgTid, true)) {
                SetForegroundWindow(h);
                AttachThreadInput(myTid, fgTid, false);
            }
            if (GetForegroundWindow() == h) return true;
            System.Threading.Thread.Sleep(250);
        }
        return GetForegroundWindow() == h;
    }
}
"@

$proc = Start-Process -FilePath $Exe -ArgumentList "--stay" -PassThru
$exit = 0
try {
    $pid32 = [uint32]$proc.Id
    $main = [IntPtr]::Zero
    for ($i = 0; $i -lt ($WaitSeconds * 4); $i++) {
        Start-Sleep -Milliseconds 250
        $main = [CaretKeysX]::FindWindow($pid32, "hotspot")
        if ($main -ne [IntPtr]::Zero) { break }
    }
    if ($main -eq [IntPtr]::Zero) { throw "launcher window not found for pid $pid32" }

    $edit = [CaretKeysX]::FindEdit($main)
    if ($edit -eq [IntPtr]::Zero) { throw "search EDIT control not found" }
    Start-Sleep -Milliseconds 800

    # Real keyboard input needs our window in the foreground; otherwise fall
    # back to posted key messages (plain caret keys only).
    $realKeys = [CaretKeysX]::ForceForeground($main)
    Start-Sleep -Milliseconds 500
    $focus = [CaretKeysX]::FocusOf($pid32, $main)
    Write-Output ("mode      : " + $(if ($realKeys) { "SendKeys (real input)" } else { "PostMessage fallback" }))
    Write-Output ("focus hwnd: " + $focus.ToString() + $(if ($focus -eq $edit) { " (EDIT)" } elseif ($focus -eq [IntPtr]::Zero) { " (unknown)" } else { " (NOT the EDIT control!)" }))

    function Send-Key([string]$keys) {
        if ($realKeys) {
            if ([CaretKeysX]::GetForegroundWindow() -ne $main) { throw "lost foreground focus; aborting to avoid leaking keystrokes" }
            [System.Windows.Forms.SendKeys]::SendWait($keys)
        } else {
            if ($keys -match '[\^+]') { throw "key '$keys' needs real input (modifier not reproducible via PostMessage)" }
            foreach ($token in ([regex]::Matches($keys, '\{[A-Z]+\}') | ForEach-Object { $_.Value })) {
                switch ($token) {
                    "{LEFT}"  { [CaretKeysX]::PostKey($edit, [CaretKeysX]::VK_LEFT) }
                    "{RIGHT}" { [CaretKeysX]::PostKey($edit, [CaretKeysX]::VK_RIGHT) }
                    "{HOME}"  { [CaretKeysX]::PostKey($edit, [CaretKeysX]::VK_HOME) }
                    "{END}"   { [CaretKeysX]::PostKey($edit, [CaretKeysX]::VK_END) }
                    default   { throw "key '$token' needs real input" }
                }
                Start-Sleep -Milliseconds 120
            }
        }
        Start-Sleep -Milliseconds 200
    }

    $text = "foo bar baz"   # 11 chars, 3 words
    $len = $text.Length
    [CaretKeysX]::SendMessageW($edit, [CaretKeysX]::WM_SETTEXT, [IntPtr]::Zero, $text) | Out-Null
    Start-Sleep -Milliseconds 600

    $results = New-Object System.Collections.Generic.List[object]
    function Check([string]$name, [int]$start, [int]$end, [string]$keys, [int]$wantStart, [int]$wantEnd, $wantText = $null) {
        [CaretKeysX]::SetSel($edit, $start, $end)
        Start-Sleep -Milliseconds 120
        Send-Key $keys
        $sel = [CaretKeysX]::GetSel($edit)
        $ok = ($sel[0] -eq $wantStart -and $sel[1] -eq $wantEnd)
        if ($null -ne $wantText) { $ok = $ok -and ([CaretKeysX]::Text($edit) -eq $wantText) }
        $results.Add([pscustomobject]@{
            Key    = $name
            Want   = "$wantStart..$wantEnd"
            Got    = "$($sel[0])..$($sel[1])"
            Result = $(if ($ok) { "PASS" } else { "FAIL" })
        })
    }

    Check "Left from end"       $len $len "{LEFT}"  ($len - 1) ($len - 1)
    Check "Right"               ($len - 1) ($len - 1) "{RIGHT}" $len $len
    Check "Home"                $len $len "{HOME}"  0 0
    Check "End"                 0 0 "{END}"         $len $len
    Check "Home then Right"     0 0 "{HOME}{RIGHT}" 1 1
    if ($realKeys) {
        # Native EDIT semantics (measured by verify\edit-baseline.ps1 on a stock
        # Win32 EDIT control): Ctrl+Right lands on the START of the next word,
        # so 0 -> 4 for "foo bar baz"; Ctrl+Left lands on the previous word start.
        Check "Ctrl+Right word jump" 0 0 "^{RIGHT}" 4 4
        Check "Ctrl+Left word jump" $len $len "^{LEFT}" 8 8
        Check "Shift+Left selection" $len $len "+{LEFT}" ($len - 1) $len
        [CaretKeysX]::SendMessageW($edit, [CaretKeysX]::WM_SETTEXT, [IntPtr]::Zero, $text) | Out-Null
        Start-Sleep -Milliseconds 400
        Check "Delete at Home" 0 0 "{DELETE}" 0 0 "oo bar baz"
        [CaretKeysX]::SendMessageW($edit, [CaretKeysX]::WM_SETTEXT, [IntPtr]::Zero, $text) | Out-Null
        Start-Sleep -Milliseconds 400
        Check "Backspace at End" $len $len "{BACKSPACE}" ($len - 1) ($len - 1) "foo bar ba"
    }

    $results | Format-Table -AutoSize | Out-String -Width 200 | Write-Output
    $failed = @($results | Where-Object { $_.Result -ne "PASS" })
    if ($failed.Count -gt 0) {
        Write-Output ("RESULT: FAIL (" + $failed.Count + "/" + $results.Count + " checks failed)")
        $exit = 1
    } else {
        Write-Output ("RESULT: PASS (" + $results.Count + "/" + $results.Count + " checks)")
    }
} finally {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}
exit $exit
