param(
    [string]$Configuration = "Release",
    [string]$Runtime = "win-x64",
    [string]$Output = "publish"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root "src\PowerToysRunStandalone\PowerToysRunStandalone.csproj"
$outDir = Join-Path $root $Output

Write-Host "==> dotnet publish ($Configuration / $Runtime, self-contained single-file)" -ForegroundColor Cyan
dotnet publish $project `
    -c $Configuration `
    -r $Runtime `
    --self-contained true `
    -p:PublishSingleFile=true `
    -p:IncludeNativeLibrariesForSelfExtract=true `
    -p:EnableCompressionInSingleFile=true `
    -p:DebugType=embedded `
    -o $outDir

if ($LASTEXITCODE -ne 0) {
    throw "dotnet publish failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "==> Published to $outDir" -ForegroundColor Green
Get-ChildItem $outDir | Select-Object Name, Length | Format-Table -AutoSize
