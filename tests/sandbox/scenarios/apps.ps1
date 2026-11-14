# Runs inside Windows Sandbox (Start-AvelSandbox.ps1 -Scenario apps, which starts it without
# networking). Phase 6b acceptance test for Avel Tools' Windows apps (`avel-cli apps`, the program
# list's uninstall of a packaged app, Tools > Windows apps...), judged by avel_package_probe
# (tests/helpers/package_probe.cpp), which reads the package manager through its public calls and
# shares no code with avel_core.
#   Fixtures (as the 6b spike built them, docs/adr/0008): a second local user, AvelOther, with a
#      password and the batch logon right (a scheduled task runs the probe as them); the test MSIX
#      (AvelTestPackage.msix, unsigned) staged, provisioned and registered for the current user
#      and, through a task, for AvelOther; a second test MSIX (AvelTestPackage2.msix) for the
#      current user alone, never provisioned. The image's own packages (System-signed, and Edge's
#      provisioned stub) are never changed: a snapshot of every other package's users and
#      provisioning must stay the same through every round.
#   1. List: `apps list --all --json` lists every System-signed main package of the image as a
#      protected app with no mode, no framework, the test package with both users, provisioned
#      and every mode, the second one for the current user alone with only remove-everywhere;
#      `apps list` leaves the protected ones out. remove, deprovision and remove-everywhere of a
#      protected app are refused (`protected`), an unknown id too, and a request mixing it with the
#      test package changes nothing.
#   2. Remove for me (by family name), then undo: the current user's registration goes, AvelOther's
#      and the provisioning stay; undo registers it again.
#   3. Don't install for new users (deprovision), then undo: no longer provisioned, both users keep
#      it; undo provisions it again.
#   4. Refusals: Remove for me of the second package (`would-delete-package`), its deprovisioning
#      (`not-provisioned`), remove-everywhere without --allow-irreversible
#      (`irreversible-needs-confirmation`), a request mixing a fitting app with a refused one, and
#      dry runs: nothing changes.
#   5. Crash recovery (AVEL_TEST_FAULT_AFTER) of a Remove for me: killed after its backup (the next
#      start settles it as failed, interrupted: nothing happened); killed after its backup with the
#      registration then removed behind its back (settled as done, by looking; undo registers it
#      again); killed after it was done (settled, undone).
#   6. `avel-cli uninstall` of the test package: a soft removal through a ChangeSet
#      (`results[].change_set`) that `undo` gives back; of the second package: removed as before
#      (no change set), and gone from this PC.
#   7. GUI (apps_gui_round.ps1): Tools > Windows apps..., Check often removed, the test package
#      checked, Remove for me and Undo last change from the window, the Remove for everyone
#      confirmation (its button disabled until "I understand" is checked), cancelled.
#   8. Last, remove for everyone: the test package is deprovisioned and removed for every user, and
#      gone from this PC (not staged, not provisioned, nobody has it).
# Writes results\apps.json (avel-cli's output and the probe's), results\apps.log and, last,
# results\apps.txt.

$ErrorActionPreference = 'Stop'
$bin = 'C:\Avel\bin'
$results = 'C:\Avel\results'
$cli = Join-Path $bin 'avel-cli.exe'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$scenario = 'apps'
. "$PSScriptRoot\common.ps1"

$testMsix = 'C:\Avel\sandbox\installers\AvelTestPackage.msix'
$testMsix2 = 'C:\Avel\sandbox\installers\AvelTestPackage2.msix'
$work = 'C:\AvelApps'
$probeDir = "$work\bin"
$outDir = "$work\out"
$probe = "$probeDir\avel_package_probe.exe"
$windowsApps = 'C:\Program Files\WindowsApps'
$otherUser = 'AvelOther'
$otherPassword = 'Av3l!' + [guid]::NewGuid().ToString('N').Substring(0, 12)
$otherSid = $null
$currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$callNumber = 0
$test = $null        # The test package: full, family, name, id, display
$test2 = $null       # The second test package, current user only
$baseline = $null    # Every other package's users and provisioning, after the fixtures

Add-Type -Namespace AvelApps -Name UserEnv -MemberDefinition @'
[DllImport("userenv.dll", CharSet = CharSet.Unicode)]
public static extern int CreateProfile(string sid, string userName, System.Text.StringBuilder profilePath, uint size);
'@

# Where an error record comes from: file:line.
function Get-ErrorPlace($record) {
    $file = $record.InvocationInfo.ScriptName
    $where = if ($file) { Split-Path -Leaf $file } else { 'line' }
    return "$($where):$($record.InvocationInfo.ScriptLineNumber)"
}

function Invoke-Round([string]$name, [scriptblock]$body) {
    Log ''
    Log $name
    $started = Get-Date
    try {
        & $body
    } catch {
        Fail "$($name): $($_.Exception.Message) at $(Get-ErrorPlace $_)"
    }
    Log ('  ({0:N0} s)' -f ((Get-Date) - $started).TotalSeconds)
}

function Quote-Argument([string]$argument) {
    if ($argument -eq '' -or $argument -match '[\s"]') { return '"' + ($argument -replace '"', '\"') + '"' }
    return $argument
}

# --- The probe (the oracle) ---------------------------------------------------------------------

# One line for a probe call's result.
function Format-Probe($json) {
    if (-not $json) { return 'no output' }
    $parts = @("ok=$($json.ok)")
    foreach ($name in 'hr', 'error_code', 'extended_error', 'is_registered', 'elapsed_ms') {
        if ($null -ne $json.$name -and "$($json.$name)" -ne '') { $parts += "$name=$($json.$name)" }
    }
    $text = $parts -join ' '
    if ($json.error_text) { $text += " text='" + ($json.error_text -replace '\s+', ' ').Trim() + "'" }
    if ($json.message) { $text += " message='" + ($json.message -replace '\s+', ' ').Trim() + "'" }
    return $text
}

