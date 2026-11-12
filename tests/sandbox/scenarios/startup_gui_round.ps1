# Dot-sourced by the startup scenario, which runs Invoke-StartupGuiRound last: Phase 6a's GUI round,
# the Startup window (Tools > Startup...) driven in English through UI Automation (gui.ps1) by its
# object names (docs/tools.md, "GUI"), on the fixtures as rounds 1-6 leave them (all there, enabled;
# ids in $fixtures). Each step is judged with avel-cli (startup list --all-users, undo --list), the
# state dumps and the window itself (its State column and its status bar's result):
#   1. The window lists every user's entries, and the signed-out user's hive is free again once it
#      has. The search "Contoso" shows exactly the fixtures; Windows' own row, selected alone,
#      leaves Enable, Disable and Delete disabled.
#   2. Two Run values selected together (the machine's and the current user's), then Disable: both
#      disabled as Task Manager does, in one new change set of two items.
#   3. Undo last change: both enabled again (their StartupApproved values gone again), the set
#      restored.
#   4. A service: Disable sets its start to Manual (listed as disabled by Avel); Enable starts it
#      automatically again.
#   5. A Startup folder shortcut: Delete asks first (startupDeleteConfirmation); confirmed, the file
#      is gone; Undo last change puts it back.
#   6. The state dump equals the one taken before the round, and avel.exe closes (the Startup
#      window, then the main window) and exits with 0.
# Expects startup.ps1's helpers (Get-StartupList, Get-Entry, Test-States...) and startup_fixture.ps1.

. "$PSScriptRoot\gui.ps1"

$ellipsis = [string][char]0x2026
$startupGuiProcess = $null

if (-not ('AvelStartupKeys' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class AvelStartupKeys {
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void Down(byte key) { keybd_event(key, 0, 0, UIntPtr.Zero); }
    public static void Up(byte key) { keybd_event(key, 0, 2, UIntPtr.Zero); }
}
'@
}

function Get-StartupWindow([int]$seconds = 30) { Find-Window '^Startup$' $seconds 'startupWindow' }

function Get-StartupLabel($window, [string]$id) {
    try { return Get-Text (ById $window $id 2 $CT::Text) } catch { return '' }
}

# Waits until the window is idle with a listing: Refresh enabled, no work under way, the count shown.
# Returns the count's text.
function Wait-StartupIdle($window, [int]$seconds = 180) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ($true) {
        try {
            $refresh = ById $window 'startupRefreshButton' 2
            $progress = Get-StartupLabel $window 'startupProgress'
            $count = Get-StartupLabel $window 'startupCount'
            if ($refresh.Current.IsEnabled -and -not $progress -and $count -match '[0-9]') { return $count }
        } catch { }
        if ((Get-Date) -gt $deadline) { throw "the Startup window isn't idle after $seconds s: $(Texts $window)" }
        Start-Sleep -Milliseconds 400
    }
}

# Waits for the status bar's result to read `pattern` (and differ from `previous`), then for the
# listing that follows a change. Returns the result.
function Wait-StartupResult($window, [string]$pattern, [string]$previous, [int]$seconds = 120) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $text = ''
    while ($true) {
        $text = Get-StartupLabel $window 'startupResult'
        if ($text -ne $previous -and $text -match $pattern) { break }
        if ((Get-Date) -gt $deadline) { throw "the result reads '$text', not /$pattern/, after $seconds s" }
        Start-Sleep -Milliseconds 300
    }
    $null = Wait-StartupIdle $window
    Log "  window: $text"
    return $text
}

# The table's rows, through its Grid pattern: name, state, source and user, and the name's cell.
function Get-StartupRows($window) {
    $table = ById $window 'startupTable' 10
    $grid = $table.GetCurrentPattern([System.Windows.Automation.GridPattern]::Pattern)
    $rows = [System.Collections.Generic.List[object]]::new()
    for ($row = 0; $row -lt $grid.Current.RowCount; $row++) {
        $cells = @(for ($column = 0; $column -lt 4; $column++) { Get-GridCell $grid $row $column })
        $texts = @($cells | ForEach-Object { if ($_) { "$($_.Current.Name)" } else { '' } })
        $rows.Add([pscustomobject]@{ name = $texts[0]; state = $texts[1]; source = $texts[2]; user = $texts[3]; cell = $cells[0] })
    }
    return $rows.ToArray()
}

function Test-RowSelected($row) {
    try { return [bool]$row.cell.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected }
    catch { return $false }
}

# The names of the selected rows.
function Get-SelectedNames($window) { @(Get-StartupRows $window | Where-Object { Test-RowSelected $_ } | ForEach-Object { $_.name }) }

