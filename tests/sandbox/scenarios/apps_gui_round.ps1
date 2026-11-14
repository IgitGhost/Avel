# Dot-sourced by the apps scenario, which runs Invoke-AppsGuiRound before its last round: Phase 6b's
# GUI round, the Windows apps window (Tools > Windows apps...) driven in English through UI
# Automation (gui.ps1) by its object names (docs/tools.md, "GUI (Tools > Windows apps...)"), on the
# test package as rounds 1-6 leave it (the current user's and AvelOther's, provisioned). Each step is
# judged with the probe (Test-FamilyState), avel-cli (undo --list) and the window itself (its
# status bar's result, its count, the Installed for column):
#   1. The window lists the apps (every user's: the test package reads "You and 1 other user, new
#      users").
#   2. Check often removed checks exactly the unprotected often-removed apps avel-cli lists (none in
#      the Sandbox's image: "No other often removed app to check."); Uncheck all clears them.
#   3. The search (the test package's family name) shows its row alone; its check box gets checked
#      (UI Automation's Toggle, else a click on it, else the keyboard), and the count says 1 checked.
#   4. Remove for me: one new change set of one item; the probe: the current user's registration
#      gone, AvelOther's and the provisioning kept; the row reads "1 other user, new users".
#   5. Undo last change: the set restored, the registration back.
#   6. Remove for everyone...: the confirmation (removeForEveryoneConfirmation) opens with its
#      button disabled; checking "I understand this can't be undone" enables it, unchecking disables
#      it again; Cancel closes it and nothing changes.
#   7. avel.exe closes (the window, then the main window) and exits with 0.
# Expects apps.ps1's helpers (Test-FamilyState, Get-ChangeSets, Get-NewSet, Get-AppsList...).

. "$PSScriptRoot\gui.ps1"

$ellipsis = [string][char]0x2026

function Get-AppsWindow([int]$seconds = 30) { Find-Window '^Windows apps$' $seconds 'windowsAppsWindow' }

function Get-AppsLabel($window, [string]$id) {
    try { return Get-Text (ById $window $id 2 $CT::Text) } catch { return '' }
}

# Waits until the window is idle with a listing: Refresh enabled, no work under way, the count
# shown. Returns the count's text.
function Wait-AppsIdle($window, [int]$seconds = 120) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ($true) {
        try {
            $refresh = ById $window 'windowsAppsRefreshButton' 2
            $progress = Get-AppsLabel $window 'windowsAppsProgress'
            $count = Get-AppsLabel $window 'windowsAppsCount'
            if ($refresh.Current.IsEnabled -and -not $progress -and $count -match '[0-9]') { return $count }
        } catch { }
        if ((Get-Date) -gt $deadline) { throw "the Windows apps window isn't idle after $seconds s: $(Texts $window)" }
        Start-Sleep -Milliseconds 400
    }
}

# Waits for the status bar's result to read `pattern` (and differ from `previous`), then for the
# listing that follows a change. Returns the result.
function Wait-AppsResult($window, [string]$pattern, [string]$previous, [int]$seconds = 120) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $text = ''
    while ($true) {
        $text = Get-AppsLabel $window 'windowsAppsResult'
        if ($text -ne $previous -and $text -match $pattern) { break }
        if ((Get-Date) -gt $deadline) { throw "the result reads '$text', not /$pattern/, after $seconds s" }
        Start-Sleep -Milliseconds 300
    }
    $null = Wait-AppsIdle $window
    Log "  window: $text"
    return $text
}

# The table's rows, through its Grid pattern: the check cell, name and Installed for.
function Get-AppsRows($window) {
    $table = ById $window 'windowsAppsTable' 10
    $grid = $table.GetCurrentPattern([System.Windows.Automation.GridPattern]::Pattern)
    $rows = [System.Collections.Generic.List[object]]::new()
    for ($row = 0; $row -lt $grid.Current.RowCount; $row++) {
        $check = Get-GridCell $grid $row 0
        $name = Get-GridCell $grid $row 1
        $users = Get-GridCell $grid $row 4
        $rows.Add([pscustomobject]@{ check = $check; nameCell = $name
                                     checkName = $(if ($check) { "$($check.Current.Name)" } else { '' })
                                     name = $(if ($name) { "$($name.Current.Name)" } else { '' })
                                     users = $(if ($users) { "$($users.Current.Name)" } else { '' }) })
    }
    return $rows.ToArray()
}