# Runs a probe call that changes something, as the current user (elevated); logs it and keeps its
# output for results\apps.json. Returns its JSON, or $null.
function Invoke-Probe([string[]]$arguments, [string]$label, [int]$timeoutSeconds = 300) {
    $script:callNumber++
    $tag = '{0:00}-probe-{1}' -f $script:callNumber, $label
    $out = Join-Path $outDir "$tag.json"
    $err = Join-Path $outDir "$tag.err"
    $line = ($arguments | ForEach-Object { Quote-Argument $_ }) -join ' '
    $process = Start-Process $probe $line -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $null = $process.Handle
    if (-not $process.WaitForExit($timeoutSeconds * 1000)) {
        & taskkill.exe /T /F /PID $process.Id | Out-Null
        Log "  probe $($label): timed out after $timeoutSeconds s"
        return $null
    }
    $text = Get-Content $out -Raw -Encoding utf8
    $raw[$tag] = @{ command = $line; exit = $process.ExitCode; stdout = $text }
    $json = $null
    if ($text) { try { $json = $text | ConvertFrom-Json } catch { } }
    Log "  probe $label [$($arguments[0])]: $(Format-Probe $json)"
    return $json
}

# A probe call that only reads; returns its JSON.
function Get-Probe([string[]]$arguments) {
    $output = & $probe @arguments
    if ($LASTEXITCODE -ne 0) { throw "avel_package_probe $($arguments -join ' ') failed ($LASTEXITCODE)" }
    return ($output -join "`n") | ConvertFrom-Json
}

# Who the probe says has a package: the SIDs whose state is installed (not SYSTEM's).
function Get-InstalledSids($package) {
    @(@($package.users) | Where-Object { $_.sid -and $_.state -eq 'installed' -and $_.sid -ne 'S-1-5-18' } |
        ForEach-Object { "$($_.sid)" } | Sort-Object -Unique)
}

function Get-Who([string]$sid) {
    if ($sid -eq $currentSid) { return 'current' }
    if ($sid -eq $otherSid) { return $otherUser }
    return $sid
}

# A family's state, as the probe sees it: its packages on this PC (any user), who has it, whether
# it is provisioned, and whether a package of it is staged (its folder in WindowsApps).
function Get-FamilyState([string]$family) {
    $name = ($family -split '_')[0]
    $packages = @((Get-Probe @('list', '--all-users', '--filter', $name)).packages | Where-Object { $_.family_name -eq $family })
    $users = @($packages | ForEach-Object { Get-InstalledSids $_ } | Sort-Object -Unique)
    $provisioned = @((Get-Probe @('provisioned', '--filter', $name)).packages | Where-Object { $_.family_name -eq $family })
    $staged = @($packages | Where-Object { Test-Path -LiteralPath "$windowsApps\$($_.full_name)" }).Count -gt 0
    $text = 'packages=[{0}] users=[{1}] provisioned={2} staged={3}' -f (($packages | ForEach-Object { $_.full_name }) -join ','),
        (($users | ForEach-Object { Get-Who $_ }) -join ','), $(if ($provisioned) { $provisioned[0].full_name } else { 'no' }), $staged
    return [pscustomobject]@{ family = $family; packages = @($packages | ForEach-Object { $_.full_name }); users = $users
                              current = $users -contains $currentSid; other = [bool]($otherSid -and $users -contains $otherSid)
                              provisioned = [bool]$provisioned.Count; staged = $staged; text = $text }
}

# Checks a family's state against the probe; returns it.
function Test-FamilyState($package, [string]$label, [bool]$current, [bool]$other, [bool]$provisioned, [bool]$staged) {
    $state = Get-FamilyState $package.family
    $bad = @()
    if ($state.current -ne $current) { $bad += "current user $(if ($state.current) { 'has it' } else { 'lacks it' })" }
    if ($state.other -ne $other) { $bad += "$otherUser $(if ($state.other) { 'has it' } else { 'lacks it' })" }
    if ($state.provisioned -ne $provisioned) { $bad += $(if ($state.provisioned) { 'provisioned' } else { 'not provisioned' }) }
    if ($state.staged -ne $staged) { $bad += $(if ($state.staged) { 'staged' } else { 'not staged' }) }
    $others = @($state.users | Where-Object { $_ -ne $currentSid -and $_ -ne $otherSid })
    if ($others) { $bad += "registered for $($others -join ', ')" }
    if ($bad) { Fail "$($label): $($package.name): $($bad -join '; ') ($($state.text))" }
    else { Log "  OK: $label ($($package.name): $($state.text))" }
    return $state
}

# Every package of this PC but the test packages: who has it and whether it is provisioned. A
# System-signed package's registration for AvelOther is left out: their first logon registers
# Windows' own packages for them, which may still be going on.
function Get-PackageSnapshot {
    $skip = @($test, $test2 | Where-Object { $_ } | ForEach-Object { $_.family })
    $list = Get-Probe @('list', '--all-users')
    $provisioned = Get-Probe @('provisioned')
    $snapshot = [ordered]@{}
    foreach ($package in @($list.packages) | Sort-Object full_name) {
        if ($package.family_name -in $skip) { continue }
        $system = "$($package.signature_kind)" -eq 'system'
        $users = @(@($package.users) | Where-Object { $_.sid -and -not ($system -and $_.sid -eq $otherSid) } |
            ForEach-Object { "$(Get-Who $_.sid):$($_.state)" } | Sort-Object) -join ','
        $snapshot["package $($package.full_name)"] = $users
    }
    foreach ($package in @($provisioned.packages) | Sort-Object full_name) {
        if ($package.family_name -notin $skip) { $snapshot["provisioned $($package.full_name)"] = 'yes' }
    }
    return $snapshot
}

function Compare-PackageSnapshot($expected, [string]$label) {
    $actual = Get-PackageSnapshot
    $differences = @()
    foreach ($key in @($expected.Keys) + @($actual.Keys) | Sort-Object -Unique) {
        $left = if ($expected.Contains($key)) { $expected[$key] } else { '<absent>' }
        $right = if ($actual.Contains($key)) { $actual[$key] } else { '<absent>' }
        if ($left -ne $right) { $differences += "$key : '$left' -> '$right'" }
    }
    if ($differences) {
        Fail "$($label): $($differences.Count) other package(s) changed"
        foreach ($line in $differences | Select-Object -First 10) { Log "      $line" }
    } else {
        Log "  OK: $label ($($expected.Count) other packages and provisionings as before)"
    }
}

# --- AvelOther ----------------------------------------------------------------------------------

