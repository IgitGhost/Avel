# Runs inside Windows Sandbox (Start-AvelSandbox.ps1 -Scenario startup). Phase 6a acceptance test
# for Avel Tools' Startup manager (`avel-cli startup`), on "Contoso Startup" (startup_fixture.ps1):
# one entry of every source for the machine, the current user and a second local user who is signed
# out, plus a Run value of Windows' own.
#   1. Fixtures: the second user and their profile are created without signing in (CreateProfile),
#      and their hive must not be loaded; the fixtures are written into it while the scenario
#      mounts it, and it is unloaded again before Avel runs.
#   2. List: `startup list --all-users --json` must find every fixture once, enabled, with its kind,
#      user, 32-bit flag, target, owner program, StartupApproved key and first delete location;
#      the Windows-own entry protected; `startup list` without --all-users must leave out the
#      signed-out user's entries and give the others the same ids.
#   3. Refusals, which must change nothing (state dumps before and after): Windows' own entry for
#      every action (protected), RunOnce, RunOnceEx and a user's 32-bit Run value for disable and
#      enable (not-supported), a request mixing one of them with a supported entry, an unknown id,
#      enabling an enabled entry (already-in-state, not an error) and a dry run.
#   4. Every fixture but Windows' own at once: disable must set the StartupApproved values as Task
#      Manager does (03 00 00 00 and the time of the change), the task's Enabled flag off and the
#      services' start to Manual (still listed, disabled_by_avel, the delayed flag kept); enable
#      must write 02 and 11 zeros, enable the task and start the services automatically again;
#      delete must remove every entry with its StartupApproved value. Undo of the three sets, newest
#      first, must give back identical state dumps.
#   5. A service disabled with --service-start disabled: start Disabled, listed as disabled by Avel;
#      disabling it again (Manual) is already-in-state; enable, then undo both.
#   6. The signed-out user's entries alone: disable, enable, delete and their undo.
# After every avel-cli run the signed-out user's hive must not be loaded (neither under HKU nor
# privately: its NTUSER.DAT must open exclusively).
#   GUI (Phase 6a, the Startup window): startup_gui_round.ps1 defines Invoke-StartupGuiRound, which
#      runs last on the fixtures as rounds 1-6 leave them (all there, enabled; ids in $fixtures):
#      avel.exe through UI Automation disables, enables, deletes and undoes, checked with avel-cli,
#      the state dumps and the window itself.
# Writes results\startup.json (avel-cli's output), results\startup.log and, last,
# results\startup.txt.

$ErrorActionPreference = 'Stop'
$bin = 'C:\Avel\bin'
$results = 'C:\Avel\results'
$cli = Join-Path $bin 'avel-cli.exe'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$scenario = 'startup'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\startup_fixture.ps1"

$callNumber = 0
$fixtures = @()
$allSpecs = @()
$enabledBytes = '02' + ('0' * 22)

function Invoke-Round([string]$name, [scriptblock]$body) {
    Log ''
    Log $name
    $started = Get-Date
    try {
        & $body
    } catch {
        Fail "$($name): $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
        try { if (Test-Path "Registry::HKEY_USERS\$offlineMount") { Dismount-OtherHive } } catch { }
    }
    Log ('  ({0:N0} s)' -f ((Get-Date) - $started).TotalSeconds)
}

# avel-cli startup <arguments> --json; then the signed-out user's hive must be free again.
function Invoke-Startup([string[]]$arguments, [string]$label) {
    $script:callNumber++
    $run = Invoke-Cli (@('startup') + $arguments + @('--json')) ('{0:00}-{1}' -f $script:callNumber, $label)
    Test-OtherHiveFree $label
    return $run
}

function Get-StartupList([string]$label, [switch]$AllUsers) {
    $arguments = @('list')
    if ($AllUsers) { $arguments += '--all-users' }
    $run = Invoke-Startup $arguments $label
    if ($run.exit -ne 0 -or -not $run.json) { throw "startup list returned $($run.exit)" }
    return $run.json
}

function Find-Entries($list, $fixture) {
    @($list.entries | Where-Object {
        $_.kind -eq $fixture.kind -and $_.name -eq $fixture.name -and $_.user_sid -eq $fixture.sid -and
        [bool]$_.wow64 -eq $fixture.wow64 })
}

