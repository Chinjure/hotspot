# Installs the .NET 8 SDK into the current user's profile (no admin required).
# Usage:
#   powershell.exe -ExecutionPolicy Bypass -File scripts\install-dotnet.ps1
$ErrorActionPreference = "Stop"

$installDir = Join-Path $env:USERPROFILE ".dotnet"
$dotnetExe = Join-Path $installDir "dotnet.exe"

if (Test-Path $dotnetExe) {
    Write-Host "dotnet already installed at $dotnetExe"
    & $dotnetExe --info
    exit 0
}

$scriptPath = Join-Path $env:TEMP "dotnet-install.ps1"
if (!(Test-Path $scriptPath)) {
    Write-Host "Downloading dotnet-install.ps1..."
    Invoke-WebRequest -Uri "https://dot.net/v1/dotnet-install.ps1" -OutFile $scriptPath
}

Write-Host "Installing .NET 8 SDK to $installDir ..."
& $scriptPath -Channel 8.0 -InstallDir $installDir -NoPath

if (!(Test-Path $dotnetExe)) {
    throw "dotnet.exe was not found after installation at $dotnetExe"
}

Write-Host "Installed."
& $dotnetExe --info