# Selects the rows named `names`, and only them: through their cells' SelectionItem pattern; if
# that doesn't select them, by clicks (Ctrl+click for the second and later).
function Select-StartupRows($window, [string[]]$names) {
    $want = @($names | Sort-Object)
    $find = {
        param($name)
        $found = @(Get-StartupRows $window | Where-Object { $_.name -eq $name })
        if ($found.Count -ne 1) { throw "the table shows $($found.Count) row(s) named '$name'" }
        return $found[0]
    }
    for ($i = 0; $i -lt $names.Count; $i++) {
        $item = (& $find $names[$i]).cell.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
        if ($i -eq 0) { $item.Select() } else { $item.AddToSelection() }
        Start-Sleep -Milliseconds 200
    }
    Start-Sleep -Milliseconds 500
    $selected = @(Get-SelectedNames $window | Sort-Object)
    if (-not (Compare-Object $selected $want)) { return }
    Log "    UI Automation selected '$($selected -join "', '")'; clicking the rows instead"
    try { $window.SetFocus() } catch { }
    for ($i = 0; $i -lt $names.Count; $i++) {
        $cell = (& $find $names[$i]).cell
        try { $cell.GetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern).ScrollIntoView() } catch { }
        $rect = (& $find $names[$i]).cell.Current.BoundingRectangle
        if ($rect.IsEmpty) { throw "the row '$($names[$i])' has no place on screen" }
        if ($i -gt 0) { [AvelStartupKeys]::Down(0x11) }  # Ctrl
        try { [AvelInput]::Click([int]($rect.Left + [Math]::Min(60, $rect.Width / 2)), [int]($rect.Top + $rect.Height / 2)) }
        finally { if ($i -gt 0) { [AvelStartupKeys]::Up(0x11) } }
        Start-Sleep -Milliseconds 300
    }
    Start-Sleep -Milliseconds 500
    $selected = @(Get-SelectedNames $window | Sort-Object)
    if (Compare-Object $selected $want) { throw "selected '$($selected -join "', '")', not '$($want -join "', '")'" }
}

function Get-StartupButton($window, [string]$name) { ById $window "startup$($name)Button" 10 }

function Test-RowStates($window, $names, [string]$state, [string]$label) {
    $rows = @(Get-StartupRows $window)
    $bad = $failures
    foreach ($name in $names) {
        $row = @($rows | Where-Object { $_.name -eq $name })
        if ($row.Count -ne 1) { Fail "$($label): the table shows $($row.Count) row(s) named '$name'" }
        elseif ($row[0].state -ne $state) { Fail "$($label): '$name' reads '$($row[0].state)' in the State column, not '$state'" }
    }
    if ($failures -eq $bad) { Log "  OK: $label (the State column reads '$state')" }
}

# The change sets in the store, as undo --list --json gives them.
function Get-ChangeSets([string]$label) {
    $script:callNumber++
    $run = Invoke-Cli @('undo', '--list', '--json') ('{0:00}-{1}' -f $script:callNumber, $label)
    if ($run.exit -ne 0) { throw "undo --list returned $($run.exit)" }
    return @(@($run.json) | Where-Object { $_ })
}

# The one set made since `before` (a list of sets); fails unless there is exactly one, of `items`
# items.
function Get-NewSet($before, [string]$label, [int]$items) {
    $known = @($before | ForEach-Object { $_.id })
    $new = @(Get-ChangeSets "$label-sets" | Where-Object { $_.id -notin $known })
    if ($new.Count -ne 1) { Fail "$($label): $($new.Count) new change set(s), not 1"; return $null }
    Log "  change set $($new[0].id) '$($new[0].description)': $($new[0].state), $($new[0].items) item(s), $($new[0].done) done"
    if ($new[0].items -ne $items -or $new[0].done -ne $items) { Fail "$($label): the set has $($new[0].items) item(s), $($new[0].done) done, not $items" }
    return $new[0]
}

function Test-SetState($setId, [string]$state, [string]$label) {
    $set = @(Get-ChangeSets "$label-sets" | Where-Object { $_.id -eq $setId })[0]
    if (-not $set) { Fail "$($label): set $setId is gone" }
    elseif ($set.state -ne $state) { Fail "$($label): set $setId is $($set.state), not $state" }
    else { Log "  OK: $label (set $setId $state)" }
}

# One step of the round; what it throws fails it, and the round goes on.
function Invoke-StartupGuiStep([string]$name, [scriptblock]$body) {
    Log "  -- $name"
    try { & $body }
    catch { Fail "gui, $($name): $($_.Exception.Message) at $(Split-Path -Leaf $_.InvocationInfo.ScriptName):$($_.InvocationInfo.ScriptLineNumber)" }
}