function Get-Entry($list, $fixture, [string]$label) {
    $found = @(Find-Entries $list $fixture)
    if ($found.Count -ne 1) { Fail "$($label): $($fixture.label) listed $($found.Count) time(s)"; return $null }
    return $found[0]
}

function Get-Fixtures([string[]]$labels) { @($fixtures | Where-Object { $_.label -in $labels }) }

# startup <action> <the fixtures' ids> [extra] --yes --json; checks the exit code.
function Invoke-StartupAction([string]$action, $selected, [string]$label, [string[]]$extra = @(), [int]$expectExit = 0,
                              [switch]$NoYes) {
    $ids = @($selected | ForEach-Object { if ($_ -is [string]) { $_ } else { $_.id } })
    $arguments = @($action) + $ids + $extra
    if (-not $NoYes) { $arguments += '--yes' }
    $run = Invoke-Startup $arguments $label
    if ($run.exit -ne $expectExit) { Fail "$($label): exit $($run.exit), not $expectExit" }
    if (-not $run.json) { throw "$($label): no JSON output" }
    return $run.json
}

# Every item of the action's change set is done; returns the set's id.
function Test-ChangeDone($json, [string]$label) {
    $change = $json.change
    if (-not $change -or -not $change.set_id) { Fail "$($label): no change set"; return $null }
    Log "  change set $($change.set_id), $(@($change.items).Count) item(s)"
    foreach ($item in $change.items) {
        Log ('    {0,-4} {1,-18} {2,-17} {3}' -f $item.index, $item.state, $item.location.kind, (Where-Text $item.location))
        if ($item.state -ne 'done') { Fail "$($label): $(Where-Text $item.location) is $($item.state) ($($item.reason))" }
    }
    return $change.set_id
}

function Test-Issues($json, [string]$code, [int]$count, [string]$label) {
    $issues = @($json.issues | Where-Object { $_.code -eq $code })
    if ($issues.Count -ne $count -or @($json.issues).Count -ne $count) {
        Fail "$($label): issues $(@($json.issues | ForEach-Object { "$($_.id)=$($_.code)" }) -join ', '), expected $count $code"
    }
    if ($json.change) { Fail "$($label): a change set was made ($($json.change.set_id))" }
}

function Invoke-Undo([string]$setId, [string]$label) {
    if (-not $setId) { return }
    $script:callNumber++
    $run = Invoke-Cli @('undo', $setId, '--json') ('{0:00}-{1}' -f $script:callNumber, $label)
    Test-OtherHiveFree $label
    if ($run.json.state -ne 'restored') { Fail "$($label): set $setId is $($run.json.state) (exit $($run.exit))" }
    else { Log "  OK: $label ($setId restored)" }
}

# "type=3 data=..." of a value spec's dump, or $null when the value is missing.
function Get-DumpData($dump, [string]$spec) {
    if ($dump[$spec] -match '^type=(\d+) data=([0-9a-fA-F]*)') { return @{ type = [int]$Matches[1]; data = $Matches[2].ToLowerInvariant() } }
    return $null
}

# Task Manager's "disabled": 03 00 00 00 and the FILETIME of the change.
function Test-DisabledBytes([string]$hex, [string]$label) {
    if ($hex -notmatch '^03000000([0-9a-f]{16})$') { Fail "$($label): disabled bytes $hex"; return }
    $bytes = [byte[]]@(for ($i = 0; $i -lt 16; $i += 2) { [Convert]::ToByte($Matches[1].Substring($i, 2), 16) })
    $when = [DateTime]::FromFileTimeUtc([BitConverter]::ToInt64($bytes, 0))
    $age = ([DateTime]::UtcNow - $when).TotalMinutes
    if ($age -lt -5 -or $age -gt 60) { Fail "$($label): disabled at $($when.ToString('s'))Z, not now" }
}

