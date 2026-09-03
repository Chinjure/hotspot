# Quick smoke test: start the published exe, wait a few seconds, verify it is
# still alive, then stop it.
$ErrorActionPreference = "Stop"
$exe = "C:\Users\Mayn\Desktop\hotspot\PowerToysRunStandalone\publish\PowerToysRunStandalone.exe"

if (!(Test-Path $exe)) {
    throw "EXE not found: $exe"
}

$p = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 8

if ($p.HasExited) {
    Write-Output "EXITED code=$($p.ExitCode)"
    exit 1
}

Write-Output "RUNNING pid=$($p.Id)"
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
exit 0
