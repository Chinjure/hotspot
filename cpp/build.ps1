param(
    [string]$Configuration = "Release",
    [string]$Output = "publish"
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptRoot
$srcDir = Join-Path $scriptRoot "src"
$buildDir = Join-Path $scriptRoot "build"
$outDir = Join-Path $repoRoot $Output

New-Item -ItemType Directory -Force -Path $buildDir, $outDir | Out-Null

# ---- Locate Visual Studio C++ toolset via vswhere ----
$programFilesX86 = ${env:ProgramFiles(x86)}
if (-not $programFilesX86) { $programFilesX86 = "C:\Program Files (x86)" }
$vswhere = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at $vswhere"
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vsPath) {
    throw "Visual Studio C++ toolset not found. Install the 'Desktop development with C++' workload."
}
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found: $vcvars"
}

# ---- Collect sources and per-file object names ----
$sources = Get-ChildItem -Recurse -Filter *.cpp -Path $srcDir | ForEach-Object { $_.FullName }
if (-not $sources) { throw "No .cpp sources found under $srcDir" }

$objs = @()
foreach ($src in $sources) {
    $rel = $src.Substring($srcDir.Length).TrimStart('\', '/').Replace('\', '_').Replace('/', '_')
    $objs += (Join-Path $buildDir ($rel + ".obj"))
}

# ---- Generate a .cmd so vcvarsall + cl/link/rc run in one environment ----
$cmdFile = Join-Path $buildDir "_build.cmd"
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("@echo off")
$lines.Add("call `"$vcvars`" x64 >nul 2>&1")

for ($i = 0; $i -lt $sources.Count; $i++) {
    $src = $sources[$i]
    $obj = $objs[$i]
    $compile = "cl.exe /nologo /c /EHsc /W4 /utf-8 /O2 /std:c++20 " +
               "/DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS " +
               "/I`"$srcDir`" /Fo`"$obj`" `"$src`""
    $lines.Add($compile)
    # A failed compile must stop the build; otherwise a stale .obj from a
    # previous run would be linked silently.
    $lines.Add("if errorlevel 1 exit /b 1")
}

$lines.Add("rc.exe /nologo /fo `"$buildDir\app.res`" `"$srcDir\app.rc`"")
$lines.Add("if errorlevel 1 exit /b 1")

$libs = "user32.lib shell32.lib gdi32.lib d2d1.lib dwrite.lib dwmapi.lib shlwapi.lib advapi32.lib comctl32.lib imm32.lib msimg32.lib ole32.lib oleaut32.lib"
$linkLine = "link.exe /nologo /SUBSYSTEM:WINDOWS /OUT:`"$outDir\hotspot-cpp.exe`" " + ($objs -join " ") + " `"$buildDir\app.res`" $libs"
$lines.Add($linkLine)
$lines.Add("exit /b %errorlevel%")

# ASCII is safe for this repository path (C:\Users\Mayn\Desktop\hotspot\...).
[System.IO.File]::WriteAllText($cmdFile, ($lines -join "`r`n"), [System.Text.Encoding]::ASCII)

Write-Host "==> hotspot-cpp.exe (MSVC x64 via PowerShell)" -ForegroundColor Cyan
Write-Host "    vcvarsall: $vcvars"
Write-Host "    output:    $outDir\hotspot-cpp.exe"
cmd.exe /d /c "`"$cmdFile`""
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "==> Build OK: $outDir\hotspot-cpp.exe" -ForegroundColor Green
Get-Item (Join-Path $outDir "hotspot-cpp.exe") | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
