# Runs inside Windows Sandbox (Start-AvelSandbox.ps1 -Scenario leftovers). Phase 3a acceptance
# test for the leftover scan, which only reads:
#   1. installs the five samples and previews their leftovers (avel-cli scan): each program's
#      own folder or package must be found;
#   2. removes them in one quiet batch (avel-cli uninstall), which scans for leftovers afterwards:
#      Notepad++'s context-menu sparse package, which its uninstaller leaves registered, must be
#      reported as a package leftover;
#   3. checks every reported item against an oracle that knows nothing of Avel's protection
#      rules: no special folder, profile or drive root (nor anything containing one), nothing in
#      the Windows folder, no SYSTEM registry key.
# Writes results\leftovers.json (avel-cli's output) and, last, results\leftovers.txt.

$ErrorActionPreference = 'Stop'
$bin = 'C:\Avel\bin'
$installers = 'C:\Avel\sandbox\installers'
$results = 'C:\Avel\results'
$cli = Join-Path $bin 'avel-cli.exe'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$report = [System.Collections.Generic.List[string]]::new()
$raw = [ordered]@{}
$failures = 0
$progressLog = Join-Path $results 'leftovers.log'
Remove-Item $progressLog -ErrorAction SilentlyContinue
function Log([string]$text) {
    $report.Add($text)
    Write-Host $text
    try { Add-Content $progressLog ('{0:HH:mm:ss} {1}' -f (Get-Date), $text) -Encoding utf8 } catch { }
}
function Fail([string]$text) { $script:failures++; Log "  FAIL: $text" }

. "$PSScriptRoot\samples.ps1"

# Runs avel-cli with a time limit and returns its parsed JSON output (or $null).
function Invoke-Cli([string[]]$arguments, [string]$label, [int]$timeoutSeconds) {
    $out = Join-Path $env:TEMP "avel-$label.json"
    $process = Start-Process $cli ($arguments -join ' ') -NoNewWindow -PassThru -RedirectStandardOutput $out
    if (-not $process.WaitForExit($timeoutSeconds * 1000)) {
        & taskkill.exe /T /F /PID $process.Id | Out-Null
        Fail "$label timed out after $timeoutSeconds s"
        return $null
    }
    $text = Get-Content $out -Raw -Encoding utf8
    $raw[$label] = $text
    try { return $text | ConvertFrom-Json } catch { Fail "$label printed no JSON: $text"; return $null }
}

. "$PSScriptRoot\oracle.ps1"

try {
    Log "Avel Phase 3a leftover scenario, $(Get-Date -Format s)"
    Log "OS: $([Environment]::OSVersion.VersionString), user: $env:USERNAME"
    Log "avel-cli: $((& $cli version) -join ' ')"
    Log "Oracle: $($keep.Count) folders that must never be reported"

    Log 'Installing samples'
    foreach ($sample in $samples) {
        $code = Install-Sample $sample.kind
        if ($code -notin 0, 3010) { Fail "installing $($sample.kind) returned $code" }
    }
    Find-Samples (Get-Inventory)
    $present = @($samples | Where-Object { $_.program })
    foreach ($sample in $samples) {
        if (-not $sample.program) { Fail "$($sample.kind) not found in the inventory ($($sample.matches) matches)" }
        else { Log ('  {0,-5} {1}  [{2}]  {3}' -f $sample.kind, $sample.program.name, $sample.program.id, $sample.program.install_location) }
    }
    $ids = @($present | ForEach-Object { "`"$($_.program.id)`"" })

    # 1. Preview: what would be left if the uninstallers removed nothing.
    Log ''
    Log 'Preview (avel-cli scan, every level)'
    $preview = Invoke-Cli (@('scan') + $ids + @('--json', '--all-levels')) 'preview' 300
    if ($preview) {
        Log ("  {0} items, {1} protected matches dropped, {2} ms" -f $preview.leftovers.Count, $preview.protected_count, $preview.elapsed_ms)
        foreach ($diagnostic in $preview.diagnostics) { Log "  diagnostic: $($diagnostic | ConvertTo-Json -Compress)" }
        foreach ($sample in $present) {
            $own = @($preview.leftovers | Where-Object { $_.programs -contains $sample.program.id })
            Log "  $($sample.kind) ($($own.Count)):"
            $own | ForEach-Object { Log "    $(Describe $_)" }
            # The VC++ runtime and the packaged app have no folder of their own to find.
            $folder = @($own | Where-Object { $_.location.kind -eq 'folder' -and $_.confidence -in 'good', 'very-good' })
            if ($sample.kind -in 'msi', 'nsis', 'inno' -and -not $folder) {
                Fail "preview $($sample.kind): its folder wasn't found at Good or above"
            }
        }
        Check-Safe $preview.leftovers 'preview'
    }

    # 2. Uninstall all five, then the automatic leftover scan.
    Log ''
    Log 'Quiet batch uninstall with the leftover scan'
    $json = Invoke-Cli (@('uninstall') + $ids + @('--quiet', '--no-restore-point', '--yes', '--json')) 'uninstall' 900
    if ($json) {
        $index = 0
        foreach ($sample in $present) {
            Log ('  {0,-5} {1,-45} {2}' -f $sample.kind, $sample.program.name, $json.results[$index].outcome)
            $index++
        }
        $scan = $json.leftover_scan
        if (-not $scan) {
            Fail 'uninstall printed no leftover_scan'
        } else {
            Log ("  leftover scan: {0} items, {1} protected matches dropped, {2} ms" -f $scan.leftovers.Count, $scan.protected_count, $scan.elapsed_ms)
            foreach ($sample in $present) {
                $own = @($scan.leftovers | Where-Object { $_.programs -contains $sample.program.id })
                Log "  $($sample.kind) ($($own.Count)):"
                $own | ForEach-Object { Log "    $(Describe $_)" }
            }
            $npp = $samples | Where-Object { $_.kind -eq 'nsis' }
            if ($npp.program) {
                $package = @($scan.leftovers | Where-Object { $_.location.kind -eq 'package' -and $_.programs -contains $npp.program.id })
                if ($package) { Log "  OK: Notepad++'s sparse package found: $($package[0].location.package_full_name)" }
                else { Fail "Notepad++'s sparse package wasn't reported" }
            }
            Check-Safe $scan.leftovers 'after uninstall'
        }
    }

    # The scan must not have changed anything: the sparse package is still registered.
    $still = @(Get-Inventory | Where-Object { $_.name -match '^Notepad\+\+$' })
    Log "  Notepad++ sparse package still registered after the scan: $([bool]$still)"
} catch {
    Fail "scenario error: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
    Log ''
    Log $(if ($failures -eq 0) { 'RESULT: PASS' } else { "RESULT: FAIL ($failures problem(s))" })
    $raw | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $results 'leftovers.json') -Encoding utf8
    Set-Content -Path (Join-Path $results 'leftovers.txt') -Value $report -Encoding utf8
}