function New-OtherUser {
    New-LocalUser -Name $otherUser -Password (ConvertTo-SecureString $otherPassword -AsPlainText -Force) `
        -AccountNeverExpires -PasswordNeverExpires | Out-Null
    $script:otherSid = (New-Object Security.Principal.NTAccount($otherUser)).Translate(
        [Security.Principal.SecurityIdentifier]).Value
    # Scheduled tasks log on as a batch job: Performance Log Users hold that right on client SKUs.
    Add-LocalGroupMember -SID 'S-1-5-32-559' -Member $otherUser
    $path = New-Object Text.StringBuilder 260
    $hr = [AvelApps.UserEnv]::CreateProfile($otherSid, $otherUser, $path, 260)
    if ($hr -ne 0) { throw ('CreateProfile for {0} returned 0x{1:X8}' -f $otherUser, $hr) }
    Log "  user $otherUser ($otherSid), profile $($path.ToString()) (CreateProfile, never signed in)"
}

# Runs the probe as AvelOther through a scheduled task with their password (the 6b spike's way:
# an S4U task can't be registered for them). Returns its JSON, or $null.
function Invoke-ProbeAsOther([string[]]$arguments, [string]$label, [int]$timeoutSeconds = 300) {
    $script:callNumber++
    $tag = '{0:00}-other-{1}' -f $script:callNumber, $label
    $out = Join-Path $outDir "$tag.json"
    $err = Join-Path $outDir "$tag.err"
    $done = Join-Path $outDir "$tag.done"
    $cmdFile = Join-Path $outDir "$tag.cmd"
    $line = (@("`"$probe`"") + ($arguments | ForEach-Object { Quote-Argument $_ })) -join ' '
    [IO.File]::WriteAllText($cmdFile, "@echo off`r`n$line > `"$out`" 2> `"$err`"`r`necho %ERRORLEVEL%> `"$done`"`r`n",
                            [Text.Encoding]::ASCII)
    $taskName = "p$tag"
    $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/d /c `"$cmdFile`""
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
    $started = Get-Date
    Register-ScheduledTask -TaskPath '\AvelApps\' -TaskName $taskName -Action $action -Settings $settings `
        -User "$env:COMPUTERNAME\$otherUser" -Password $otherPassword -RunLevel Limited -Force | Out-Null
    try {
        Start-ScheduledTask -TaskPath '\AvelApps\' -TaskName $taskName
        $deadline = $started.AddSeconds($timeoutSeconds)
        while (-not (Test-Path $done) -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 500
            $info = Get-ScheduledTaskInfo -TaskPath '\AvelApps\' -TaskName $taskName
            $task = Get-ScheduledTask -TaskPath '\AvelApps\' -TaskName $taskName
            # 0x41301 running, 0x41303 not yet run
            if ($task.State -eq 'Ready' -and $info.LastTaskResult -notin 0x41301, 0x41303 -and
                ((Get-Date) - $started).TotalSeconds -gt 5) {
                Start-Sleep -Seconds 1
                if (-not (Test-Path $done)) { Log ('  as {0} {1}: the task ended with 0x{2:X8}' -f $otherUser, $label, $info.LastTaskResult); break }
            }
        }
    } finally {
        Unregister-ScheduledTask -TaskPath '\AvelApps\' -TaskName $taskName -Confirm:$false
    }
    $text = if (Test-Path $out) { Get-Content $out -Raw -Encoding utf8 } else { '' }
    $raw[$tag] = @{ user = $otherUser; command = $line; stdout = $text }
    $json = $null
    if ($text) { try { $json = $text | ConvertFrom-Json } catch { } }
    Log ('  probe as {0} {1} [{2}]: {3} ({4:N1} s)' -f $otherUser, $label, $arguments[0], (Format-Probe $json),
         ((Get-Date) - $started).TotalSeconds)
    return $json
}

# --- avel-cli -----------------------------------------------------------------------------------

# avel-cli apps <arguments> --json; checks the exit code. Returns its JSON.
function Invoke-Apps([string[]]$arguments, [string]$label, [int]$expectExit = 0) {
    $script:callNumber++
    $run = Invoke-Cli (@('apps') + $arguments + @('--json')) ('{0:00}-{1}' -f $script:callNumber, $label)
    if ($run.exit -ne $expectExit) { Fail "$($label): exit $($run.exit), not $expectExit" }
    if (-not $run.json) { throw "$($label): no JSON output" }
    return $run.json
}

function Get-AppsList([string]$label, [switch]$All) {
    $arguments = @('list')
    if ($All) { $arguments += '--all' }
    $list = Invoke-Apps $arguments $label
    if (-not $list.elevated -or -not $list.all_users) { Fail "$($label): elevated $($list.elevated), all_users $($list.all_users)" }
    if ($list.error -ne 0) { Fail "$($label): the listing was cut short ($($list.error))" }
    return $list
}

function Find-App($list, [string]$family) {
    $found = @($list.apps | Where-Object { $_.family -eq $family })
    if ($found.Count -gt 1) { Fail "$family listed $($found.Count) times" }
    if ($found.Count) { return $found[0] }
    return $null
}

function Format-App($app) {
    $users = (@($app.users) | ForEach-Object { Get-Who $_.sid }) -join ','
    $modes = @('remove', 'deprovision', 'remove_everywhere' | Where-Object { $app.modes.$_ }) -join ','
    return '{0} {1} ({2}) {3} users=[{4}] provisioned={5} category={6} protection={7} signature={8} soft_undoable={9} modes=[{10}]' -f
        $app.id, $app.display_name, $app.family, $app.version, $users, $app.provisioned, $app.category, $app.protection,
        $app.signature, $app.soft_undoable, $modes
}