# The one row named `name` (the table filtered by the search).
function Get-AppsRow($window, [string]$name) {
    $found = @(Get-AppsRows $window | Where-Object { $_.name -eq $name })
    if ($found.Count -ne 1) { throw "the table shows $($found.Count) row(s) named '$name': $(@(Get-AppsRows $window | ForEach-Object { $_.name }) -join ', ')" }
    return $found[0]
}

# How many apps are checked, from the count ("..., 1 checked").
function Get-CheckedCount($window) {
    if ((Get-AppsLabel $window 'windowsAppsCount') -match '([0-9]+) checked$') { return [int]$Matches[1] }
    return -1
}

function Get-AppsButton($window, [string]$id) { ById $window $id 10 }

# Checks the row named `name`, and says how; throws when nothing did. Qt's item views take checks
# from real input (README: "The GUI rounds"), so: the cell's Toggle pattern, in case it does; else
# clicks across its check box; else the keyboard (a click on the row's name, Home to its check
# cell, Space).
function Set-AppChecked($window, [string]$name) {
    $before = Get-CheckedCount $window
    $changed = { (Get-CheckedCount $window) -gt $before }
    $row = Get-AppsRow $window $name
    if ($row.checkName -ne $name) { Fail "gui: the check box cell is named '$($row.checkName)', not '$name'" }
    try {
        $toggle = $row.check.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
        Log "    the check cell's Toggle pattern says $($toggle.Current.ToggleState); toggling"
        $toggle.Toggle()
        Start-Sleep -Seconds 1
        if (& $changed) { return 'its Toggle pattern' }
    } catch {
        Log "    the check cell has no Toggle pattern ($($_.Exception.GetType().Name))"
    }
    try { $window.SetFocus() } catch { }
    $rect = (Get-AppsRow $window $name).check.Current.BoundingRectangle
    if (-not $rect.IsEmpty) {
        $scale = [Math]::Max(1.0, $rect.Height / 20.0)  # Rows are about 20 pixels high at 100 %
        foreach ($offset in 15, 10, 6, 20) {
            [AvelInput]::Click([int]($rect.Left + [Math]::Min($offset * $scale, $rect.Width - 2)), [int]($rect.Top + $rect.Height / 2))
            Start-Sleep -Milliseconds 800  # Longer than a double click
            if (& $changed) { return "a click on its check box ($offset px into the cell at $([int]$rect.Left),$([int]$rect.Top), $([int]$rect.Width)x$([int]$rect.Height))" }
        }
        Log "    clicks on the check cell changed nothing (at $([int]$rect.Left),$([int]$rect.Top), $([int]$rect.Width)x$([int]$rect.Height))"
    }
    $rect = (Get-AppsRow $window $name).nameCell.Current.BoundingRectangle
    if (-not $rect.IsEmpty) {
        [AvelInput]::Click([int]($rect.Left + [Math]::Min(60, $rect.Width / 2)), [int]($rect.Top + $rect.Height / 2))
        Start-Sleep -Milliseconds 500
        [AvelInput]::Key(0x24, $true)  # Home: the row's first cell
        Start-Sleep -Milliseconds 300
        [AvelInput]::Key(0x20, $false)  # Space
        Start-Sleep -Seconds 1
        if (& $changed) { return 'the keyboard (a click on its name, Home, Space)' }
    }
    throw "nothing checked '$name' (the count reads '$(Get-AppsLabel $window 'windowsAppsCount')')"
}

# One step of the round; what it throws fails it, and the round goes on.
# Its parameter isn't $name: the steps' blocks see the caller's variables, and $name is the app's.
function Invoke-AppsGuiStep([string]$stepName, [scriptblock]$body) {
    Log "  -- $stepName"
    try { & $body }
    catch { Fail "gui, $($stepName): $($_.Exception.Message) at $(Get-ErrorPlace $_)" }
}

function Test-InstalledFor($window, [string]$name, [string]$expected, [string]$label) {
    $text = (Get-AppsRow $window $name).users
    if ($text -ne $expected) { Fail "$($label): Installed for reads '$text', not '$expected'" }
    else { Log "  OK: $label (Installed for reads '$text')" }
}

