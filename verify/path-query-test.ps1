# Compiles and runs the path-search matcher unit tests (verify/path_query_test.cpp)
# with the same MSVC toolset the product build uses. No index, no GUI.
$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcDir = Join-Path (Split-Path -Parent $scriptRoot) "src"
$outDir = Join-Path $scriptRoot "out"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$programFilesX86 = ${env:ProgramFiles(x86)}
if (-not $programFilesX86) { $programFilesX86 = "C:\Program Files (x86)" }
$vswhere = Join-Path $programFilesX86 "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found at $vswhere" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vsPath) { throw "Visual Studio C++ toolset not found." }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"

$exe = Join-Path $outDir "path_query_test.exe"
$cmdFile = Join-Path $outDir "_test.cmd"
$lines = @(
    "@echo off",
    "call `"$vcvars`" x64 >nul 2>&1",
    "cl.exe /nologo /EHsc /W4 /utf-8 /std:c++20 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS /I`"$srcDir`" /Fo`"$outDir\\`" /Fe`"$exe`" `"$scriptRoot\path_query_test.cpp`" `"$srcDir\path_query.cpp`" `"$srcDir\file_rank.cpp`" `"$srcDir\winutil.cpp`" user32.lib shell32.lib shlwapi.lib advapi32.lib ole32.lib",
    "if errorlevel 1 exit /b 1",
    "`"$exe`"",
    "exit /b %errorlevel%"
)
[System.IO.File]::WriteAllText($cmdFile, ($lines -join "`r`n"), [System.Text.Encoding]::ASCII)
cmd.exe /d /c "`"$cmdFile`""
if ($LASTEXITCODE -ne 0) { throw "path_query_test failed with exit code $LASTEXITCODE" }