# Checks how `apps list` shows a test package: who has it (as the probe says), whether it is
# provisioned, and its modes.
function Test-ListedApp($list, $package, [string]$label, [bool]$provisioned, [bool]$softUndoable, [bool]$remove,
                        [bool]$deprovision, [bool]$everywhere) {
    $app = Find-App $list $package.family
    if (-not $app) { Fail "$($label): $($package.name) isn't listed"; return $null }
    $state = Get-FamilyState $package.family
    $bad = @()
    $listed = @(@($app.users) | ForEach-Object { "$($_.sid)" } | Sort-Object)
    if (($listed -join ',') -ne (@($state.users) -join ',')) { $bad += "users [$(($listed | ForEach-Object { Get-Who $_ }) -join ',')], the probe says [$(($state.users | ForEach-Object { Get-Who $_ }) -join ',')]" }
    foreach ($user in @($app.users)) {
        if ([bool]$user.current -ne ($user.sid -eq $currentSid)) { $bad += "user $($user.sid) current=$($user.current)" }
        if ($user.sid -eq $otherSid -and "$($user.account_name)" -notmatch "(^|\\)$otherUser$") { $bad += "account name '$($user.account_name)'" }
    }
    if ([bool]$app.provisioned -ne $provisioned) { $bad += "provisioned $($app.provisioned)" }
    if ($provisioned -and $app.provisioned_full_name -ne $package.full) { $bad += "provisioned as '$($app.provisioned_full_name)'" }
    if ($app.protection -ne 'none') { $bad += "protection $($app.protection)" }
    if ($app.category -ne 'other') { $bad += "category $($app.category)" }
    if ([bool]$app.soft_undoable -ne $softUndoable) { $bad += "soft_undoable $($app.soft_undoable)" }
    if ([bool]$app.modes.remove -ne $remove -or [bool]$app.modes.deprovision -ne $deprovision -or
        [bool]$app.modes.remove_everywhere -ne $everywhere) {
        $bad += "modes remove=$($app.modes.remove) deprovision=$($app.modes.deprovision) remove_everywhere=$($app.modes.remove_everywhere)"
    }
    if (@($app.versions | Where-Object { $_.full_name -eq $package.full }).Count -ne 1) { $bad += "versions $(@($app.versions | ForEach-Object { $_.full_name }) -join ',')" }
    if ($bad) { Fail "$($label): $($package.name): $($bad -join '; ')"; Log "      $(Format-App $app)" }
    else { Log "  OK: $label ($(Format-App $app))" }
    return $app
}

# Whether an issue names `id` (an app's id or family, as given or as the planner reports it).
function Test-IssueFor($issue, [string]$id) {
    if ("$($issue.id)" -eq $id) { return $true }
    foreach ($package in @($test, $test2, $script:protectedApp) | Where-Object { $_ }) {
        $names = @($package.id, $package.family)
        if ($id -in $names -and "$($issue.id)" -in $names) { return $true }
    }
    return $false
}

# An action refused: exactly the `expected` issues (id = code), no plan, no change set.
function Test-Refused($json, [hashtable]$expected, [string]$label) {
    $issues = @($json.issues)
    $bad = @()
    foreach ($entry in $expected.GetEnumerator()) {
        $found = @($issues | Where-Object { Test-IssueFor $_ $entry.Key })
        if ($found.Count -ne 1 -or $found[0].code -ne $entry.Value) {
            $bad += "$($entry.Key): $(@($found | ForEach-Object { $_.code }) -join ',') not $($entry.Value)"
        }
    }
    if ($issues.Count -ne $expected.Count) { $bad += "issues $(@($issues | ForEach-Object { "$($_.id)=$($_.code)" }) -join ', ')" }
    if (@($json.plan).Count) { $bad += "a plan of $(@($json.plan).Count) item(s)" }
    if ($json.change) { $bad += "a change set ($($json.change.set_id))" }
    if ($bad) { Fail "$($label): $($bad -join '; ')" }
    else { Log "  OK: $label refused ($(@($issues | ForEach-Object { $_.code }) -join ', '))" }
}

# Every item of an action's change set is done, of `kind`; returns the set's id.
function Test-ChangeDone($json, [string]$label, [string]$kind, [int]$count = 1) {
    $change = $json.change
    if (-not $change -or -not $change.set_id) { Fail "$($label): no change set"; return $null }
    Log "  change set $($change.set_id), $(@($change.items).Count) item(s): $($json.description)"
    foreach ($item in $change.items) {
        $location = $item.location
        Log ('    {0,-4} {1,-10} {2,-21} {3}' -f $item.index, $item.state, $location.kind, (Where-Text $location))
        if ($item.state -ne 'done') { Fail "$($label): $(Where-Text $location) is $($item.state) ($($item.reason))" }
        if ($location.kind -ne $kind) { Fail "$($label): an item of kind $($location.kind), not $kind" }
        if ($kind -eq 'package-user' -and ($location.user_sid -ne $currentSid -or -not $location.before -or $location.after)) {
            Fail "$($label): the item is for $($location.user_sid), $($location.before) -> $($location.after)"
        }
        if ($kind -eq 'package-provisioning' -and (-not $location.before -or $location.after)) {
            Fail "$($label): the item goes $($location.before) -> $($location.after)"
        }
        if ($kind -eq 'package' -and -not $location.all_users) { Fail "$($label): a package item not for every user" }
    }
    if (@($change.items).Count -ne $count) { Fail "$($label): $(@($change.items).Count) item(s), not $count" }
    return $change.set_id
}

function Invoke-Undo([string]$setId, [string]$label) {
    if (-not $setId) { return }
    $script:callNumber++
    $run = Invoke-Cli @('undo', $setId, '--json') ('{0:00}-{1}' -f $script:callNumber, $label)
    $items = @($run.json.items | ForEach-Object { "$($_.index)=$($_.state)$(if ($_.reason -ne 'none') { " ($($_.reason))" })" }) -join ', '
    if ($run.json.state -ne 'restored' -or $run.exit -ne 0) { Fail "$($label): set $setId is $($run.json.state) (exit $($run.exit)): $items" }
    else { Log "  OK: $label ($setId restored: $items)" }
}

# The change sets in the store (undo --list, which also settles interrupted ones).
function Get-ChangeSets([string]$label) {
    $script:callNumber++
    $run = Invoke-Cli @('undo', '--list', '--json') ('{0:00}-{1}' -f $script:callNumber, $label)
    if ($run.exit -ne 0) { throw "undo --list returned $($run.exit)" }
    return @(@($run.json) | Where-Object { $_ })
}

# The one set made since `before` (a list of sets), or $null.
function Get-NewSet($before, [string]$label) {
    $known = @($before | ForEach-Object { $_.id })
    $new = @(Get-ChangeSets "$label-sets" | Where-Object { $_.id -notin $known })
    if ($new.Count -ne 1) { Fail "$($label): $($new.Count) new change set(s), not 1"; return $null }
    Log "  change set $($new[0].id) '$($new[0].description)': $($new[0].state), $($new[0].items) item(s), $($new[0].done) done"
    return $new[0]
}

