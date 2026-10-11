# End-to-end check for path search ("\" triggers path mode):
#   * path queries find files and folders by location
#   * the better path match always comes first, and inside one match tier the
#     order is exe/lnk > folder > other files
#   * name search (no separator) keeps working
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
    # The log is UTF-8 (and carries Chinese subtitles), so it must not be read
    # through the ANSI default: a mangled full-width space swallows the " | "
    # separator and every line stops parsing.
    return Get-Content $logPath -Encoding UTF8
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

# The requirement as an invariant: the better match always comes first, and only
# inside one match class does the kind matter -- exe/lnk, then folders, then
# plain files in path mode (file_rank::betterPathMatch), but exe/lnk, then plain
# files, then folders in name mode (file_rank::betterNameMatch). Exact name (0)
# and exact stem (1) are one class -- see file_rank::qualityClass.
function Get-Class([int]$tier) { if ($tier -le 1) { return 0 } return $tier }

$KindRank = @{ exe = 0; lnk = 1; file = 2; dir = 3 }
$LaunchRank = @{ exe = 0; lnk = 0; file = 1; dir = 1 }
$PathKindGroup = @{ exe = 0; lnk = 0; dir = 1; file = 2 }

# The comparator's own key, in the comparator's own order:
#   path mode: class, kind group (exe/lnk, folder, file), tier, kind
#   name mode: class, launchable, tier, kind
function Get-SortKey([object]$hit, [string]$mode) {
    $class = Get-Class $hit.Tier
    if ($mode -eq "name") {
        return ,@($class, $LaunchRank[$hit.Kind], $hit.Tier, $KindRank[$hit.Kind])
    }
    return ,@($class, $PathKindGroup[$hit.Kind], $hit.Tier, $KindRank[$hit.Kind])
}

function Assert-Order([object[]]$hits, [string]$what, [string]$mode = "path") {
    $ok = $true
    $detail = ""
    for ($i = 1; $i -lt $hits.Count; $i++) {
        $previous = Get-SortKey $hits[$i - 1] $mode
        $current = Get-SortKey $hits[$i] $mode
        $worse = $false
        for ($k = 0; $k -lt $previous.Count; $k++) {
            if ($previous[$k] -ne $current[$k]) { $worse = $previous[$k] -gt $current[$k]; break }
        }
        if ($worse) {
            $ok = $false
            $detail = " ($($hits[$i - 1].Kind) tier=$($hits[$i - 1].Tier) after $($hits[$i].Kind) tier=$($hits[$i].Tier))"
            break
        }
    }
    Assert $ok "$what$detail"
}

New-Fixtures

# ---- 1. Live scan, absolute path query: better tier first, kind inside a tier ----
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
# target.exe/.lnk/.md match exactly (stem), target-dir only by prefix, so the
# three exact hits come first -- exe, lnk, then the plain file -- and the folder
# follows on its worse tier.
Assert ($order.StartsWith("exe,lnk,file,dir")) "exact-name hits before the prefix folder (got $order)"
Assert-Order $hits "live path query keeps match quality first"

# ---- 2. Live scan, trailing separator: folder itself + contents ----
$query = "$FixtureRoot\alpha\beta\"
$lines = Invoke-Search @("--live-search") $query $liveLog
$hits = Get-Hits $lines
$kinds = Get-Kinds $hits
Write-Host "      order: $kinds"
Assert ($hits.Count -ge 4) "trailing path query returned >= 4 hits (got $($hits.Count))"
# Everything found here is a descendant of the named folder (one tier), so the
# kind order inside that tier decides: exe/lnk, then the folder, then plain files.
Assert ($kinds.StartsWith("exe,lnk,dir,file")) "trailing query keeps kind order inside one tier ($kinds)"
Assert-Order $hits "trailing query keeps match quality first"

# ---- 3. Index scan, trailing separator on a real indexed directory ----
$indexTarget = "$workspaceRoot\"
$lines = Invoke-Search @("--index-search") $indexTarget $indexLog
$hits = Get-Hits $lines
$kinds = Get-Kinds $hits
Write-Host "      order: $kinds"
Assert (($lines -join "`n") -match "\(path mode\)") "index search reports path mode"
Assert ($hits.Count -ge 2) "index path query returned hits (got $($hits.Count))"
Assert-Order $hits "index path query keeps match quality first inside one tier"
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
Assert ($hits[0].Name -eq "Windows" -and $hits[0].Kind -eq "dir") "C:\Windows\ lists the named folder first ($kinds)"
Assert-Order $hits "C:\Windows\ keeps match quality first inside one tier"

# ---- 5. Plain name search stays in name mode and still returns hits ----
$lines = Invoke-Search @("--index-search") "hotspot" $indexLog
$hits = Get-Hits $lines
Assert (($lines -join "`n") -match "\(name mode\)") "plain name search stays in name mode"
Assert ($hits.Count -ge 1) "plain name search still returns hits (got $($hits.Count))"
Assert-Order $hits "plain name search keeps match quality first inside one tier" "name"

Write-Host ""
if ($failures -eq 0) {
    Write-Host "PASS  path-search-check" -ForegroundColor Green
    Write-Host "      fixtures kept at $FixtureRoot"
    exit 0
}
Write-Host "FAIL  path-search-check ($failures failure(s))" -ForegroundColor Red
exit 1
