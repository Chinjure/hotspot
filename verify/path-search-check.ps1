# End-to-end check for path search ("\" triggers path mode):
#   * path queries find files and folders by location
#   * result order is exe/lnk > folder > other files
#   * name search (no separator) keeps its old ordering
#
# Runs the real publish\hotspot-cpp.exe in CLI mode, so it needs no GUI focus.
# Usage: powershell -ExecutionPolicy Bypass -File verify\path-search-check.ps1
param(
    [string]$Exe = "",
    [string]$FixtureRoot = ""
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptRoot
# Layout-agnostic exe lookup: the project is either the repository root
# (<root>\publish\hotspot-cpp.exe) or a cpp\ subdirectory of it.
$exeCandidates = @(
    (Join-Path $projectRoot "publish\hotspot-cpp.exe"),
    (Join-Path (Split-Path -Parent $projectRoot) "publish\hotspot-cpp.exe")
)
if (-not $Exe) { $Exe = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1 }
if (-not $Exe) { $Exe = $exeCandidates[0] }
# The workspace root owns publish\ and is the directory this check searches.
$workspaceRoot = Split-Path -Parent (Split-Path -Parent $Exe)
if (-not $FixtureRoot) { $FixtureRoot = Join-Path $env:TEMP "hs-path-verify" }
if (!(Test-Path $Exe)) { throw "EXE not found: $Exe (build first: build.ps1)" }

$logDir = Join-Path $env:LOCALAPPDATA "hotspot\ntfs"
$liveLog = Join-Path $logDir "live-search.log"
$indexLog = Join-Path $logDir "search.log"
$failures = 0

function Assert([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "ok    $what" -ForegroundColor Green }
    else { Write-Host "FAIL  $what" -ForegroundColor Red; $script:failures++ }
}

function New-Fixtures {
    $dirs = @("$FixtureRoot\alpha\beta\target-dir", "$FixtureRoot\gamma\target-dir", "$FixtureRoot\delta\target-dir")
    foreach ($d in $dirs) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
    Set-Content -Path "$FixtureRoot\alpha\beta\target.exe" -Value "fake exe"
    Set-Content -Path "$FixtureRoot\alpha\beta\target.md" -Value "fake md"
    Set-Content -Path "$FixtureRoot\alpha\beta\target-dir\inner.txt" -Value "inner"
    Set-Content -Path "$FixtureRoot\gamma\target-dir\deep.md" -Value "deep"
    Set-Content -Path "$FixtureRoot\delta\target-dir\target.exe" -Value "fake exe 2"
    $ws = New-Object -ComObject WScript.Shell
    $lnk = $ws.CreateShortcut("$FixtureRoot\alpha\beta\target.lnk")
    $lnk.TargetPath = "C:\Windows\notepad.exe"
    $lnk.Save()
}

function Invoke-Search([string[]]$Mode, [string]$query, [string]$logPath) {
    Remove-Item $logPath -ErrorAction SilentlyContinue
    & $Exe @Mode $query | Out-Null
    if (!(Test-Path $logPath)) { throw "no log written: $logPath (query: $query)" }
    return Get-Content $logPath
}

function Get-Hits([string[]]$lines) {
    $hits = @()
    foreach ($line in $lines) {
        $m = [regex]::Match($line, '^(?<name>.+?) \| (?<path>.+?) \| (?<kind>exe|lnk|dir|file) tier=(?<tier>-?\d+)\s*$')
        if ($m.Success) {
            $hits += [pscustomobject]@{
                Name = $m.Groups['name'].Value
                Path = $m.Groups['path'].Value
                Kind = $m.Groups['kind'].Value
                Tier = [int]$m.Groups['tier'].Value
            }
        }
    }
    return $hits
}

function Get-Kinds([object[]]$hits) { return ($hits | ForEach-Object { $_.Kind }) -join "," }

# The requirement as an invariant: exe/lnk first, then folders, then plain files.
function Assert-GroupOrder([string]$kinds, [string]$what) {
    $group = @{ exe = 0; lnk = 0; dir = 1; file = 2 }
    $previous = -1
    $ok = $true
    foreach ($kind in $kinds.Split(",")) {
        if (-not $kind) { continue }
        $current = $group[$kind]
        if ($current -lt $previous) { $ok = $false }
        $previous = $current
    }
    Assert $ok "$what ($kinds)"
}

New-Fixtures

# ---- 1. Live scan, absolute path query: exe/lnk > folder > file, same tier ----
$query = "$FixtureRoot\alpha\beta\target"
$lines = Invoke-Search @("--live-search") $query $liveLog
$hits = Get-Hits $lines
Assert ($lines -join "`n" | Select-String -SimpleMatch "query=$query" -Quiet) "live path query is passed through: $query"
Assert ($hits.Count -ge 4) "live path query returned >= 4 hits (got $($hits.Count))"
$names = ($hits | ForEach-Object { $_.Name })
Assert ($names -contains "target.exe") "live path query found target.exe"
Assert ($names -contains "target.lnk") "live path query found target.lnk"
Assert ($names -contains "target-dir") "live path query found the folder target-dir"
Assert ($names -contains "target.md") "live path query found target.md"
$order = Get-Kinds $hits
Write-Host "      order: $order"
Assert ($order.StartsWith("exe,lnk,dir,file")) "order is exe, lnk, folder, file (got $order)"

# ---- 2. Live scan, trailing separator: folder itself + contents ----
$query = "$FixtureRoot\alpha\beta\"
$lines = Invoke-Search @("--live-search") $query $liveLog
$hits = Get-Hits $lines
$kinds = Get-Kinds $hits
Write-Host "      order: $kinds"
Assert ($hits.Count -ge 4) "trailing path query returned >= 4 hits (got $($hits.Count))"
$firstFile = $kinds.IndexOf("file")
$firstDir = $kinds.IndexOf("dir")
Assert ($firstDir -ge 0 -and ($firstFile -lt 0 -or $firstDir -lt $firstFile)) "trailing query lists folders before plain files ($kinds)"

# ---- 3. Index scan, trailing separator on a real indexed directory ----
$indexTarget = "$workspaceRoot\"
$lines = Invoke-Search @("--index-search") $indexTarget $indexLog
$hits = Get-Hits $lines
$kinds = Get-Kinds $hits
Write-Host "      order: $kinds"
Assert (($lines -join "`n") -match "\(path mode\)") "index search reports path mode"
Assert ($hits.Count -ge 2) "index path query returned hits (got $($hits.Count))"
Assert-GroupOrder $kinds "index path query keeps exe/lnk, folder, file group order"
$repoPattern = '^' + [regex]::Escape($workspaceRoot) + '(\\|$)'
$outside = @($hits | Where-Object { $_.Path -notmatch $repoPattern })
Assert ($outside.Count -eq 0) "every hit of the repo listing is inside the repo"

# ---- 4. Index scan, absolute folder listing has no cross-directory leaks ----
$query = "C:\Windows\"
$lines = Invoke-Search @("--index-search") $query $indexLog
$hits = Get-Hits $lines
Assert ($hits.Count -ge 5) "C:\Windows\ returned hits (got $($hits.Count))"
$leaks = @($hits | Where-Object { $_.Path -notmatch '^C:\\Windows(\\|$)' })
Assert ($leaks.Count -eq 0) "every C:\Windows\ hit is inside C:\Windows (leaks: $(($leaks | ForEach-Object { $_.Path }) -join '; '))"
$kinds = Get-Kinds $hits
Write-Host "      order: $kinds"
Assert ($kinds -match "^(exe|lnk)") "C:\Windows\ lists launchable hits first ($kinds)"
Assert-GroupOrder $kinds "C:\Windows\ keeps exe/lnk, folder, file group order"

# ---- 5. Regression: plain name search is unchanged ----
$lines = Invoke-Search @("--index-search") "hotspot" $indexLog
$hits = Get-Hits $lines
Assert (($lines -join "`n") -match "\(name mode\)") "plain name search stays in name mode"
Assert ($hits.Count -ge 1) "plain name search still returns hits (got $($hits.Count))"

Write-Host ""
if ($failures -eq 0) {
    Write-Host "PASS  path-search-check" -ForegroundColor Green
    Write-Host "      fixtures kept at $FixtureRoot"
    exit 0
}
Write-Host "FAIL  path-search-check ($failures failure(s))" -ForegroundColor Red
exit 1