# undo --show: the set with its items.
function Get-SetDetails([string]$setId, [string]$label) {
    $script:callNumber++
    $run = Invoke-Cli @('undo', '--show', $setId, '--json') ('{0:00}-{1}' -f $script:callNumber, $label)
    if ($run.exit -ne 0 -or -not $run.json) { throw "undo --show $setId returned $($run.exit)" }
    foreach ($item in @($run.json.items)) {
        Log ('    {0,-4} {1,-10} {2,-12} {3,-21} {4}' -f $item.index, $item.state, $item.reason, $item.location.kind, (Where-Text $item.location))
    }
    return $run.json
}

# --- Rounds -------------------------------------------------------------------------------------

function Get-PackageRecord([string]$fullName) {
    $parts = $fullName -split '_'
    return [pscustomobject]@{ full = $fullName; family = $parts[0] + '_' + $parts[-1]; name = $parts[0]; id = $null; display = $null }
}

function Invoke-FixtureRound {
    foreach ($file in $testMsix, $testMsix2) {
        if (-not (Test-Path $file)) { throw "no $file; run Get-SampleInstallers.ps1" }
    }
    New-Item -ItemType Directory -Force $work, $probeDir, $outDir | Out-Null
    if (-not (Test-Path "$bin\avel_package_probe.exe")) { throw 'avel_package_probe.exe is not in the build output' }
    # A copy AvelOther can run (the mapped build folder is the current user's).
    Copy-Item "$bin\avel_package_probe.exe" $probeDir -Force
    Get-ChildItem $bin -Filter '*.dll' | Where-Object { $_.Name -match '^(msvcp|vcruntime|concrt)' } |
        ForEach-Object { Copy-Item $_.FullName $probeDir -Force }
    & icacls.exe $work /grant '*S-1-5-32-545:(OI)(CI)M' /T /Q | Out-Null
    $online = $false
    try { $online = Test-Connection 1.1.1.1 -Count 1 -Quiet -ErrorAction Stop } catch { }
    Log "  network: $(if ($online) { 'ONLINE (expected offline: Start-AvelSandbox.ps1 starts apps without networking)' } else { 'offline' })"

    New-OtherUser
    # Their first logon registers Windows' own packages for them; it happens here, before any snapshot.
    $whoami = Invoke-ProbeAsOther @('whoami') 'whoami'
    if (-not $whoami -or -not $whoami.ok) { throw "the probe doesn't run as $otherUser" }
    Log "    ran as $($whoami.user) $($whoami.sid), elevated=$($whoami.elevated)"

    # The test package: staged, provisioned (which registers it for the caller too), and registered
    # for AvelOther by a task running as them.
    $stage = Invoke-Probe @('add', $testMsix, '--allow-unsigned', '--stage-only') 'stage-test'
    if (-not $stage -or -not $stage.ok) { throw 'staging the test package failed' }
    $full = @((Get-Probe @('list', '--all-users', '--filter', 'Avel.TestPackage_')).packages)[0].full_name
    if (-not $full) { throw 'the staged test package is not listed' }
    $script:test = Get-PackageRecord $full
    $provision = Invoke-Probe @('provision', $test.family) 'provision-test'
    if (-not $provision -or -not $provision.ok) { throw 'provisioning the test package failed' }
    if (-not (Get-FamilyState $test.family).current) {
        $register = Invoke-Probe @('register-family', $test.family) 'register-current'
        if (-not $register -or -not $register.ok) { throw 'registering the test package for the current user failed' }
    }
    $other = Invoke-ProbeAsOther @('register-family', $test.family) 'register-other'
    if (-not $other -or -not $other.ok) {
        $other = Invoke-ProbeAsOther @('register-full', $test.full, '--allow-unsigned') 'register-other-full'
    }
    $null = Test-FamilyState $test 'the test package' $true $true $true $true

    # The second one: the current user's alone.
    $add = Invoke-Probe @('add', $testMsix2, '--allow-unsigned') 'add-test2'
    if (-not $add -or -not $add.ok) { throw 'adding the second test package failed' }
    $full2 = @((Get-Probe @('list', '--all-users', '--filter', 'Avel.TestPackage2_')).packages)[0].full_name
    if (-not $full2) { throw 'the second test package is not listed' }
    $script:test2 = Get-PackageRecord $full2
    $null = Test-FamilyState $test2 'the second test package' $true $false $false $true

    $script:baseline = Get-PackageSnapshot
    $edge = @($baseline.Keys | Where-Object { $_ -like '*Microsoft.MicrosoftEdge.Stable_*' })
    Log "  $($baseline.Count) other packages and provisionings; Edge's: $(($edge | ForEach-Object { "$_ [$($baseline[$_])]" }) -join '; ')"
}