function Invoke-StartupGuiRound {
    # UI Automation's rectangles and the mouse in the same pixels, before any UI Automation call.
    [AvelInput]::UsePhysicalPixels()
    $before = Get-StartupDump $allSpecs
    $list = Get-StartupList 'gui-list' -AllUsers
    # The names the table shows: a service's display name, else the entry's name.
    $names = @{}
    foreach ($fixture in $fixtures) {
        $entry = Get-Entry $list $fixture 'gui list'
        if ($entry) { $names[$fixture.label] = if ($entry.display_name) { "$($entry.display_name)" } else { "$($entry.name)" } }
    }
    $script:startupGuiProcess = Start-AvelGui
    $null = $startupGuiProcess.Handle  # Without this, ExitCode stays empty once the process has exited
    try {
        $window = $null
        Invoke-StartupGuiStep '1: Tools > Startup..., the list, the search, Windows'' own row' {
            $null = Window 'Avel' 60
            Open-Menu 'Tools' ('Startup' + $ellipsis)
            $script:startupWindowElement = Get-StartupWindow
            $count = Wait-StartupIdle $startupWindowElement
            Log "  window: $count; $(Get-StartupLabel $startupWindowElement 'startupWarnings')"
            Test-OtherHiveFree 'the window listed every user'
            Set-Text (ById $startupWindowElement 'startupSearch' 10) 'Contoso'
            Start-Sleep -Milliseconds 800
            $rows = @(Get-StartupRows $startupWindowElement)
            $shown = @($rows | ForEach-Object { $_.name } | Sort-Object)
            $expected = @($names.Values | Sort-Object)
            if (Compare-Object $shown $expected) {
                Fail "gui search: the table shows '$($shown -join "', '")', not the fixtures '$($expected -join "', '")'"
            } else {
                Log "  OK: the search shows the $($rows.Count) fixtures ($(Get-StartupLabel $startupWindowElement 'startupCount'))"
            }
            foreach ($row in $rows) { if ($row.state -ne 'Enabled') { Fail "gui search: '$($row.name)' reads '$($row.state)'" } }
            Select-StartupRows $startupWindowElement @($names['windows-own'])
            $enabled = @('Enable', 'Disable', 'Delete' | Where-Object { (Get-StartupButton $startupWindowElement $_).Current.IsEnabled })
            if ($enabled) { Fail "gui: with Windows' own entry selected, $($enabled -join ', ') enabled" }
            else { Log '  OK: Windows'' own entry selected: Enable, Disable and Delete disabled' }
        }
        $window = $script:startupWindowElement
        if (-not $window) { throw 'the Startup window did not open' }

        $runs = Get-Fixtures 'run-machine', 'run-user'
        $runNames = @($runs | ForEach-Object { $names[$_.label] })
        $runSet = $null
        Invoke-StartupGuiStep '2: two Run values selected, Disable' {
            Select-StartupRows $window $runNames
            $sets = Get-ChangeSets 'gui-disable-before'
            $previous = Get-StartupLabel $window 'startupResult'
            Click (Get-StartupButton $window 'Disable')
            $null = Wait-StartupResult $window '^2 entries disabled\.' $previous
            Test-RowStates $window $runNames 'Disabled' 'gui disable'
            Test-States (Get-StartupList 'gui-list-disabled' -AllUsers) $runs 'disabled' 'gui disable'
            $set = Get-NewSet $sets 'gui disable' 2
            if ($set) { $script:startupGuiRunSet = $set.id }
            $selected = @(Get-SelectedNames $window | Sort-Object)
            if (Compare-Object $selected @($runNames | Sort-Object)) { Fail "gui disable: selected after the refresh: '$($selected -join "', '")'" }
        }

        Invoke-StartupGuiStep '3: Undo last change' {
            $previous = Get-StartupLabel $window 'startupResult'
            Click (Get-StartupButton $window 'Undo')
            $null = Wait-StartupResult $window '^Undone: Disable 2 startup entries\.' $previous
            Test-RowStates $window $runNames 'Enabled' 'gui undo'
            $listed = Get-StartupList 'gui-list-undone' -AllUsers
            foreach ($fixture in $runs) {
                $entry = Get-Entry $listed $fixture 'gui undo'
                if ($entry -and $entry.state -ne 'enabled') { Fail "gui undo: $($fixture.label) listed $($entry.state)" }
            }
            $specs = Get-FixtureSpecs $runs
            $expected = [ordered]@{}
            foreach ($spec in $specs) { $expected[$spec] = $before[$spec] }
            Compare-State $expected (Get-StartupDump $specs) 'gui undo: both Run values and their StartupApproved values as before'
            if ($script:startupGuiRunSet) { Test-SetState $startupGuiRunSet 'restored' 'gui undo' }
        }

        $service = Get-Fixtures 'service'
        $serviceName = $names['service']
        Invoke-StartupGuiStep '4: a service, Disable then Enable' {
            Select-StartupRows $window @($serviceName)
            $sets = Get-ChangeSets 'gui-service-before'
            $previous = Get-StartupLabel $window 'startupResult'
            Click (Get-StartupButton $window 'Disable')
            $previous = Wait-StartupResult $window '^1 entry disabled\.' $previous
            Test-RowStates $window @($serviceName) 'Disabled' 'gui service disable'
            Test-States (Get-StartupList 'gui-list-service-disabled' -AllUsers) $service 'disabled' 'gui service disable: start Manual'
            $null = Get-NewSet $sets 'gui service disable' 1
            Click (Get-StartupButton $window 'Enable')
            $null = Wait-StartupResult $window '^1 entry enabled\.' $previous
            Test-RowStates $window @($serviceName) 'Enabled' 'gui service enable'
            Test-States (Get-StartupList 'gui-list-service-enabled' -AllUsers) $service 'enabled' 'gui service enable: start Automatic'
        }

        $folder = Get-Fixtures 'folder-user'
        $folderName = $names['folder-user']
        $folderFile = "$userStartup\Contoso User.lnk"
        Invoke-StartupGuiStep '5: a Startup folder shortcut, Delete, then Undo last change' {
            Select-StartupRows $window @($folderName)
            $sets = Get-ChangeSets 'gui-delete-before'
            $previous = Get-StartupLabel $window 'startupResult'
            Click (Get-StartupButton $window 'Delete')
            $box = Find-Window '' 20 'startupDeleteConfirmation'
            Log "  confirmation '$($box.Current.Name)': $(Texts $box)"
            if (-not (Test-Path -LiteralPath $folderFile)) { Fail 'gui delete: the file is gone before the confirmation' }
            Click (ById $box 'startupConfirmDeleteButton' 10)
            $previous = Wait-StartupResult $window '^1 entry deleted\.' $previous
            if (Test-Path -LiteralPath $folderFile) { Fail "gui delete: $folderFile is still there" }
            if (@(Get-StartupRows $window | Where-Object { $_.name -eq $folderName }).Count) { Fail "gui delete: the table still shows '$folderName'" }
            if (@(Find-Entries (Get-StartupList 'gui-list-deleted' -AllUsers) $folder[0]).Count) { Fail 'gui delete: still listed by avel-cli' }
            $set = Get-NewSet $sets 'gui delete' 1
            Click (Get-StartupButton $window 'Undo')
            $null = Wait-StartupResult $window ('^Undone: Delete startup entry ' + [regex]::Escape($folderName) + '\.') $previous
            if (-not (Test-Path -LiteralPath $folderFile)) { Fail "gui undo delete: $folderFile isn't back" }
            Test-RowStates $window @($folderName) 'Enabled' 'gui undo delete'
            $found = @(Find-Entries (Get-StartupList 'gui-list-undo-deleted' -AllUsers) $folder[0])
            if ($found.Count -ne 1 -or $found[0].id -ne $folder[0].id -or $found[0].state -ne 'enabled') {
                Fail "gui undo delete: listed $($found.Count) time(s), $($found[0].state), id $($found[0].id)"
            }
            if ($set) { Test-SetState $set.id 'restored' 'gui undo delete' }
        }

        Invoke-StartupGuiStep '6: avel.exe closes' {
            $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
            $deadline = (Get-Date).AddSeconds(20)
            $open = $true
            while ($open -and (Get-Date) -lt $deadline) {
                try { $null = Get-StartupWindow 0 } catch { $open = $false }
                if ($open) { Start-Sleep -Milliseconds 300 }
            }
            if ($open) { Fail 'gui: the Startup window is still open after its Close' }
            (Window 'Avel' 10).GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
            if (-not $startupGuiProcess.WaitForExit(30000)) { Fail 'gui: avel.exe is still running 30 s after its main window closed' }
            elseif ($startupGuiProcess.ExitCode -ne 0) { Fail "gui: avel.exe exited with $($startupGuiProcess.ExitCode)" }
            else { Log '  OK: the Startup window and the main window closed, avel.exe exited with 0' }
        }
    } finally {
        if (-not $startupGuiProcess.HasExited) {
            Stop-Process -Id $startupGuiProcess.Id -Force -ErrorAction SilentlyContinue
            $null = $startupGuiProcess.WaitForExit(15000)
        }
    }
    Test-OtherHiveFree 'after avel.exe'
    Compare-State $before (Get-StartupDump $allSpecs) 'the GUI round gave back every entry exactly'
}