function Invoke-AppsGuiRound {
    # UI Automation's rectangles and the mouse in the same pixels, before any UI Automation call.
    [AvelInput]::UsePhysicalPixels()
    $list = Get-AppsList 'gui-list'
    $oftenRemoved = @($list.apps | Where-Object { $_.category -eq 'often-removed' -and $_.protection -eq 'none' })
    $name = $test.display
    if (-not $name) { throw 'the test package has no display name' }
    $process = Start-AvelGui
    $null = $process.Handle  # Without this, ExitCode stays empty once the process has exited
    try {
        $window = $null
        Invoke-AppsGuiStep '1: Tools > Windows apps...' {
            $null = Window 'Avel' 60
            Open-Menu 'Tools' ('Windows apps' + $ellipsis)
            $script:appsWindowElement = Get-AppsWindow
            $count = Wait-AppsIdle $appsWindowElement
            Log "  window: $count; $(Get-AppsLabel $appsWindowElement 'windowsAppsWarnings')"
            $banner = Get-AppsLabel $appsWindowElement 'windowsAppsBanner'
            if ($banner) { Fail "gui: the window shows a banner: $banner" }
        }
        $window = $script:appsWindowElement
        if (-not $window) { throw 'the Windows apps window did not open' }

        Invoke-AppsGuiStep "2: Check often removed ($($oftenRemoved.Count) unprotected often-removed app(s) listed)" {
            $previous = Get-AppsLabel $window 'windowsAppsResult'
            Click (Get-AppsButton $window 'checkOftenRemovedButton')
            if ($oftenRemoved.Count -eq 0) {
                $null = Wait-AppsResult $window '^No other often removed app to check\.' $previous 20
            } else {
                $null = Wait-AppsResult $window "^$($oftenRemoved.Count) often removed apps? checked\." $previous 20
            }
            $checked = Get-CheckedCount $window
            if ($checked -ne $oftenRemoved.Count) { Fail "gui: $checked app(s) checked, not $($oftenRemoved.Count)" }
            else { Log "  OK: $checked app(s) checked" }
            if ($checked -gt 0) {
                Click (Get-AppsButton $window 'uncheckAllButton')
                Start-Sleep -Seconds 1
                if ((Get-CheckedCount $window) -ne 0) { Fail "gui: Uncheck all left $(Get-CheckedCount $window) checked" }
            }
        }

        Invoke-AppsGuiStep "3: search the test package, check it" {
            Set-Text (ById $window 'windowsAppsSearch' 10) $test.family
            Start-Sleep -Milliseconds 800
            $rows = @(Get-AppsRows $window)
            if ($rows.Count -ne 1 -or $rows[0].name -ne $name) { Fail "gui search: the table shows '$(@($rows | ForEach-Object { $_.name }) -join "', '")', not '$name' alone" }
            Test-InstalledFor $window $name 'You and 1 other user, new users' 'gui list'
            $how = Set-AppChecked $window $name
            Log "  OK: checked by $how ($(Get-AppsLabel $window 'windowsAppsCount'))"
            if ((Get-CheckedCount $window) -ne 1) { Fail "gui: $(Get-CheckedCount $window) app(s) checked, not 1" }
        }

        $script:appsGuiSet = $null
        Invoke-AppsGuiStep '4: Remove for me' {
            $sets = Get-ChangeSets 'gui-remove-before'
            $previous = Get-AppsLabel $window 'windowsAppsResult'
            $button = Get-AppsButton $window 'removeForMeButton'
            if (-not $button.Current.IsEnabled) { throw 'Remove for me is disabled' }
            Click $button
            $null = Wait-AppsResult $window '^1 app removed for you\.' $previous
            $null = Test-FamilyState $test 'gui remove for me: removed for the current user' $false $true $true $true
            $set = Get-NewSet $sets 'gui remove for me'
            if ($set) {
                $script:appsGuiSet = $set.id
                if ($set.items -ne 1 -or $set.done -ne 1) { Fail "gui remove for me: the set has $($set.items) item(s), $($set.done) done" }
            }
            Test-InstalledFor $window $name '1 other user, new users' 'gui remove for me'
        }

        Invoke-AppsGuiStep '5: Undo last change' {
            $previous = Get-AppsLabel $window 'windowsAppsResult'
            $button = Get-AppsButton $window 'windowsAppsUndoButton'
            if (-not $button.Current.IsEnabled) { throw 'Undo last change is disabled' }
            Click $button
            $null = Wait-AppsResult $window '^Undone: ' $previous
            $null = Test-FamilyState $test 'gui undo: registered again for the current user' $true $true $true $true
            if ($script:appsGuiSet) {
                $set = @(Get-ChangeSets 'gui-undo-sets' | Where-Object { $_.id -eq $script:appsGuiSet })[0]
                if (-not $set -or $set.state -ne 'restored') { Fail "gui undo: set $script:appsGuiSet is $($set.state)" }
                else { Log "  OK: set $($set.id) restored" }
            }
            Test-InstalledFor $window $name 'You and 1 other user, new users' 'gui undo'
        }

        Invoke-AppsGuiStep '6: Remove for everyone... asks first, its button disabled until "I understand"; Cancel' {
            if ((Get-CheckedCount $window) -ne 1) {
                Log "    $(Get-CheckedCount $window) app(s) checked after the undo; checking the test package again"
                $null = Set-AppChecked $window $name
            }
            $sets = Get-ChangeSets 'gui-everywhere-before'
            $before = (Get-FamilyState $test.family).text
            Click (Get-AppsButton $window 'removeForEveryoneButton')
            $box = Find-Window '' 20 'removeForEveryoneConfirmation'
            Log "  confirmation '$($box.Current.Name)': $(Texts $box)"
            $confirm = ById $box 'removeForEveryoneConfirmButton' 10
            if ($confirm.Current.IsEnabled) { Fail 'gui: Remove for everyone is enabled before "I understand" is checked' }
            else { Log '  OK: its button is disabled' }
            $understand = ById $box 'removeForEveryoneUnderstandCheck' 10
            $toggle = $understand.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
            $toggle.Toggle()
            $deadline = (Get-Date).AddSeconds(5)
            while (-not (ById $box 'removeForEveryoneConfirmButton' 5).Current.IsEnabled -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
            if (-not (ById $box 'removeForEveryoneConfirmButton' 5).Current.IsEnabled) { Fail 'gui: checking "I understand" did not enable Remove for everyone' }
            else { Log '  OK: "I understand" checked: the button is enabled' }
            $toggle = (ById $box 'removeForEveryoneUnderstandCheck' 5).GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
            $toggle.Toggle()
            Start-Sleep -Milliseconds 500
            if ((ById $box 'removeForEveryoneConfirmButton' 5).Current.IsEnabled) { Fail 'gui: unchecking "I understand" left Remove for everyone enabled' }
            else { Log '  OK: unchecked again: the button is disabled' }
            Click (ById $box 'removeForEveryoneCancelButton' 5)
            $deadline = (Get-Date).AddSeconds(10)
            $open = $true
            while ($open -and (Get-Date) -lt $deadline) {
                try { $null = Find-Window '' 0 'removeForEveryoneConfirmation' } catch { $open = $false }
                if ($open) { Start-Sleep -Milliseconds 300 }
            }
            if ($open) { Fail 'gui: the confirmation is still open after Cancel' }
            Start-Sleep -Seconds 2
            $after = (Get-FamilyState $test.family).text
            if ($after -ne $before) { Fail "gui cancel: the test package changed: $before -> $after" }
            else { Log "  OK: cancelled, the test package as before ($after)" }
            $known = @($sets | ForEach-Object { $_.id })
            $new = @(Get-ChangeSets 'gui-everywhere-after' | Where-Object { $_.id -notin $known })
            if ($new) { Fail "gui cancel: $($new.Count) new change set(s)" }
        }

        Invoke-AppsGuiStep '7: avel.exe closes' {
            $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
            $deadline = (Get-Date).AddSeconds(20)
            $open = $true
            while ($open -and (Get-Date) -lt $deadline) {
                try { $null = Get-AppsWindow 0 } catch { $open = $false }
                if ($open) { Start-Sleep -Milliseconds 300 }
            }
            if ($open) { Fail 'gui: the Windows apps window is still open after its Close' }
            (Window 'Avel' 10).GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
            if (-not $process.WaitForExit(30000)) { Fail 'gui: avel.exe is still running 30 s after its main window closed' }
            elseif ($process.ExitCode -ne 0) { Fail "gui: avel.exe exited with $($process.ExitCode)" }
            else { Log '  OK: the Windows apps window and the main window closed, avel.exe exited with 0' }
        }
    } finally {
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $null = $process.WaitForExit(15000)
        }
    }
    $null = Test-FamilyState $test 'after the GUI round: as before it' $true $true $true $true
    Compare-PackageSnapshot $baseline 'nothing else changed'
}