function Invoke-ListRound {
    $all = Get-AppsList 'list-all' -All
    $protected = @($all.apps | Where-Object { $_.protection -ne 'none' })
    Log "  $(@($all.apps).Count) apps listed in $($all.elapsed_ms) ms, $($protected.Count) protected ($((@($protected | Group-Object protection) | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', '))"
    foreach ($app in @($all.apps | Where-Object { $_.protection -eq 'none' })) { Log "    $(Format-App $app)" }

    # The image's System-signed main packages: each a protected app with no mode. Frameworks and
    # resource packages aren't apps.
    $packages = @((Get-Probe @('list', '--all-users')).packages)
    $main = @($packages | Where-Object { $_.is_framework -ne $true -and $_.is_resource -ne $true })
    $systemFamilies = @($main | Where-Object { "$($_.signature_kind)" -eq 'system' } | ForEach-Object { $_.family_name } | Sort-Object -Unique)
    $frameworkFamilies = @($packages | Where-Object { $_.is_framework -eq $true -or $_.is_resource -eq $true } |
        ForEach-Object { $_.family_name } | Where-Object { $_ -notin @($main | ForEach-Object { $_.family_name }) } | Sort-Object -Unique)
    if (-not $systemFamilies.Count) { Fail 'the probe lists no System-signed package' }
    $bad = $failures
    foreach ($family in $systemFamilies) {
        $app = Find-App $all $family
        if (-not $app) { Fail "System-signed $family isn't listed"; continue }
        if ($app.protection -eq 'none') { Fail "System-signed $family isn't protected" }
        $modes = @('remove', 'deprovision', 'remove_everywhere' | Where-Object { $app.modes.$_ })
        if ($modes) { Fail "System-signed $family offers $($modes -join ', ')" }
    }
    foreach ($family in $frameworkFamilies) { if (Find-App $all $family) { Fail "framework or resource family $family is listed" } }
    if ($failures -eq $bad) {
        Log "  OK: the $($systemFamilies.Count) System-signed families listed, protected ($((@($systemFamilies | ForEach-Object { (Find-App $all $_).protection }) | Group-Object | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', ')), with no mode; $($frameworkFamilies.Count) framework families left out"
    }
    $edge = @($all.apps | Where-Object { $_.family -like 'Microsoft.MicrosoftEdge.Stable_*' })
    foreach ($app in $edge) { Log "  Edge's package (never changed here): $(Format-App $app)" }

    $app = Test-ListedApp $all $test 'the test package' $true $true $true $true $true
    if ($app) { $test.id = $app.id; $test.display = $app.display_name }
    $app = Test-ListedApp $all $test2 'the second test package' $false $false $false $false $true
    if ($app) { $test2.id = $app.id; $test2.display = $app.display_name }

    $mine = Get-AppsList 'list'
    $shown = @($mine.apps | Where-Object { $_.protection -ne 'none' })
    if ($shown) { Fail "list without --all shows $($shown.Count) protected app(s)" }
    foreach ($package in $test, $test2) {
        $found = Find-App $mine $package.family
        if (-not $found -or $found.id -ne $package.id) { Fail "list without --all: $($package.name) $(if ($found) { "id $($found.id)" } else { 'missing' })" }
    }
    if (-not $shown) { Log "  OK: without --all: $(@($mine.apps).Count) apps, none protected, the test packages with the same ids" }

    # Refusals of a protected app.
    $candidates = @($all.apps | Where-Object { $_.protection -eq 'system' })
    $pick = @($candidates | Where-Object { @($_.users | Where-Object { $_.current }).Count } | Select-Object -First 1)
    if (-not $pick) { $pick = @($candidates | Select-Object -First 1) }
    if (-not $pick) { Fail 'no System-signed app to refuse'; return }
    $script:protectedApp = [pscustomobject]@{ id = $pick[0].id; family = $pick[0].family; name = $pick[0].name }
    Log "  protected app for the refusals: $(Format-App $pick[0])"
    $before = Get-PackageSnapshot
    $testBefore = (Get-FamilyState $test.family).text
    $id = $protectedApp.id
    Test-Refused (Invoke-Apps @('remove', $id, '--yes') 'refuse-protected-remove' 1) @{ $id = 'protected' } 'remove of a protected app'
    Test-Refused (Invoke-Apps @('deprovision', $id, '--yes') 'refuse-protected-deprovision' 1) @{ $id = 'protected' } 'deprovision of a protected app'
    Test-Refused (Invoke-Apps @('remove-everywhere', $id, '--allow-irreversible', '--yes') 'refuse-protected-everywhere' 1) `
        @{ $id = 'protected' } 'remove-everywhere --allow-irreversible of a protected app'
    Test-Refused (Invoke-Apps @('remove', $protectedApp.family, '--yes') 'refuse-protected-family' 1) @{ $protectedApp.family = 'protected' } 'remove of a protected app by its family name'
    Test-Refused (Invoke-Apps @('remove', 'ffffffffff', '--yes') 'refuse-unknown' 1) @{ 'ffffffffff' = 'unknown-id' } 'remove of an unknown id'
    Test-Refused (Invoke-Apps @('remove', $test.id, $id, '--yes') 'refuse-mixed' 1) @{ $id = 'protected' } 'remove of the test package with a protected app'
    Compare-PackageSnapshot $before 'the refusals changed no package'
    $testAfter = (Get-FamilyState $test.family).text
    if ($testAfter -ne $testBefore) { Fail "the refusals changed the test package: $testBefore -> $testAfter" }
    else { Log '  OK: the test package as before' }
}

function Invoke-RemoveForMeRound {
    $json = Invoke-Apps @('remove', $test.family, '--yes') 'remove-for-me'
    if (@($json.apps).Count -ne 1 -or $json.apps[0].id -ne $test.id) { Fail "remove by family name: apps $(@($json.apps | ForEach-Object { $_.id }) -join ',')" }
    $set = Test-ChangeDone $json 'remove for me' 'package-user'
    $null = Test-FamilyState $test 'removed for the current user, still for AvelOther and new users' $false $true $true $true
    $list = Get-AppsList 'list-removed-for-me'
    $null = Test-ListedApp $list $test 'listed for AvelOther and new users' $true $true $false $true $true
    Invoke-Undo $set 'undo remove for me'
    $null = Test-FamilyState $test 'registered again for the current user' $true $true $true $true
    $list = Get-AppsList 'list-undone-remove'
    $null = Test-ListedApp $list $test 'listed as before' $true $true $true $true $true
    Compare-PackageSnapshot $baseline 'nothing else changed'
}

function Invoke-DeprovisionRound {
    $json = Invoke-Apps @('deprovision', $test.id, '--yes') 'deprovision'
    $set = Test-ChangeDone $json 'deprovision' 'package-provisioning'
    $null = Test-FamilyState $test 'no longer provisioned, both users keep it' $true $true $false $true
    $list = Get-AppsList 'list-deprovisioned'
    $null = Test-ListedApp $list $test 'listed as not provisioned' $false $true $true $false $true
    Invoke-Undo $set 'undo deprovision'
    $null = Test-FamilyState $test 'provisioned again, both users keep it' $true $true $true $true
    $list = Get-AppsList 'list-undone-deprovision'
    $null = Test-ListedApp $list $test 'listed as before' $true $true $true $true $true
    Compare-PackageSnapshot $baseline 'nothing else changed'
}

function Invoke-RefusalRound {
    $before = Get-PackageSnapshot
    $states = @($test, $test2 | ForEach-Object { (Get-FamilyState $_.family).text })
    $sets = Get-ChangeSets 'refusals-before'
    Test-Refused (Invoke-Apps @('remove', $test2.id, '--yes') 'refuse-would-delete' 1) @{ $test2.id = 'would-delete-package' } `
        'remove for me of a package nobody else has, not provisioned'
    Test-Refused (Invoke-Apps @('deprovision', $test2.id, '--yes') 'refuse-not-provisioned' 1) @{ $test2.id = 'not-provisioned' } `
        'deprovision of a package not provisioned'
    Test-Refused (Invoke-Apps @('remove-everywhere', $test.id, '--yes') 'refuse-irreversible' 1) `
        @{ $test.id = 'irreversible-needs-confirmation' } 'remove-everywhere without --allow-irreversible'
    Test-Refused (Invoke-Apps @('remove', $test.id, $test2.id, '--yes') 'refuse-mixed-apps' 1) @{ $test2.id = 'would-delete-package' } `
        'remove for me of a fitting app with a refused one'
    $dry = Invoke-Apps @('remove', $test.id, '--dry-run') 'dry-run-remove'
    if (-not $dry.dry_run -or @($dry.plan).Count -ne 1 -or $dry.plan[0].kind -ne 'package-user' -or $dry.change -or @($dry.issues).Count) {
        Fail "dry run of remove: plan $(@($dry.plan | ForEach-Object { $_.kind }) -join ','), change $($dry.change.set_id), $(@($dry.issues).Count) issue(s)"
    } else { Log "  OK: a dry run of remove plans one package-user item ($($dry.description))" }
    $dry = Invoke-Apps @('remove-everywhere', $test.id, $test2.id, '--allow-irreversible', '--dry-run') 'dry-run-everywhere'
    $kinds = @($dry.plan | ForEach-Object { "$($_.kind)$(if ($_.all_users) { '(all users)' })" })
    if (@($dry.plan).Count -ne 2 -or @($dry.plan | Where-Object { $_.kind -ne 'package' -or -not $_.all_users }).Count -or $dry.change -or @($dry.issues).Count) {
        Fail "dry run of remove-everywhere: plan $($kinds -join ', '), change $($dry.change.set_id), $(@($dry.issues).Count) issue(s)"
    } else { Log "  OK: a dry run of remove-everywhere plans $($kinds -join ', ') ($($dry.description))" }
    $new = @(Get-ChangeSets 'refusals-after' | Where-Object { $_.id -notin @($sets | ForEach-Object { $_.id }) })
    if ($new) { Fail "the refusals and dry runs made $($new.Count) change set(s)" }
    Compare-PackageSnapshot $before 'the refusals and dry runs changed no other package'
    $after = @($test, $test2 | ForEach-Object { (Get-FamilyState $_.family).text })
    for ($i = 0; $i -lt 2; $i++) {
        if ($after[$i] -ne $states[$i]) { Fail "the refusals changed a test package: $($states[$i]) -> $($after[$i])" }
    }
    if ($after[0] -eq $states[0] -and $after[1] -eq $states[1]) { Log '  OK: both test packages as before' }
}

# Remove for me, stopped by AVEL_TEST_FAULT_AFTER after `records` journal records; `between` runs
# before the next start. Returns the set, settled.
function Invoke-Crash([int]$records, [string]$label, [scriptblock]$between = $null) {
    $sets = Get-ChangeSets "$label-before"
    $script:callNumber++
    $env:AVEL_TEST_FAULT_AFTER = "$records"
    try { $crash = Invoke-Cli @('apps', 'remove', $test.id, '--yes', '--json') ('{0:00}-{1}' -f $script:callNumber, $label) }
    finally { Remove-Item Env:\AVEL_TEST_FAULT_AFTER }
    if ($crash.exit -ne 99) { Fail "$($label): avel-cli exited with $($crash.exit), not 99" }
    else { Log "  avel-cli stopped after $records journal record(s) (exit 99)" }
    if ($between) { & $between }
    $set = Get-NewSet $sets $label  # undo --list opens the store, which settles the set
    if (-not $set) { return $null }
    if ($set.state -ne 'complete') { Fail "$($label): the interrupted set is $($set.state) after the next start" }
    return $set
}

function Invoke-CrashRound {
    Log '  5a: stopped right after the backup, before Windows was asked'
    $set = Invoke-Crash 1 'crash-after-backup'
    if ($set) {
        $show = Get-SetDetails $set.id 'show-crash-after-backup'
        $item = @($show.items)[0]
        if ($item.state -ne 'failed' -or $item.reason -ne 'interrupted') { Fail "5a: the item is $($item.state) ($($item.reason)), not failed (interrupted)" }
        else { Log '  OK: settled as failed (interrupted)' }
    }
    $null = Test-FamilyState $test '5a: nothing changed' $true $true $true $true
    if ($set) {
        $script:callNumber++
        $undo = Invoke-Cli @('undo', $set.id, '--json') ('{0:00}-undo-crash-after-backup' -f $script:callNumber)
        Log "  undo of that set: $($undo.json.state), exit $($undo.exit), $(@($undo.json.items).Count) item(s)"
        $null = Test-FamilyState $test '5a: its undo changed nothing' $true $true $true $true
    }

    Log '  5b: stopped right after the backup, and the registration removed before the next start (as if Windows had done it)'
    $set = Invoke-Crash 1 'crash-then-removed' {
        $null = Invoke-Probe @('remove', $test.full) 'remove-behind'
        $null = Test-FamilyState $test '5b: removed for the current user behind the set''s back' $false $true $true $true
    }
    if ($set) {
        $show = Get-SetDetails $set.id 'show-crash-then-removed'
        $item = @($show.items)[0]
        if ($item.state -ne 'done') { Fail "5b: the item is $($item.state) ($($item.reason)), not done" }
        else { Log '  OK: settled as done, by looking' }
        Invoke-Undo $set.id '5b: undo'
    }
    $null = Test-FamilyState $test '5b: registered again for the current user' $true $true $true $true

    Log '  5c: stopped right after the item was done, before the set was finished'
    $set = Invoke-Crash 2 'crash-after-done'
    $null = Test-FamilyState $test '5c: removed for the current user' $false $true $true $true
    if ($set) {
        $show = Get-SetDetails $set.id 'show-crash-after-done'
        $item = @($show.items)[0]
        if ($item.state -ne 'done') { Fail "5c: the item is $($item.state) ($($item.reason)), not done" }
        else { Log '  OK: settled, the item done' }
        Invoke-Undo $set.id '5c: undo'
    }
    $null = Test-FamilyState $test '5c: registered again for the current user' $true $true $true $true
    Compare-PackageSnapshot $baseline 'nothing else changed'
}

# The program list's entry of a package family for the current user.
function Find-Program([string]$family, [string]$label) {
    $script:callNumber++
    $run = Invoke-Cli @('list', '--json') ('{0:00}-{1}' -f $script:callNumber, $label)
    if ($run.exit -ne 0 -or -not $run.json) { throw "avel-cli list returned $($run.exit)" }
    $found = @($run.json.programs | Where-Object { $_.package_family_name -eq $family })
    foreach ($program in $found) {
        Log "    program $($program.id) '$($program.name)' $($program.package_full_name) user '$($program.user_name)' ($($program.user_sid)) source $($program.source)"
    }
    $mine = @($found | Where-Object { $_.user_sid -eq $currentSid })
    if (-not $mine) { $mine = @($found | Where-Object { -not $_.user_sid }) }
    if (-not $mine) { Fail "$($label): the program list has no entry of $family for the current user ($($found.Count) of others)"; return $null }
    return $mine[0]
}

function Invoke-Uninstall($program, [string]$label) {
    $script:callNumber++
    $run = Invoke-Cli @('uninstall', "`"$($program.id)`"", '--quiet', '--no-restore-point', '--no-leftover-scan', '--yes', '--json') `
        ('{0:00}-{1}' -f $script:callNumber, $label) 600
    if (-not $run.json) { throw "$($label): no JSON output (exit $($run.exit))" }
    $result = @($run.json.results)[0]
    Log ("  uninstall '{0}': {1}, method {2}, change set {3}, exit {4}, error {5}" -f $result.name, $result.outcome,
         $result.plan.method, $(if ($result.change_set) { $result.change_set } else { 'none' }), $run.exit, $result.error)
    if ($result.outcome -ne 'removed') { Fail "$($label): outcome $($result.outcome)" }
    if ($result.plan.method -ne 'package') { Fail "$($label): method $($result.plan.method)" }
    return $result
}

