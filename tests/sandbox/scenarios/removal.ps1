# Runs inside Windows Sandbox (Start-AvelSandbox.ps1 -Scenario removal). Phase 3b acceptance test
# for removing leftovers and undoing it:
#   1. installs three samples, "Contoso Fixture" (fixture.ps1: one leftover of every kind), a
#      program of the same publisher that stays, and canaries named after the fixture in places
#      Avel must never touch; dumps their state with avel_state_dump;
#   2. uninstalls the samples and the fixture with --remove-leftovers --allow-irreversible: every
#      fixture leftover and Notepad++'s sparse package must be removed, the firewall rule only
#      reported, the canaries and the other program left byte for byte as they were, and nothing
#      reported that the oracle protects;
#   3. undoes one item (--item), then the rest: the fixture's leftovers must be back exactly;
#   4. reinstalls the fixture and removes it again, killing avel-cli part way
#      (AVEL_TEST_FAULT_AFTER); the next run must settle the interrupted set, and undo must again
#      give back exactly what was there.
# Writes results\removal.json (avel-cli's output), results\removal.log and, last,
# results\removal.txt.

$ErrorActionPreference = 'Stop'
$bin = 'C:\Avel\bin'
$installers = 'C:\Avel\sandbox\installers'
$results = 'C:\Avel\results'
$cli = Join-Path $bin 'avel-cli.exe'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$report = [System.Collections.Generic.List[string]]::new()
$raw = [ordered]@{}
$failures = 0
$progressLog = Join-Path $results 'removal.log'
Remove-Item $progressLog -ErrorAction SilentlyContinue
function Log([string]$text) {
    $report.Add($text)
    Write-Host $text
    try { Add-Content $progressLog ('{0:HH:mm:ss} {1}' -f (Get-Date), $text) -Encoding utf8 } catch { }
}
function Fail([string]$text) { $script:failures++; Log "  FAIL: $text" }

. "$PSScriptRoot\samples.ps1"
. "$PSScriptRoot\fixture.ps1"
. "$PSScriptRoot\oracle.ps1"
$samples = @($samples | Where-Object { $_.kind -in 'msi', 'nsis', 'inno' })  # Burn and MSIX add nothing here

# Runs avel-cli with a time limit; returns @{ exit; text; json }.
function Invoke-Cli([string[]]$arguments, [string]$label, [int]$timeoutSeconds = 300) {
    $out = Join-Path $env:TEMP "avel-$label.out"
    $err = Join-Path $env:TEMP "avel-$label.err"
    $process = Start-Process $cli ($arguments -join ' ') -NoNewWindow -PassThru -RedirectStandardOutput $out `
        -RedirectStandardError $err
    $null = $process.Handle  # Without this, ExitCode stays empty once the process has exited
    if (-not $process.WaitForExit($timeoutSeconds * 1000)) {
        & taskkill.exe /T /F /PID $process.Id | Out-Null
        Fail "$label timed out after $timeoutSeconds s"
        return @{ exit = -1; text = ''; json = $null }
    }
    $text = Get-Content $out -Raw -Encoding utf8
    $errors = Get-Content $err -Raw -Encoding utf8
    $raw[$label] = @{ exit = $process.ExitCode; stdout = $text; stderr = $errors }
    if ($errors) { $errors.Trim() -split "`n" | ForEach-Object { Log "  [stderr] $_" } }
    $json = $null
    if ($text) { try { $json = $text | ConvertFrom-Json } catch { } }
    return @{ exit = $process.ExitCode; text = $text; json = $json }
}

function Where-Text($location) {
    switch ($location.kind) {
        { $_ -in 'folder', 'file' } { return $location.path }
        'registry-key' { return $location.key }
        'registry-value' { return "$($location.key) : $($location.value)" }
        'service' { return $location.service }
        'task' { return $location.task }
        'package' { return $location.package_full_name }
        default { return $location.rule_id }
    }
}

# Compares two state dumps section by section; logs the first differences.
function Compare-State($expected, $actual, [string]$label) {
    $same = $true
    foreach ($spec in $expected.Keys) {
        if ($expected[$spec] -ne $actual[$spec]) {
            $same = $false
            Fail "$($label): $spec differs"
            $left = $expected[$spec] -split "`n"
            $right = $actual[$spec] -split "`n"
            $shown = 0
            for ($i = 0; $i -lt [Math]::Max($left.Count, $right.Count) -and $shown -lt 3; $i++) {
                if ($left[$i] -ne $right[$i]) {
                    Log "      expected: $($left[$i])"
                    Log "      actual:   $($right[$i])"
                    $shown++
                }
            }
        }
    }
    if ($same) { Log "  OK: $label" }
}

