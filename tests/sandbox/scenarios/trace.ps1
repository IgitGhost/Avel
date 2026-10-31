# Runs inside Windows Sandbox (Start-AvelSandbox.ps1 -Scenario trace). Phase 5a and 5b acceptance
# test for Avel Trace, the install tracker, and removing programs by their traces (docs/trace.md),
# checked against an independent oracle:
# avel_capture (tests/helpers/capture.cpp) captures every file on C: and the registry roots the
# tracker watches right before and right after each traced installation, and the diff of the two
# captures is what really changed. In rounds 0, 0b and 1 both captures are taken while the trace is
# open, so the trace must account for every line of the diff, unless its item demonstrably
# changed again inside the trace but outside the captures (Get-AgainReason); around `trace run`
# (rounds 2-4) they can't be, and a line is put down to the moments between a capture and the
# trace's own window (from its trace.json) only with evidence: its creation, last write or key's
# last write time lies there (for a gone key or value, that of the nearest key above it still
# there), or a watch of the volume's names saw it go away (or a created one appear) then. A created
# item the volume's journal has no record of between the captures is logged as KNOWN in any round.
#   U. the tracker's unit tests that need elevation (USN journal, installer watch, registry
#      snapshot) or the Sandbox's user profile;
#   0. idle: a minute with nothing but a stock app opened and closed (opened once before the trace
#      too, as a profile in use has long done): no change may be reported
#      that isn't noise, and the trace accounts for every change the oracle saw. What it doesn't
#      is logged as KNOWN in the later rounds;
#   0b. a container's base layer: one of Windows' files deleted and another renamed between
#      `trace start` and `trace stop`: exactly that deletion and rename, no tombstone as created;
#   1. "Contoso Trace Fixture" (trace_fixture.ps1) installed between `trace start` and `trace stop`:
#      completeness (every line of the oracle's diff is accounted for by a reported change of the
#      same kind), precision (every reported change that isn't noise is in the oracle's diff), and
#      each of the fixture's items and known values;
#   1 (5b). the trace linked to the fixture by itself and its items tied to it; the user adds their
#      own PATH entries; `force` reverts the fixture by its trace (Revert: the oracle's diff between
#      the capture before the install and the one after the removal holds exactly the KNOWN set,
#      Test-Revert; exact PATH, SharedDLLs and CONTOSO_HOME), and `undo` gives back exactly what was
#      there before the removal (Undo: state dumps, and the oracle's diff, Test-Undo, with a manual
#      trace around the removal and undo telling Windows' own activity apart). Then `trace export`,
#      `trace delete`, `trace import` and `trace link`; the fixture's own uninstaller removes its
#      folder and key, and `force --trace <the imported trace>` reverts the rest, judged the same way;
#   2-4. 7-Zip (MSI), Notepad++ (NSIS) and Inno Setup through `trace run`: it must end by itself
#      (installer-done) and be complete against the oracle; precision exceptions are logged;
#   2-4 (5b). `scan` previews each with its trace; `uninstall --quiet --remove-leftovers` runs its
#      uninstaller and removes what the trace saw the installer create, and Test-Revert judges what
#      is left against the capture before the install;
#   5. the fixture through `trace run`, starting its app at the end: done within 45 s, the app
#      listed as left running and still running;
#   6. a `trace run` killed after its snapshot is finished by `trace stop`; a USN journal deleted
#      and recreated during a trace makes it incomplete (C: "replaced"), the registry still reported.
#   GUI. Phase 5c, last: avel.exe through UI Automation, judged the same way (trace_gui_round.ps1;
#      the `trace_gui` scenario runs it alone).
# Rounds 1-5 keep what they traced in $traced (trace id, oracle captures, results), for the 5b
# rounds to compare against. The oracle and the checks are in trace_oracle.ps1, shared with the
# GUI round.
# Writes results\trace.json (avel-cli's output), results\trace.log, results\trace-oracle-<round>.txt
# (each round's oracle diff, every line with what accounted for it; for the 5b checks, what judged
# it) and, last, results\trace.txt.

$ErrorActionPreference = 'Stop'
$bin = 'C:\Avel\bin'
$installers = 'C:\Avel\sandbox\installers'
$results = 'C:\Avel\results'
$cli = Join-Path $bin 'avel-cli.exe'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$scenario = 'trace'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\oracle.ps1"
. "$PSScriptRoot\trace_fixture.ps1"
# The oracle and what judges a trace against it, shared with trace_gui.ps1.
. "$PSScriptRoot\trace_oracle.ps1"
. "$PSScriptRoot\gui.ps1"
# The GUI round (5c), run last; trace_gui.ps1 runs it alone.
. "$PSScriptRoot\trace_gui_round.ps1"