function Invoke-UninstallRound {
    Log '  6a: the test package (AvelOther and new users keep it): a soft removal, undoable'
    $program = Find-Program $test.family 'programs'
    if ($program) {
        $result = Invoke-Uninstall $program 'uninstall-test'
        if (-not $result.change_set) { Fail '6a: no change set reported (results[].change_set)' }
        $null = Test-FamilyState $test '6a: removed for the current user only' $false $true $true $true
        if ($result.change_set) {
            $show = Get-SetDetails $result.change_set 'show-uninstall'
            Log "  set '$($show.description)': $($show.state)"
            $items = @($show.items)
            if ($items.Count -ne 1 -or $items[0].location.kind -ne 'package-user' -or $items[0].state -ne 'done' -or
                $items[0].location.user_sid -ne $currentSid) {
                Fail "6a: the set holds $(@($items | ForEach-Object { "$($_.location.kind) $($_.state)" }) -join ', ')"
            }
            Invoke-Undo $result.change_set '6a: undo'
        }
        $null = Test-FamilyState $test '6a: registered again for the current user' $true $true $true $true
    }

    Log '  6b: the second test package (nobody else keeps it): removed as before, no change set'
    $program = Find-Program $test2.family 'programs-2'
    if ($program) {
        $result = Invoke-Uninstall $program 'uninstall-test2'
        if ($result.change_set) { Fail "6b: a change set was reported ($($result.change_set))" }
        $null = Test-FamilyState $test2 '6b: gone from this PC' $false $false $false $false
        $state = Get-FamilyState $test2.family
        if ($state.packages) { Fail "6b: still listed by the package manager: $($state.packages -join ', ')" }
    }
    Compare-PackageSnapshot $baseline 'nothing else changed'
}