# Each fixture's listed state and its real state (the dump): StartupApproved bytes, the task's
# Enabled flag, the service's start (`serviceStart` when disabled).
function Test-States($list, $selected, [string]$expected, [string]$label, [int]$serviceStart = 3) {
    $dump = Get-StartupDump (Get-FixtureSpecs $selected)
    $bad = $failures
    foreach ($fixture in $selected) {
        $what = "$label, $($fixture.label)"
        $entry = Get-Entry $list $fixture $label
        if (-not $entry) { continue }
        if ($entry.id -ne $fixture.id) { Fail "$($what): id $($entry.id), not $($fixture.id)" }
        if ($entry.state -ne $expected) { Fail "$($what): listed $($entry.state)" }
        switch ($fixture.toggle) {
            'approval' {
                $listed = if ($entry.approval.value) { "$($entry.approval.value.data)".ToLowerInvariant() } else { '' }
                $real = Get-DumpData $dump $fixture.approval
                if (-not $real -or $real.type -ne 3 -or $real.data -ne $listed) {
                    Fail "$($what): StartupApproved value $($dump[$fixture.approval].Trim()), listed $listed"
                }
                if ($expected -eq 'disabled') { Test-DisabledBytes $listed $what }
                elseif ($listed -ne $enabledBytes) { Fail "$($what): enabled bytes $listed" }
            }
            'task' {
                $enabled = $expected -eq 'enabled'
                if ([bool]$entry.task_enabled -ne $enabled) { Fail "$($what): task_enabled $($entry.task_enabled)" }
                if ($dump[$fixture.item] -notmatch "^enabled=$($enabled.ToString().ToLowerInvariant())") {
                    Fail "$($what): the task is $(($dump[$fixture.item] -split "`n")[0])"
                }
            }
            'service' {
                $start = if ($expected -eq 'enabled') { 2 } else { $serviceStart }
                if ($entry.service_start -ne $start) { Fail "$($what): listed start $($entry.service_start), not $start" }
                if ($dump[$fixture.item] -notmatch "(?m)^type=\d+ start=$start ") { Fail "$($what): the service's start isn't $start" }
                if ([bool]$entry.service_delayed -ne ($fixture.label -eq 'service-delayed')) {
                    Fail "$($what): delayed flag $($entry.service_delayed)"
                }
                if ([bool]$entry.disabled_by_avel -ne ($expected -eq 'disabled')) {
                    Fail "$($what): disabled_by_avel $($entry.disabled_by_avel)"
                }
            }
        }
    }
    if ($failures -eq $bad) { Log "  OK: $label ($(@($selected).Count) entries $expected)" }
}

function Test-Gone($list, $selected, [string]$label) {
    $dump = Get-StartupDump (Get-FixtureSpecs $selected)
    $bad = $failures
    foreach ($fixture in $selected) {
        if (@(Find-Entries $list $fixture).Count) { Fail "$($label): $($fixture.label) is still listed" }
        foreach ($spec in @($fixture.item, $fixture.approval) | Where-Object { $_ }) {
            if ($dump[$spec] -notmatch '^<missing>') { Fail "$($label): still there: $spec" }
        }
    }
    if ($failures -eq $bad) { Log "  OK: $label ($(@($selected).Count) entries and their StartupApproved values gone)" }
}

function Invoke-FixtureRound {
    New-OtherUser
    Test-OtherHiveFree 'profile created'
    Install-StartupFixtures
    Test-OtherHiveFree 'fixtures installed'
    $script:fixtures = Get-StartupFixtures
    $script:allSpecs = Get-FixtureSpecs $fixtures
    Log "  $($fixtures.Count) fixtures, $($allSpecs.Count) state dump specs; Windows' own file: $(Get-WindowsOwnFile)"
}

function Invoke-ListRound {
    $all = Get-StartupList 'list-all-users' -AllUsers
    if (-not $all.elevated) { Fail 'avel-cli does not run elevated' }
    Log "  $(@($all.entries).Count) entries listed in $($all.elapsed_ms) ms, $(@($all.entries | Where-Object { $_.protection -ne 'none' }).Count) protected"
    foreach ($diagnostic in $all.diagnostics) {
        Log "  diagnostic: $($diagnostic.source) $($diagnostic.code) $($diagnostic.subject) $($diagnostic.error)"
        if ($diagnostic.subject -eq $otherSid) { Fail "the signed-out user's hive: $($diagnostic.code)" }
    }
    $firstLocation = @{ 'run' = 'registry-value'; 'run-once' = 'registry-value'; 'run-once-ex' = 'registry-key'
                        'startup-folder' = 'file'; 'task' = 'task'; 'service' = 'service' }
    foreach ($fixture in $fixtures) {
        $what = "list, $($fixture.label)"
        $entry = Get-Entry $all $fixture 'list'
        if (-not $entry) { continue }
        $fixture.id = $entry.id
        Log ('  {0}  {1,-8} {2,-15} {3,-8} {4,-16} {5}' -f $entry.id, $entry.state, $entry.kind, $fixture.owner,
             $fixture.label, $entry.target)
        $bad = $failures
        if ($entry.id -notmatch '^[0-9a-f]{10}$') { Fail "$($what): id $($entry.id)" }
        if ($entry.state -ne 'enabled') { Fail "$($what): $($entry.state)" }
        if ($entry.protection -ne $fixture.protection) { Fail "$($what): protection $($entry.protection)" }
        if ($entry.target -ne $fixture.target) { Fail "$($what): target '$($entry.target)', not '$($fixture.target)'" }
        if ($entry.target_missing) { Fail "$($what): target missing" }
        if ($fixture.target -like "$startupRoot\*" -and $entry.program_name -ne 'Contoso Startup') {
            Fail "$($what): program '$($entry.program_name)'"
        }
        if ($fixture.owner -eq 'other' -and $entry.user_name -notmatch "\\$otherUser$") { Fail "$($what): user $($entry.user_name)" }
        if ($fixture.approvalKey) {
            $approval = $entry.approval
            if (-not $approval) { Fail "$($what): no StartupApproved key" }
            else {
                $root = if ($fixture.owner -eq 'machine') { $approval.key -like 'HKLM\*' } else { $approval.key -like "*\$($fixture.sid)\*" }
                if (-not $approval.key.EndsWith("\$($fixture.approvalKey)", 'OrdinalIgnoreCase') -or -not $root) {
                    Fail "$($what): StartupApproved key $($approval.key)"
                }
                if ($approval.value_name -ne $fixture.name) { Fail "$($what): StartupApproved value $($approval.value_name)" }
                if ($approval.value) { Fail "$($what): a StartupApproved value already" }
            }
        } elseif ($entry.approval) { Fail "$($what): has a StartupApproved key $($entry.approval.key)" }
        $first = @($entry.delete_locations)[0]
        if ($first.kind -ne $firstLocation[$fixture.kind]) { Fail "$($what): first delete location $($first.kind)" }
        if ($fixture.kind -eq 'service') {
            if ($entry.service_start -ne 2) { Fail "$($what): start $($entry.service_start)" }
            if ([bool]$entry.service_delayed -ne ($fixture.label -eq 'service-delayed')) { Fail "$($what): delayed $($entry.service_delayed)" }
        }
        if ($fixture.kind -eq 'task' -and -not $entry.task_enabled) { Fail "$($what): task not enabled" }
        if ($fixture.label -eq 'windows-own' -and -not $entry.signer.windows) { Fail "$($what): signer $($entry.signer.name) isn't Windows'" }
        if ($failures -eq $bad) { Log "    OK" }
    }

    $mine = Get-StartupList 'list'
    $bad = $failures
    foreach ($fixture in $fixtures) {
        $found = @(Find-Entries $mine $fixture)
        if ($fixture.owner -eq 'other') {
            if ($found.Count) { Fail "list without --all-users shows $($fixture.label)" }
        } elseif ($found.Count -ne 1 -or $found[0].id -ne $fixture.id) {
            Fail "list without --all-users: $($fixture.label) $($found.Count) time(s), id $($found[0].id)"
        }
    }
    if ($failures -eq $bad) { Log '  OK: without --all-users: the machine''s and the current user''s entries, same ids' }
}

function Invoke-RefusalRound {
    $before = Get-StartupDump $allSpecs
    $own = Get-Fixtures 'windows-own'
    foreach ($action in 'disable', 'enable', 'delete') {
        $json = Invoke-StartupAction $action $own "refuse-own-$action" -expectExit 1
        Test-Issues $json 'protected' 1 "$action Windows' own entry"
        if (@($json.plan).Count) { Fail "$action Windows' own entry: a plan of $(@($json.plan).Count) item(s)" }
    }
    $deleteOnly = @($fixtures | Where-Object { -not $_.toggle })
    foreach ($action in 'disable', 'enable') {
        $json = Invoke-StartupAction $action $deleteOnly "refuse-$action" -expectExit 1
        Test-Issues $json 'not-supported' $deleteOnly.Count "$action RunOnce, RunOnceEx, a user's 32-bit Run value"
    }
    $json = Invoke-StartupAction 'disable' (Get-Fixtures 'run-machine', 'runonce-machine') 'refuse-mixed' -expectExit 1
    Test-Issues $json 'not-supported' 1 'disable a Run value with a RunOnce value'
    $json = Invoke-StartupAction 'delete' @('ffffffffff') 'refuse-unknown' -expectExit 1
    Test-Issues $json 'unknown-id' 1 'delete an unknown id'
    $json = Invoke-StartupAction 'enable' (Get-Fixtures 'run-machine', 'task') 'already-enabled'
    Test-Issues $json 'already-in-state' 2 'enable enabled entries'
    if (@($json.issues | Where-Object { $_.error }).Count) { Fail 'already-in-state counts as an error' }
    $toggles = @($fixtures | Where-Object { $_.toggle -and $_.protection -eq 'none' })
    $json = Invoke-StartupAction 'disable' $toggles 'dry-run' @('--dry-run') -NoYes
    if (@($json.plan).Count -ne $toggles.Count -or $json.change -or @($json.issues).Count) {
        Fail "dry run: $(@($json.plan).Count) planned of $($toggles.Count), change $($json.change.set_id)"
    } else { Log "  OK: dry run plans $($toggles.Count) state changes" }
    Compare-State $before (Get-StartupDump $allSpecs) 'refusals and the dry run changed nothing'
}

function Invoke-BatchRound {
    $toggles = @($fixtures | Where-Object { $_.toggle -and $_.protection -eq 'none' })
    $deletable = @($fixtures | Where-Object { $_.protection -eq 'none' })
    $untouched = @($fixtures | Where-Object { -not $_.toggle -or $_.protection -ne 'none' })
    $before = Get-StartupDump $allSpecs
    $sets = @()

    $json = Invoke-StartupAction 'disable' $toggles 'disable-all'
    $sets += Test-ChangeDone $json 'disable'
    $kinds = @($json.change.items | ForEach-Object { $_.location.kind })
    $expectedKinds = @($toggles | ForEach-Object { @{ approval = 'startup-approved'; task = 'task-state'; service = 'service-start' }[$_.toggle] })
    if (Compare-Object @($kinds | Sort-Object) @($expectedKinds | Sort-Object)) { Fail "disable planned: $($kinds -join ', ')" }
    Test-States (Get-StartupList 'list-disabled' -AllUsers) $toggles 'disabled' 'after disable'
    $now = Get-StartupDump (Get-FixtureSpecs $untouched)
    $same = $true
    foreach ($spec in Get-FixtureSpecs $untouched) { if ($now[$spec] -ne $before[$spec]) { $same = $false; Fail "disable changed $spec" } }
    if ($same) { Log '  OK: entries with no disabled state left alone' }

    $json = Invoke-StartupAction 'enable' $toggles 'enable-all'
    $sets += Test-ChangeDone $json 'enable'
    Test-States (Get-StartupList 'list-enabled' -AllUsers) $toggles 'enabled' 'after enable'

    $json = Invoke-StartupAction 'delete' $deletable 'delete-all'
    $sets += Test-ChangeDone $json 'delete'
    $planned = @($json.change.items | ForEach-Object { Where-Text $_.location })
    foreach ($fixture in $deletable) {
        $entry = @($json.entries | Where-Object { $_.id -eq $fixture.id })[0]
        foreach ($location in @($entry.delete_locations)) {
            if ((Where-Text $location) -notin $planned) { Fail "delete: not planned: $(Where-Text $location)" }
        }
    }
    Test-Gone (Get-StartupList 'list-deleted' -AllUsers) $deletable 'after delete'
    $own = Get-Fixtures 'windows-own'
    $now = Get-StartupDump (Get-FixtureSpecs $own)
    foreach ($spec in Get-FixtureSpecs $own) { if ($now[$spec] -ne $before[$spec]) { Fail "delete changed $spec" } }

    Invoke-Undo $sets[2] 'undo delete'
    Test-States (Get-StartupList 'list-undo-delete' -AllUsers) $toggles 'enabled' 'after undoing delete'
    Invoke-Undo $sets[1] 'undo enable'
    Test-States (Get-StartupList 'list-undo-enable' -AllUsers) $toggles 'disabled' 'after undoing enable'
    Invoke-Undo $sets[0] 'undo disable'
    Compare-State $before (Get-StartupDump $allSpecs) 'undo of the three sets gave back every entry exactly'
    $list = Get-StartupList 'list-undone' -AllUsers
    foreach ($fixture in $deletable) {
        $found = @(Find-Entries $list $fixture)
        if ($found.Count -ne 1 -or $found[0].id -ne $fixture.id -or $found[0].state -ne 'enabled' -or $found[0].disabled_by_avel) {
            Fail "after undo: $($fixture.label) listed $($found.Count) time(s), $($found[0].state), id $($found[0].id)"
        }
    }
}

function Invoke-ServiceRound {
    $service = Get-Fixtures 'service'
    $before = Get-StartupDump (Get-FixtureSpecs $service)
    $json = Invoke-StartupAction 'disable' $service 'service-disabled' @('--service-start', 'disabled')
    if ($json.service_start -ne 'disabled') { Fail "service_start in the output: $($json.service_start)" }
    $first = Test-ChangeDone $json 'disable --service-start disabled'
    Test-States (Get-StartupList 'list-service-disabled' -AllUsers) $service 'disabled' 'start Disabled' 4
    $json = Invoke-StartupAction 'disable' $service 'service-manual-again'
    Test-Issues $json 'already-in-state' 1 'disabling a Disabled service (Manual)'
    $json = Invoke-StartupAction 'enable' $service 'service-enable'
    $second = Test-ChangeDone $json 'enable'
    Test-States (Get-StartupList 'list-service-enabled' -AllUsers) $service 'enabled' 'enabled again'
    Invoke-Undo $second 'undo enable'
    Test-States (Get-StartupList 'list-service-undo-enable' -AllUsers) $service 'disabled' 'listed while Disabled by Avel' 4
    Invoke-Undo $first 'undo disable'
    Compare-State $before (Get-StartupDump (Get-FixtureSpecs $service)) 'undo gave back the service exactly'
    Test-States (Get-StartupList 'list-service-undone' -AllUsers) $service 'enabled' 'automatic again, not disabled by Avel'
}

function Invoke-SignedOutRound {
    $other = @($fixtures | Where-Object { $_.owner -eq 'other' })
    $specs = Get-FixtureSpecs $other
    $before = Get-StartupDump $specs
    $sets = @()
    $sets += Test-ChangeDone (Invoke-StartupAction 'disable' $other 'other-disable') 'disable'
    Test-States (Get-StartupList 'list-other-disabled' -AllUsers) $other 'disabled' 'after disable'
    $sets += Test-ChangeDone (Invoke-StartupAction 'enable' $other 'other-enable') 'enable'
    Test-States (Get-StartupList 'list-other-enabled' -AllUsers) $other 'enabled' 'after enable'
    $sets += Test-ChangeDone (Invoke-StartupAction 'delete' $other 'other-delete') 'delete'
    Test-Gone (Get-StartupList 'list-other-deleted' -AllUsers) $other 'after delete'
    Invoke-Undo $sets[2] 'undo delete'
    Invoke-Undo $sets[1] 'undo enable'
    Invoke-Undo $sets[0] 'undo disable'
    Compare-State $before (Get-StartupDump $specs) "undo gave back $otherUser's entries exactly"
    Test-OtherHiveFree 'end of the round'
}

try {
    Log "Avel Phase 6a startup scenario, $(Get-Date -Format s)"
    Log "OS: $([Environment]::OSVersion.VersionString), user: $env:USERNAME ($currentSid)"
    Log "avel-cli: $((& $cli version) -join ' ')"
    Invoke-Round 'Round 1: fixtures, a signed-out second user' { Invoke-FixtureRound }
    if ($fixtures.Count) {
        Invoke-Round 'Round 2: list' { Invoke-ListRound }
        if (@($fixtures | Where-Object { -not $_.id }).Count) {
            Fail 'not every fixture is listed; the rounds that change them are skipped'
        } else {
            Invoke-Round 'Round 3: refusals change nothing' { Invoke-RefusalRound }
            Invoke-Round 'Round 4: disable, enable, delete every entry, then undo' { Invoke-BatchRound }
            Invoke-Round 'Round 5: a service disabled with --service-start disabled' { Invoke-ServiceRound }
            Invoke-Round "Round 6: the signed-out user's entries alone" { Invoke-SignedOutRound }
            # The Startup window's round.
            if (Test-Path "$PSScriptRoot\startup_gui_round.ps1") {
                . "$PSScriptRoot\startup_gui_round.ps1"
                Invoke-Round 'GUI round' { Invoke-StartupGuiRound }
            }
        }
    }
} catch {
    Fail "scenario error: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
    Write-Report
}