# The same silent switches as Install-Sample in samples.ps1; the MSI is given to `trace run` as is.
# `entry`: the name of the sample's own Uninstall entry, which its trace's primary link must be.
$traceSamples = @(
    @{ label = 'r2-7zip'; name = '7-Zip (MSI)'; entry = '^7-Zip '; command = @("$installers\7z-x64.msi", '/qn', '/norestart') },
    @{ label = 'r3-notepadpp'; name = 'Notepad++ (NSIS)'; entry = '^Notepad\+\+ '
       command = @("$installers\npp-installer-x64.exe", '/S') },
    @{ label = 'r4-innosetup'; name = 'Inno Setup'; entry = '^Inno Setup '
       command = @("$installers\innosetup-x64.exe", '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/ALLUSERS') }
)

# 5b of round 1: the fixture, still installed, reverted by its trace with `force` and undone; then
# its trace exported, deleted, imported and linked by hand, its own uninstaller run (its folder and
# key go), and the rest reverted with `force --trace <the imported trace>` (the program is gone, so
# the target is trace:<id>) and undone. Leaves the fixture removed and the user's entries gone.
function Invoke-FixtureRevertRound($record) {
    Log ''
    Log 'Round 1 (5b): the fixture reverted by its trace with force and undone; its trace exported, deleted, imported and linked by hand; its own uninstaller run, the rest reverted with force --trace and undone'
    if (-not $record -or -not $record.id) { Fail 'round 1 (5b): there is no trace of the fixture to revert'; return }
    if (-not (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso Trace')) { Fail 'round 1 (5b): the fixture is not installed'; return }
    try {
        $id = $record.id
        $fixture = New-FixtureContext (Get-TraceFixtureRuleId)
        $program = Test-FixtureLinks $id $fixture
        if (-not $program) { $program = 'Contoso Trace' }  # Its exact name
        $file = Join-Path $captures 'r1-fixture.trace.json'
        Test-TraceExport $id $file
        foreach ($spot in $userPathEntries.Keys) { Set-UserPathEntry $spot $userPathEntries[$spot] }
        Log "  the user added their own Path entries: $(@($userPathEntries.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', ')"
        $shell = New-ShellCoverage $record.all $fixture $record.lines

        Log ''
        Log "  A. force $program --yes --end-processes, then undo"
        $revert = Invoke-FixtureRevert 'r1-force' @('force', $program, '--yes', '--end-processes') $fixture
        Test-FixtureRevert $revert $record $fixture $id $program 'r1-force' $shell

        Log ''
        Log '  B. trace export, delete, import and link; the fixture''s own uninstaller; force --trace <the imported trace>, then undo'
        $imported = Import-FixtureTrace $id $file $program
        if (-not $imported) { return }
        $uninstaller = Start-Process "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -Wait -PassThru -WindowStyle Hidden `
            -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$traceRoot\uninstall.ps1`"")
        $left = @($traceRoot, 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso Trace' | Where-Object { Test-Path -LiteralPath $_ })
        if ($uninstaller.ExitCode -ne 0 -or $left) { Fail "round 1 (5b): the fixture's own uninstaller exited with $($uninstaller.ExitCode), left: $($left -join ', ')" }
        else { Log '  the fixture''s own uninstaller removed its folder and its Uninstall key' }
        Start-Sleep -Seconds $settleSeconds  # Windows' reactions to it, before the watch
        $revert = Invoke-FixtureRevert 'r1-force-trace' @('force', '--trace', $imported, '--yes', '--end-processes') $fixture
        Test-FixtureRevert $revert $record $fixture $imported "trace:$imported" 'r1-force-trace' $shell -ProgramGone
    } finally {
        Remove-TraceFixture
        foreach ($spot in $userPathEntries.Keys) { Set-UserPathEntry $spot $userPathEntries[$spot] -Remove }
    }
}

# 5b of rounds 2-4, mode 1: the sample uninstalled with its trace. `scan` previews it; `uninstall
# --quiet --remove-leftovers` runs its own uninstaller, then what the trace saw its installer create
# joins the leftovers and what is Good or better goes. results[].trace names the trace, the trace
# use's counts add up, every planned item goes, Test-Revert judges what is left against the capture
# before the install, and retention starts the uninstalled program's trace's 30 days.
function Invoke-UninstallByTrace($sample, [int]$number) {
    Log ''
    Log "Round $number (5b): $($sample.name) uninstalled with its trace (uninstall --quiet --remove-leftovers)"
    $record = $traced[$sample.label]
    if (-not $record -or -not $record.id) { Fail "round $number (5b): there is no trace of $($sample.name)"; return }
    $label = "$($sample.label)-uninstall"
    $links = @($record.result.trace.links | Where-Object { $_ })
    foreach ($link in $links) { Log "  link: $($link.name) ($($link.program_id)) [$($link.matched_by)]$(if ($link.primary) { ', primary' })" }
    $link = @($links | Where-Object { $_.primary })[0]
    if (-not $link) { Fail "$($label): the trace isn't linked to any program, so nothing is uninstalled by it"; return }
    # Uninstalling another entry the installer's trace saw registered (run 1: Windows' compatibility
    # fix database, which Windows installed meanwhile) would remove the wrong program.
    if ($link.name -notmatch $sample.entry) { Fail "$($label): the trace's primary link is $($link.name), not $($sample.name)'s own entry; not uninstalled"; return }
    $program = $link.program_id

    $preview = Invoke-Avel @('scan', $program, '--all-levels', '--json') "$label-scan" 300
    if (-not $preview.json) {
        Fail "$($label): scan printed no JSON (exit $($preview.exit))"
    } else {
        $programs = @($preview.json.programs | Where-Object { $_ })
        if ($programs.Count -ne 1 -or $programs[0].trace -ne $record.id) { Fail "$($label): scan's programs are $(@($programs | ForEach-Object { "$($_.id) by trace $($_.trace)" }) -join '; '), not by trace $($record.id)" }
        else { Log "  OK: scan previews $program with trace $($record.id)" }
        $use = Test-TraceUse $preview.json "$label scan" $record.id $program $record
        if ($use -and [int]$use.tied -lt 1) { Fail "$($label): scan: nothing the trace saw is tied to the program while it is installed" }
    }

    $run = Invoke-Avel @('uninstall', $program, '--quiet', '--yes', '--remove-leftovers', '--no-restore-point', '--json') $label 900
    Start-Sleep -Seconds $settleSeconds
    $after = New-OracleCapture "$label-after"
    if (-not $run.json) { Fail "$($label): uninstall printed no JSON (exit $($run.exit))"; return }
    $json = $run.json
    # Not $results: that is the results folder, which the functions called from here use too.
    $outcomes = @($json.results | Where-Object { $_ })
    foreach ($outcome in $outcomes) { Log "  $($outcome.name): $($outcome.outcome), exit code $($outcome.exit_code), $([int]$outcome.elapsed_ms) ms, trace $($outcome.trace)" }
    if ($outcomes.Count -ne 1 -or $outcomes[0].outcome -notin 'removed', 'reboot-required') { Fail "$($label): the uninstall didn't remove $($sample.name)" }
    if ($outcomes.Count -ge 1 -and $outcomes[0].trace -ne $record.id) { Fail "$($label): results[].trace is '$($outcomes[0].trace)', not $($record.id)" }
    elseif ($outcomes.Count -ge 1) { Log "  OK: results[].trace names the round's trace" }
    $use = Test-TraceUse $json.leftover_scan $label $record.id $program $record
    if ($use -and [int]$use.gone -lt 1) { Fail "$($label): nothing the trace saw is gone after the uninstaller ran" }
    Write-TracedItems $json.leftover_scan
    Test-RemovalDone $json.leftover_removal $label -Optional
    Check-Safe @($json.leftover_scan.leftovers) "$label scan"
    if ($json.leftover_removal) { Check-Safe @($json.leftover_removal.items) "$label removal" }
    Test-Revert $record $after $json "$label-revert"
    $show = Invoke-Trace @('show', $record.id, '--json') "$label-show"
    if (-not $show.json) { Fail "$($label): trace show printed no JSON (exit $($show.exit))" }
    elseif (-not $show.json.trace.unresolved_since) { Fail "$($label): the uninstalled program's trace has no unresolved_since: retention didn't start its 30 days" }
    else { Log "  OK: the trace is kept 30 days from $($show.json.trace.unresolved_since)" }
}

# ---------------------------------------------------------------------------------------------
# Rounds

function Invoke-Round([string]$name, [scriptblock]$body) {
    try {
        & $body
    } catch {
        Fail "$($name): $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
        try { Close-OpenTrace } catch { Log "  cleanup failed: $($_.Exception.Message)" }
    }
}

# One traced installation between two oracle captures. -Launch: `trace run` with this command line,
# between the captures; what the oracle saw change between a capture and the trace is logged as
# KNOWN where its change time shows it (`gaps` in the record). Otherwise -Install runs between
# `trace start` and `trace stop` (its output is kept as `install`), and the captures are taken
# while the trace is open, the second once Windows has had time to react. Returns the round's
# record, also kept in $traced, or $null.
function Invoke-TracedRound([string]$label, [string]$description, [string[]]$Launch, [scriptblock]$Install) {
    Close-OpenTrace
    Start-Sleep -Seconds $settleSeconds  # Windows' reactions to the round before
    $installed = $null
    $gaps = @()
    $journalBefore = $null
    $journalAfter = $null
    $inside = $null
    if ($Launch) {
        $watch = [AvelNameWatch]::new('C:\')
        try {
            $journalBefore = [AvelJournal]::Query('C:')
            $preStart = Get-FileTimeNow
            $pre = New-OracleCapture "$label-pre"
            $clock = [Diagnostics.Stopwatch]::StartNew()
            $runStart = Get-FileTimeNow
            $result = Invoke-Trace (@('run', '--description', $description, '--json', '--') + $Launch) "$label-run" 1200
            $runEnd = Get-FileTimeNow
            $seconds = $clock.Elapsed.TotalSeconds
            $post = New-OracleCapture "$label-post"
            $postEnd = Get-FileTimeNow
            $journalAfter = [AvelJournal]::Query('C:')
        } finally {
            $watch.Dispose()
        }
        if ($watch.Overflowed) { Log '  the name watch overflowed: items around the trace may lack evidence' }
        $window = Get-TraceWindow "$($result.json.trace.id)" $runStart $runEnd
        $gaps = @(@{ from = $preStart; to = $window.start; keysTo = $window.keys; what = 'before the trace started'; watch = $watch },
                  @{ from = $window.stop; to = $postEnd; what = 'after the trace stopped'; watch = $watch })
        Log ("  outside the trace (UTC): {0} to {1} (keys and values: to {2}), {3} to {4}" -f (Format-FileTime $preStart),
             (Format-FileTime $window.start), (Format-FileTime $window.keys), (Format-FileTime $window.stop), (Format-FileTime $postEnd))
        if ($window.usns -and $journalBefore -and $journalAfter) {
            Log "  the trace's journal: $($window.usns); before the capture before at $($journalBefore[2]), after the capture after at $($journalAfter[2])"
        }
    } else {
        $start = Invoke-Trace @('start', '--description', $description, '--json') "$label-start"
        if ($start.exit -ne 0 -or $start.json.trace.state -ne 'open') {
            Fail "$($label): trace start: exit $($start.exit), state $($start.json.trace.state)"
            return $null
        }
        Start-Hold
        try {
            $journalBefore = [AvelJournal]::Query('C:')
            $pre = New-OracleCapture "$label-pre"
            $clock = [Diagnostics.Stopwatch]::StartNew()
            $installed = & $Install
            $seconds = $clock.Elapsed.TotalSeconds
            Start-Sleep -Seconds $settleSeconds
            $journalPost = [AvelJournal]::Query('C:')
            $postTime = Get-FileTimeNow
            $post = New-OracleCapture "$label-post"
            $journalAfter = [AvelJournal]::Query('C:')
        } finally {
            $result = Invoke-Trace @('stop', '--json') "$label-stop"
            $stopTime = Get-FileTimeNow
            Stop-Hold
        }
        $inside = New-InsideWindow "$($result.json.trace.id)" $journalBefore $journalPost $postTime $stopTime
    }
    if (-not $result.json) { Fail "$($label): avel-cli printed no JSON (exit $($result.exit))"; return $null }
    Write-TraceSummary $result.json
    if ($result.json.trace.stop_reason -eq 'reboot-pending') {
        Fail "$($label): the installer asked for a restart; the trace stays open, stopping it"
        $null = Invoke-Trace @('stop', '--json') "$label-stop-after-reboot-request"
    }
    $all = Get-AllChanges $result $label
    Write-NoiseCounts $all
    $record = @{ id = $result.json.trace.id; pre = $pre; post = $post; result = $result.json; all = $all
                 seconds = $seconds; install = $installed; lines = @(Get-CaptureDiff $pre $post); gaps = $gaps
                 journal = @{ before = $journalBefore; after = $journalAfter }; inside = $inside }
    $traced[$label] = $record
    return $record
}

function Invoke-UnitTests {
    Log ''
    Log 'Unit tests that need elevation (USN journal, installer watch, registry snapshot, trace session) or the Sandbox''s user profile'
    # stdout only: under 'Stop', Windows PowerShell turns a native program's stderr into an error.
    # KeysThatDenyAccessAreUnreadable skips wherever SeBackupPrivilege can be enabled, as it always
    # can elevated; the host runs it.
    $filter = 'UsnLiveTest.*:InstallerWatch*:RegistrySnapshot*:TraceSessionLiveTest.*:UserProfilesTest.*' +
              '-RegistrySnapshotTest.KeysThatDenyAccessAreUnreadable'
    $tests = @(& "$bin\avel_core_tests.exe" "--gtest_filter=$filter")
    $code = $LASTEXITCODE
    $tests | Where-Object { $_ -match '^\[  (PASSED|FAILED|SKIPPED) ' } | ForEach-Object { Log "  $_" }
    # What each failed test printed, from its RUN line on.
    $output = [System.Collections.Generic.List[string]]::new()
    foreach ($line in $tests) {
        if ($line -match '^\[ RUN      \] ') { $output.Clear() }
        $output.Add($line)
        if ($line -match '^\[  FAILED  \] ([^ ]+) \(') {
            Log "  output of $($Matches[1]):"
            foreach ($text in @($output | Select-Object -First 40)) { Log "    $text" }
        }
    }
    $ran = 0
    foreach ($line in $tests) { if ($line -match '^\[==========\] ([0-9]+) tests? from .* ran') { $ran = [int]$Matches[1] } }
    if ($code -ne 0 -or $ran -eq 0 -or ($tests -match '^\[  SKIPPED \]')) {
        Fail "the tracker's unit tests did not all pass elevated (exit $code, $ran ran)"
    } else {
        Log "  OK: $ran unit tests pass elevated"
    }
}

# Opens and closes a stock app the way a user does (through the shell); says how it went.
function Invoke-StockApp([string]$app) {
    $name = Split-Path $app -Leaf
    try {
        $process = Start-Process $app -PassThru
        Start-Sleep -Seconds 3
        $null = $process.CloseMainWindow()
        if (-not $process.WaitForExit(10000)) { $process.Kill(); return "$name started and closed (killed)" }
        return "$name started and closed"
    } catch {
        return "$name failed: $($_.Exception.Message)"
    }
}

function Invoke-IdleRound {
    Log ''
    Log 'Round 0: idle - a minute with nothing installed, a stock app opened and closed'
    Close-OpenTrace
    # Notepad is a packaged app now, missing from the Sandbox. Winver keeps no settings (Character
    # Map saves its own, a real change).
    $app = @('winver.exe', 'notepad.exe' | ForEach-Object { Join-Path $env:SystemRoot "System32\$_" } |
        Where-Object { Test-Path -LiteralPath $_ })[0]
    if ($app) {
        # Once before the trace: the shell's first launch of a program caches its name in Explorer's
        # MuiCache (creating the key in a new profile), the user's change, never noise (removal
        # relies on it), and one a profile in use has long had.
        Log "  before the trace: $(Invoke-StockApp $app)"
        Start-Sleep -Seconds $settleSeconds
    }
    $start = Invoke-Trace @('start', '--description', 'idle', '--json') 'r0-start'
    if ($start.exit -ne 0 -or $start.json.trace.state -ne 'open') {
        Fail "trace start: exit $($start.exit), state $($start.json.trace.state)"
        return
    }
    Start-Hold
    try {
        $journalBefore = [AvelJournal]::Query('C:')
        $pre = New-OracleCapture 'r0-idle-pre'
        Start-Sleep -Seconds 60
        $appNote = if ($app) { Invoke-StockApp $app } else { 'no stock app found' }
        Start-Sleep -Seconds $settleSeconds
        $journalPost = [AvelJournal]::Query('C:')
        $postTime = Get-FileTimeNow
        $post = New-OracleCapture 'r0-idle-post'
        $journalAfter = [AvelJournal]::Query('C:')
    } finally {
        $stop = Invoke-Trace @('stop', '--json') 'r0-stop'
        $stopTime = Get-FileTimeNow
        Stop-Hold
    }
    Log "  $appNote"
    $inside = New-InsideWindow "$($stop.json.trace.id)" $journalBefore $journalPost $postTime $stopTime
    if (-not $stop.json) { Fail "trace stop printed no JSON (exit $($stop.exit))"; return }
    $json = $stop.json
    Write-TraceSummary $json
    if ($json.trace.state -ne 'complete') { Fail "idle: the trace is $($json.trace.state)" }
    if ($json.trace.stop_reason -ne 'manual') { Fail "idle: stop reason $($json.trace.stop_reason), not manual" }
    $changes = @($json.changes | Where-Object { $_ })
    if ($changes.Count -or $json.trace.counts.changes) {
        Fail "idle: $([Math]::Max($changes.Count, [int]$json.trace.counts.changes)) change(s) that aren't noise"
        foreach ($change in $changes) { Log "      $(Format-Change $change)" }
    } else {
        Log '  OK: no change that isn''t noise'
    }
    $all = Get-AllChanges $stop 'r0'
    Write-NoiseCounts $all
    Test-Completeness @(Get-CaptureDiff $pre $post) $all 'r0-idle' -Calibrate -Journal @{ before = $journalBefore; after = $journalAfter } `
        -Inside $inside
    Log "  $($background.Count) path(s) the trace didn't account for while idle; later rounds log them as KNOWN"
}

function Invoke-FixtureRound {
    Log ''
    Log 'Round 1: Contoso Trace Fixture installed between trace start and trace stop'
    Remove-TraceFixture
    Initialize-TraceFixture
    $paths = Get-TraceFixturePaths
    $command = @(Get-TraceFixtureCommand)
    $file = $command[0]
    $arguments = @($command | Select-Object -Skip 1 | ForEach-Object { Format-Argument $_ }) -join ' '
    # A closure sees its variables, but not this script's functions.
    $install = { (Start-Process $file $arguments -Wait -PassThru -WindowStyle Hidden).ExitCode }.GetNewClosure()
    $record = Invoke-TracedRound 'r1-fixture' 'Contoso Trace Fixture' -Install $install
    if (-not $record) { return }
    $record.paths = $paths  # For 5b's exact checks
    $json = $record.result
    if ($record.install -ne 0) { Fail "the fixture's installer returned $($record.install)" }
    if ($json.trace.state -ne 'complete' -or $json.trace.mode -ne 'manual' -or $json.trace.stop_reason -ne 'manual') {
        Fail "round 1: state $($json.trace.state), mode $($json.trace.mode), stop reason $($json.trace.stop_reason)"
    }
    foreach ($change in @($json.changes | Where-Object { $_ })) { Log "    $(Format-Change $change)" }
    Test-Completeness $record.lines $record.all 'r1-fixture' -Journal $record.journal -Inside $record.inside
    Test-Precision $json $record.lines 'round 1' -Strict
    Test-FixtureTrace $json 'round 1' $paths
}

function Invoke-SampleRound($sample, [int]$number) {
    Log ''
    Log "Round $($number): $($sample.name) through trace run"
    if (-not (Test-Path $sample.command[0])) { Fail "$($sample.command[0]) is missing (run Get-SampleInstallers.ps1)"; return }
    $record = Invoke-TracedRound $sample.label $sample.name -Launch $sample.command
    if (-not $record) { return }
    $json = $record.result
    Log "  took $([int]$record.seconds) s"
    if ($json.trace.stop_reason -ne 'installer-done') { Fail "$($sample.name): stop reason $($json.trace.stop_reason), not installer-done" }
    if ($json.trace.state -ne 'complete' -or $json.trace.mode -ne 'launch') { Fail "$($sample.name): state $($json.trace.state), mode $($json.trace.mode)" }
    if ($json.trace.watch.exit_code -notin 0, 3010) { Fail "$($sample.name): the installer exited with $($json.trace.watch.exit_code)" }
    foreach ($change in @($json.changes | Where-Object { $_ })) { Log "    $(Format-Change $change)" }
    Test-Completeness $record.lines $record.all $sample.label -Gaps $record.gaps -Journal $record.journal
    Test-Precision $json $record.lines $sample.label
}

function Invoke-RunAfterRound {
    Log ''
    Log 'Round 5: the fixture through trace run, starting its app at the end'
    Close-OpenTrace
    Remove-TraceFixture
    Initialize-TraceFixture
    $paths = Get-TraceFixturePaths
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $run = Invoke-Trace (@('run', '--description', 'Contoso Trace Fixture, run after', '--json', '--') +
                         @(Get-TraceFixtureCommand -RunAfter)) 'r5-run-after' 300
    $seconds = $clock.Elapsed.TotalSeconds
    if (-not $run.json) { Fail "trace run printed no JSON (exit $($run.exit))"; return }
    $json = $run.json
    $traced['r5-run-after'] = @{ id = $json.trace.id; result = $json; seconds = $seconds }
    Write-TraceSummary $json
    if ($json.trace.stop_reason -ne 'installer-done' -or $json.trace.watch.end -ne 'installer-done') {
        Fail "round 5: stop reason $($json.trace.stop_reason), watch end $($json.trace.watch.end)"
    }
    if ($seconds -ge 45) { Fail "round 5: trace run took $([int]$seconds) s, not under 45 s" }
    else { Log "  OK: trace run ended by itself after $([int]$seconds) s" }
    $left = @($json.trace.watch.left_running | Where-Object { $_ -and $_.image -like "*\Contoso Trace\app.exe" })
    if (-not $left) { Fail 'round 5: the app is not listed as left running' }
    else { Log "  OK: the app is listed as left running (pid $($left[0].pid))" }
    $running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq "$traceRoot\app.exe" })
    if (-not $running) { Fail 'round 5: the app is not running any more' }
    else { Log '  OK: the app is still running'; $running | Stop-Process -Force }
    Test-FixtureTrace $json 'round 5' $paths
}

function Invoke-KilledRunRound {
    Log ''
    Log 'Round 6a: trace run killed after its snapshot; trace stop must finish the trace'
    Close-OpenTrace
    Remove-TraceFixture
    Initialize-TraceFixture
    $before = @(Get-TraceList | ForEach-Object { $_.id })
    $arguments = @(@('trace', 'run', '--description', 'killed', '--json', '--') + @(Get-TraceFixtureCommand) |
        ForEach-Object { Format-Argument $_ }) -join ' '
    $out = Join-Path $env:TEMP 'avel-r6-killed.out'
    $process = Start-Process $cli $arguments -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    $null = $process.Handle
    $open = $null
    $deadline = (Get-Date).AddSeconds(120)
    while (-not $open -and -not $process.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $open = @(Get-TraceList | Where-Object { $_.state -eq 'open' -and $before -notcontains $_.id })[0]
    }
    if (-not $open) {
        Fail "round 6a: no trace became open (avel-cli $(if ($process.HasExited) { "exited with $($process.ExitCode)" } else { 'still running' }))"
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        return
    }
    # Let the installer start, so the trace has something to record.
    $installer = $null
    $deadline = (Get-Date).AddSeconds(30)
    while (-not $installer -and (Get-Date) -lt $deadline) {
        $installer = @(Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" |
            Where-Object { $_.CommandLine -like '*ContosoTraceSetup\install.ps1*' })[0]
        if (-not $installer) { Start-Sleep -Milliseconds 250 }
    }
    if ($process.HasExited) { Fail "round 6a: trace run ended (exit $($process.ExitCode)) before it could be killed"; return }
    Stop-Process -Id $process.Id -Force
    $process.WaitForExit()
    Log "  trace $($open.id) open; avel-cli killed $(if ($installer) { "while the installer (pid $($installer.ProcessId)) ran" } else { 'before the installer started' })"
    if ($installer) { Wait-Process -Id $installer.ProcessId -Timeout 120 -ErrorAction SilentlyContinue }
    $state = @(Get-TraceList | Where-Object { $_.id -eq $open.id })[0].state
    Log "  after the kill the trace is $state"
    $stop = Invoke-Trace @('stop', '--json') 'r6-killed-stop'
    if (-not $stop.json) { Fail "round 6a: trace stop printed no JSON (exit $($stop.exit))"; return }
    $json = $stop.json
    Write-TraceSummary $json
    if ($json.trace.id -ne $open.id) { Fail "round 6a: trace stop finished $($json.trace.id), not $($open.id)" }
    if ($json.trace.state -ne 'complete') { Fail "round 6a: the trace is $($json.trace.state), not complete" }
    else { Log '  OK: trace stop finished the killed run''s trace' }
    if (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso Trace') {
        if (-not @($json.changes | Where-Object { $_ -and $_.change -eq 'created' -and $_.location.path -eq $traceRoot })) {
            Fail "round 6a: the installer finished, but the trace doesn't list $traceRoot"
        } else { Log "  OK: the trace lists what the installer did after avel-cli was killed" }
    } else {
        Log '  the installer did not finish (it ended with avel-cli)'
    }
    Remove-TraceFixture
}

function Invoke-JournalReplacedRound {
    Log ''
    Log 'Round 6b: the USN journal deleted and recreated during a trace'
    Close-OpenTrace
    $start = Invoke-Trace @('start', '--description', 'journal replaced', '--json') 'r6-journal-start'
    if ($start.exit -ne 0 -or $start.json.trace.state -ne 'open') { Fail "trace start: exit $($start.exit)"; return }
    $probe = 'HKLM\SOFTWARE\Contoso Trace Probe'
    New-Item "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Contoso Trace Probe" -Force |
        New-ItemProperty -Name Marker -Value 'journal replaced' -Force | Out-Null
    $deleted = Invoke-Native 'fsutil.exe' 'usn deletejournal /d C:'
    $created = Invoke-Native 'fsutil.exe' 'usn createjournal m=33554432 a=4194304 C:'
    Log "  fsutil usn deletejournal: exit $deleted; createjournal: exit $created"
    if ($deleted -ne 0 -or $created -ne 0) { Fail 'round 6b: fsutil failed' }
    $stop = Invoke-Trace @('stop', '--json') 'r6-journal-stop'
    Remove-Item "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Contoso Trace Probe" -Recurse -Force
    if (-not $stop.json) { Fail "round 6b: trace stop printed no JSON (exit $($stop.exit))"; return }
    $json = $stop.json
    Write-TraceSummary $json
    if ($stop.exit -ne 0) { Fail "round 6b: trace stop exited with $($stop.exit)" }
    if ($json.trace.state -ne 'incomplete' -or $json.trace.complete) { Fail "round 6b: the trace is $($json.trace.state), complete $($json.trace.complete)" }
    else { Log '  OK: the trace is incomplete' }
    $volume = @($json.trace.volumes | Where-Object { $_ -and $_.root -eq 'C:\' })
    if (-not $volume -or $volume[0].status -ne 'replaced') { Fail "round 6b: C: is $($volume.status), not replaced" }
    if (-not @($json.trace.incomplete_reasons | Where-Object { $_ -and $_.volume -eq 'C:\' -and $_.status -eq 'replaced' })) {
        Fail 'round 6b: incomplete_reasons does not name C: as replaced'
    } elseif ($volume) { Log '  OK: C: is reported replaced' }
    if (-not @($json.changes | Where-Object { $_ -and $_.change -eq 'created' -and $_.location.kind -eq 'registry-key' -and $_.location.key -eq $probe })) {
        Fail "round 6b: the registry part is missing: $probe isn't reported"
    } else { Log '  OK: the registry part is still reported' }
}

# Diagnostics, raw: how the base layer's placeholders look to programs (docs/trace.md, Windows
# containers).
function Write-SystemFacts {
    foreach ($path in "$env:SystemRoot\System32\wbem", "$env:SystemRoot\System32\kernel32.dll") {
        Log "Attributes of $($path) as programs see them: $(Get-ItemFacts $path)"
    }
}

# A container's base layer (Windows Sandbox): two of Windows' sounds, which nothing here plays,
# one deleted and one renamed after takeown and icacls (as an installer replacing a system file
# would), between `trace start` and `trace stop`. The container leaves a tombstone under each old
# name, a new item programs don't see: the trace must report the deletion and the rename (the
# security changes go with them) and nothing created, and account for the oracle's diff.
function Invoke-ContainerRound {
    Log ''
    Log "Round 0b: a container's base layer - a system file deleted, another renamed"
    $sounds = @(Get-ChildItem -LiteralPath "$env:SystemRoot\Media" -Filter '*.wav' -File -ErrorAction SilentlyContinue |
        Sort-Object Name | Select-Object -First 2)
    if ($sounds.Count -lt 2) { Fail 'round 0b: there are no two sounds in the Media folder'; return }
    $gone = $sounds[0].FullName
    $old = $sounds[1].FullName
    $new = "$old.avel"
    $change = {
        $ErrorActionPreference = 'Stop'
        foreach ($path in $gone, $old) {
            $null = & takeown.exe /f $path /a
            $null = & icacls.exe $path /grant '*S-1-5-32-544:F'
        }
        Remove-Item -LiteralPath $gone -Force
        Rename-Item -LiteralPath $old -NewName (Split-Path $new -Leaf)
        0
    }.GetNewClosure()
    $record = Invoke-TracedRound 'r0b-container' "a container's base layer" -Install $change
    $null = $traced.Remove('r0b-container')  # Not an installation
    if (-not $record) { return }
    $json = $record.result
    if ($json.trace.state -ne 'complete') { Fail "round 0b: the trace is $($json.trace.state)" }
    $media = Split-Path $gone
    $all = @($record.all.changes | Where-Object { $_ -and $_.location.path -and (Split-Path $_.location.path) -eq $media })
    foreach ($change in $all) { Log "    $(Format-Change $change)" }
    $reported = @($all | Where-Object { $_.noise -eq 'none' })
    $deleted = @($reported | Where-Object { $_.change -eq 'deleted' -and $_.location.path -eq $gone })
    $renamed = @($reported | Where-Object { $_.change -eq 'renamed' -and $_.location.path -eq $new -and $_.old_path -eq $old })
    $tombstones = @($all | Where-Object { $_.change -eq 'created' -and $_.location.path -in $gone, $old })
    if ($deleted.Count -ne 1 -or $renamed.Count -ne 1 -or $reported.Count -ne 2) {
        Fail "round 0b: want exactly 'deleted $gone' and 'renamed $new <- $old' in $media, got $($reported.Count) change(s)"
    } else {
        Log "  OK: the deletion and the rename, nothing else in $media"
    }
    if ($tombstones) { Fail "round 0b: $($tombstones.Count) tombstone(s) reported as created" }
    if ($deleted.Count -ne 1 -or $renamed.Count -ne 1 -or $tombstones) {
        Write-ItemRecords @(@($gone, $old, $new) | ForEach-Object { [pscustomobject]@{ path = $_ } }) @{}
    }
    Test-Completeness $record.lines $record.all 'r0b-container' -Journal $record.journal -Inside $record.inside
    Test-Precision $json $record.lines 'round 0b' -Strict
}
try {
    Log "Avel Phase 5 trace scenario, $(Get-Date -Format s)"
    Log "OS: $([Environment]::OSVersion.VersionString), user: $env:USERNAME ($sid), $([Environment]::ProcessorCount) processors"
    Log "avel-cli: $((& $cli version) -join ' ')"
    Write-SystemFacts
    if (Test-Path $captures) { Remove-Item $captures -Recurse -Force }

    Invoke-Round 'unit tests' { Invoke-UnitTests }
    Invoke-Round 'round 0' { Invoke-IdleRound }
    Invoke-Round 'round 0b' { Invoke-ContainerRound }
    Invoke-Round 'round 1' { Invoke-FixtureRound }
    Invoke-Round 'round 1 (5b)' { Invoke-FixtureRevertRound $traced['r1-fixture'] }
    $number = 2
    foreach ($sample in $traceSamples) {
        Invoke-Round "round $number" { Invoke-SampleRound $sample $number }
        Invoke-Round "round $number (5b)" { Invoke-UninstallByTrace $sample $number }
        $number++
    }
    Invoke-Round 'round 5' { Invoke-RunAfterRound }
    Invoke-Round 'round 6a' { Invoke-KilledRunRound }
    Invoke-Round 'round 6b' { Invoke-JournalReplacedRound }
    Invoke-Round 'GUI round' { Invoke-TraceGuiRound }
} catch {
    Fail "scenario error: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
    Write-Report
}