function Invoke-EverywhereRound {
    $json = Invoke-Apps @('remove-everywhere', $test.id, '--allow-irreversible', '--yes') 'remove-everywhere'
    $set = Test-ChangeDone $json 'remove for everyone' 'package'
    $state = Test-FamilyState $test 'gone from this PC: nobody has it, not provisioned, not staged' $false $false $false $false
    if ($state.packages) { Fail "still listed by the package manager: $($state.packages -join ', ')" }
    $list = Get-AppsList 'list-after-everywhere' -All
    if (Find-App $list $test.family) { Fail 'apps list --all still lists the test package' }
    else { Log '  OK: apps list --all no longer lists it' }
    if ($set) {
        $show = Get-SetDetails $set 'show-everywhere'
        Log "  set '$($show.description)': $($show.state)"
    }
    Compare-PackageSnapshot $baseline 'Windows'' own packages and Edge''s as after the fixtures'
}

try {
    Log "Avel Phase 6b apps scenario, $(Get-Date -Format s)"
    Log "OS: $([Environment]::OSVersion.VersionString), user: $env:USERNAME ($currentSid)"
    Log "avel-cli: $((& $cli version) -join ' ')"
    Invoke-Round 'Fixtures: AvelOther, the test packages' { Invoke-FixtureRound }
    if ($test -and $test2 -and $baseline) {
        Invoke-Round 'Round 1: list, and refusals of a protected app' { Invoke-ListRound }
        if (-not $test.id -or -not $test2.id) {
            Fail 'the test packages are not listed; the rounds that change them are skipped'
        } else {
            Invoke-Round 'Round 2: Remove for me, then undo' { Invoke-RemoveForMeRound }
            Invoke-Round 'Round 3: Don''t install for new users, then undo' { Invoke-DeprovisionRound }
            Invoke-Round 'Round 4: refusals and dry runs change nothing' { Invoke-RefusalRound }
            Invoke-Round 'Round 5: crash recovery' { Invoke-CrashRound }
            Invoke-Round 'Round 6: avel-cli uninstall of packaged apps' { Invoke-UninstallRound }
            . "$PSScriptRoot\apps_gui_round.ps1"
            Invoke-Round 'Round 7: GUI (Tools > Windows apps)' { Invoke-AppsGuiRound }
            Invoke-Round 'Round 8: remove for everyone' { Invoke-EverywhereRound }
        }
    }
} catch {
    Fail "scenario error: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
    Write-Report
}