$expectedRemovals = @(
    @{ kind = 'folder'; match = '^C:\\ProgramData\\Contoso Fixture$' },
    @{ kind = 'folder'; match = '\\AppData\\Roaming\\Contoso Fixture$' },
    @{ kind = 'folder'; match = 'Start Menu\\Programs\\Contoso Fixture$' },
    @{ kind = 'registry-key'; match = '^HKLM\\SOFTWARE\\Contoso Fixture$' },
    @{ kind = 'registry-key'; match = '^HKU\\S-1-5-21-[0-9-]+\\Software\\Contoso Fixture$' },
    @{ kind = 'registry-value'; match = '\\Run : ContosoFixtureUpdater$' },
    @{ kind = 'service'; match = '^ContosoFixtureService$' },
    @{ kind = 'task'; match = '^\\Contoso\\Contoso Fixture Updater$' },
    @{ kind = 'registry-key'; match = '\\CLSID\\\{C0475F1C-0000-4A11-9C2B-5A4E2B1F0001\}$' },
    @{ kind = 'registry-key'; match = 'ContextMenuHandlers\\ContosoFixture$' },
    @{ kind = 'registry-value'; match = 'Approved : \{C0475F1C-0000-4A11-9C2B-5A4E2B1F0001\}$' }
)

function Check-Removal($json, [string]$label, [switch]$WithSamples) {
    $removal = $json.leftover_removal
    if (-not $removal) { Fail "$($label): no leftover_removal in the output"; return $null }
    Log "  change set $($removal.set_id), $($removal.items.Count) items, $($removal.quarantined_bytes) bytes quarantined"
    foreach ($item in $removal.items) {
        Log ('    {0,-4} {1,-26} {2,-15} {3}' -f $item.index, "$($item.state) ($($item.reason))", $item.location.kind,
             (Where-Text $item.location))
    }
    $expected = $expectedRemovals
    if ($WithSamples) { $expected += @{ kind = 'package'; match = '^NotepadPlusPlus_' } }
    foreach ($want in $expected) {
        $found = @($removal.items | Where-Object { $_.location.kind -eq $want.kind -and (Where-Text $_.location) -match $want.match })
        if (-not $found) { Fail "$($label): not planned: $($want.kind) $($want.match)" }
        elseif ($found[0].state -ne 'done') { Fail "$($label): $($want.kind) $($want.match) is $($found[0].state) ($($found[0].reason))" }
    }
    # Never planned: canaries, the other program, the firewall rule.
    foreach ($item in $removal.items) {
        $where = Where-Text $item.location
        if ($where -match 'Microsoft\\Contoso Fixture|Windows\\Contoso Fixture|Contoso Keeper' -or
            $item.location.kind -eq 'firewall-rule') {
            Fail "$($label): planned what must stay: $where"
        }
    }
    $strong = @($json.leftover_scan.leftovers | Where-Object { $_.confidence -in 'good', 'very-good' })
    foreach ($item in $strong) {
        if ((Where-Text $item.location) -match 'Microsoft\\Contoso Fixture|Windows\\Contoso Fixture|Contoso Keeper') {
            Fail "$($label): reported a canary at $($item.confidence): $(Where-Text $item.location)"
        }
    }
    if (-not ($json.leftover_scan.leftovers | Where-Object { $_.location.kind -eq 'firewall-rule' })) {
        Fail "$($label): the fixture's firewall rule wasn't reported"
    }
    Check-Safe $json.leftover_scan.leftovers "$label scan"
    Check-Safe $removal.items "$label removal"
    return $removal
}

$specs = @(Get-FixtureLeftoverSpecs) + @(Get-UntouchableSpecs)

try {
    Log "Avel Phase 3b removal scenario, $(Get-Date -Format s)"
    Log "OS: $([Environment]::OSVersion.VersionString), user: $env:USERNAME"
    Log "avel-cli: $((& $cli version) -join ' ')"

    # 1. Install.
    Log 'Installing samples, the fixture and the canaries'
    foreach ($sample in $samples) {
        $code = Install-Sample $sample.kind
        if ($code -notin 0, 3010) { Fail "installing $($sample.kind) returned $code" }
    }
    Install-ContosoFixture
    Install-Untouchables
    $programs = Get-Inventory
    Find-Samples $programs
    $fixture = @($programs | Where-Object { $_.name -eq 'Contoso Fixture' })
    if ($fixture.Count -ne 1) { throw "Contoso Fixture not found in the inventory ($($fixture.Count))" }
    $ids = @($samples | Where-Object { $_.program } | ForEach-Object { "`"$($_.program.id)`"" }) + "`"$($fixture[0].id)`""
    Log "  programs: $($ids -join ', ')"
    $before = Split-StateDump (Get-StateDump $specs)
    foreach ($spec in Get-FixtureLeftoverSpecs) {
        if ($before[$spec] -match '^<missing>') { Fail "fixture not installed: $spec" }
    }

    # 2. Uninstall and remove the leftovers.
    Log ''
    Log 'Round 1: quiet uninstall with --remove-leftovers --allow-irreversible'
    $run = Invoke-Cli (@('uninstall') + $ids + @('--quiet', '--no-restore-point', '--remove-leftovers',
                                                 '--allow-irreversible', '--yes', '--json')) 'remove' 900
    if (-not $run.json) { throw "uninstall printed no JSON (exit $($run.exit))" }
    foreach ($result in $run.json.results) {
        if ($result.outcome -notin 'removed', 'reboot-required') { Fail "uninstall: $($result.name) $($result.outcome)" }
    }
    $removal = Check-Removal $run.json 'round 1' -WithSamples
    $after = Split-StateDump (Get-StateDump $specs)
    foreach ($spec in Get-UntouchableSpecs) {
        if ($after[$spec] -ne $before[$spec]) { Fail "round 1: changed what must stay: $spec" }
    }
    if (@(Get-Inventory | Where-Object { $_.name -match '^Notepad\+\+$' })) { Fail "Notepad++'s sparse package is still registered" }
    else { Log "  OK: Notepad++'s sparse package is gone" }
    if (Get-NetFirewallRule -DisplayName 'Contoso Fixture' -ErrorAction SilentlyContinue) { Log '  OK: the firewall rule stays (report only)' }
    else { Fail 'the firewall rule was removed' }

    # 3. Undo one item, then the rest.
    if ($removal) {
        $service = @($removal.items | Where-Object { $_.location.kind -eq 'service' })[0]
        Log ''
        Log "Undo item $($service.index) (the service)"
        $one = Invoke-Cli @('undo', $removal.set_id, '--item', $service.index, '--json') 'undo-item'
        if ($one.json.state -ne 'partly-restored') { Fail "undo --item: state $($one.json.state)" }
        $partial = Split-StateDump (Get-StateDump @("service:$fixtureService", "task:$fixtureTask"))
        if ($partial["service:$fixtureService"] -ne $before["service:$fixtureService"]) { Fail 'undo --item: the service differs' }
        if ($partial["task:$fixtureTask"] -notmatch '^<missing>') { Fail 'undo --item: restored more than the item' }

        Log 'Undo the rest'
        $all = Invoke-Cli @('undo', $removal.set_id, '--json') 'undo-all'
        Log "  state: $($all.json.state), exit $($all.exit)"
        if ($all.json.state -ne 'restored') { Fail "undo: state $($all.json.state)" }
        Compare-State $before (Split-StateDump (Get-StateDump $specs)) 'undo gave back exactly what was there'
    }

    # 4. Again, killed part way.
    Log ''
    Log 'Round 2: reinstall the fixture, remove it again and kill avel-cli after 7 journal records'
    Install-ContosoFixture
    $fixture = @(Get-Inventory | Where-Object { $_.name -eq 'Contoso Fixture' })
    $before = Split-StateDump (Get-StateDump $specs)
    $env:AVEL_TEST_FAULT_AFTER = '7'
    $crash = Invoke-Cli @('uninstall', "`"$($fixture[0].id)`"", '--quiet', '--no-restore-point', '--remove-leftovers',
                          '--yes', '--json') 'crash' 600
    Remove-Item Env:\AVEL_TEST_FAULT_AFTER
    if ($crash.exit -ne 99) { Fail "the killed run exited with $($crash.exit), not 99" } else { Log '  avel-cli stopped as planned (exit 99)' }
    $list = Invoke-Cli @('undo', '--list', '--json') 'list'
    $set = @($list.json | Sort-Object created -Descending)[0]
    Log "  newest set $($set.id): $($set.state), $($set.done) removed, $($set.skipped) skipped"
    if ($set.state -ne 'complete') { Fail "the interrupted set is $($set.state) after recovery" }
    $show = Invoke-Cli @('undo', '--show', $set.id, '--json') 'show'
    $interrupted = @($show.json.items | Where-Object { $_.reason -eq 'interrupted' })
    Log "  items settled as interrupted: $($interrupted.Count)"
    if ($interrupted.Count -eq 0) { Fail 'recovery marked nothing as interrupted' }
    $undo = Invoke-Cli @('undo', $set.id, '--json') 'undo-crash'
    Log "  undo: $($undo.json.state)"
    Compare-State $before (Split-StateDump (Get-StateDump $specs)) 'undo after a crash gave back exactly what was there'
} catch {
    Fail "scenario error: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
    Log ''
    Log $(if ($failures -eq 0) { 'RESULT: PASS' } else { "RESULT: FAIL ($failures problem(s))" })
    $raw | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $results 'removal.json') -Encoding utf8
    Set-Content -Path (Join-Path $results 'removal.txt') -Value $report -Encoding utf8
}
