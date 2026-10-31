# Dot-sourced by the trace scenarios: Phase 5c's GUI round, which trace.ps1 runs last and
# trace_gui.ps1 runs alone. It drives avel.exe in English through UI Automation (gui.ps1) and judges
# what it did with the oracle (trace_oracle.ps1), as the CLI rounds are judged. Expects common.ps1,
# oracle.ps1, trace_fixture.ps1, trace_oracle.ps1 and gui.ps1 to be dot-sourced, and $bin,
# $installers, $results and $cli to be set.
#   1. Trace > Install and trace... with 7-Zip's MSI (/qn): the progress dialog runs, the trace stops
#      by itself and the review opens. 7-Zip's folder is among the items tied to it, the trace is
#      complete, linked to 7-Zip's entry (trace show), and complete against the oracle, captured
#      around it as in rounds 2-4.
#   2. Trace > Start tracing... around Contoso Trace Fixture's installer: the status bar says
#      "Tracing since ..."; its Stop tracing finishes the trace and opens the review. The trace is
#      linked to the fixture, complete against the oracle (captured inside it, as in round 1) and
#      reports each of the fixture's items. In the review, the fixture's task folder (\Contoso Trace,
#      which only the trace finds) is unchecked: trace show must mark it, and only it, "excluded".
#      The fixture, not 7-Zip, gets the exclusion: 7-Zip's own uninstaller removes all its items,
#      and an item the leftover scan finds by itself (a folder named after the program) is offered
#      by that scan whatever the trace's review says, so only an item that the trace alone finds and
#      that the uninstaller leaves shows that the review keeps it.
#   3. The program list's Trace column says "Traced" for both programs, and Trace > Traces... lists
#      both traces, the newest (the fixture's) selected.
#   4. The user adds their own Path entries; the fixture is uninstalled from the program list: the
#      uninstall dialog offers "Use the install trace", checked; the leftover review opens by itself
#      with the trace's items and value edits (all its rows, read through UI Automation's Grid
#      pattern), the excluded task folder not offered (or shown unchecked); Remove. Revert: the
#      oracle's diff between the capture before the install and the one after the removal holds
#      only the KNOWN set (as in round 1 (5b), with the exact Path, SharedDLLs and CONTOSO_HOME
#      checks) and the excluded task folder, which must still be there. avel-cli scan, run before,
#      tells Test-Revert what the GUI's scan offered; undo --show, what its removal did.
#   5. Tools > Undo Center... > Restore all gives back exactly what the removal took: state dumps,
#      and the oracle's diff between the captures before the removal and after the restore, with a
#      watch trace (avel-cli) around both telling Windows' own activity apart (Test-Undo).
#   6. Optional ($guiOptionalSteps; their checks count like the others):
#      a. revert: Trace > Traces... > Revert by trace... for the fixture's trace (Avel Force by its
#         id, the program being gone) removes what step 5 restored but the excluded task folder,
#         with the value edits; undone with avel-cli, state dumps compared;
#      b. startup: a trace started in the GUI and left open when avel.exe is ended: at the next
#         start Avel asks, and "Finish the trace" finishes it and opens its review.
# Every step that fails logs FAIL lines naming it ("gui <n>: ..."); one that throws is logged as a
# failure and the GUI is started afresh (an open trace cancelled) for the next.

$ellipsis = [string][char]0x2026
# The optional steps to run: 'revert' (6a), 'startup' (6b).
$guiOptionalSteps = @('revert', 'startup')
$guiFixtureDescription = 'Contoso Trace Fixture (GUI)'
# Only the trace finds it (traced-install-root alone), and the fixture's uninstaller leaves it.
$guiExcludedItem = @{ kind = 'task-folder'; where = (Split-Path $traceTask) }
# avel.exe's own settings (QSettings: the UI language, the uninstall dialog's choices), which it
# writes while this round runs; left out of the oracle's comparisons.
$guiSettingsPattern = '^HKU\\[^\\]+\\Software\\Avel(\\|\||$)'
# The review's group and kind rows (trace_model.cpp, leftover_text.cpp).
$reviewGroupPattern = '^(Tied to .+|Not tied to the program|Environment variables and shared DLLs|Changes to items that existed before, shown only|Noise: .+) \([0-9]+\)$'
$reviewKindPattern = '^(Files and folders|Shortcuts|Registry|Services|Scheduled tasks|Packages|Firewall rules \(shown only\)) \([0-9]+\)$'
$guiProcess = $null
$guiState = @{}

function Start-TraceGui {
    $script:guiProcess = Start-AvelGui
    $null = Window 'Avel' 60
}

function Stop-TraceGui {
    if ($script:guiProcess -and -not $script:guiProcess.HasExited) {
        Stop-Process -Id $script:guiProcess.Id -Force -ErrorAction SilentlyContinue
        $null = $script:guiProcess.WaitForExit(15000)
    }
}

# One step of the round. What it throws fails it; the GUI is then started afresh, without an open
# trace, for the next step.
function Invoke-GuiStep([string]$name, [scriptblock]$body) {
    Log ''
    Log $name
    try {
        & $body
    } catch {
        Fail "$($name): $($_.Exception.Message) at $(Split-Path -Leaf $_.InvocationInfo.ScriptName):$($_.InvocationInfo.ScriptLineNumber)"
        try {
            Stop-Hold
            Stop-TraceGui
            Close-OpenTrace
            Start-TraceGui
        } catch {
            Log "  cleanup failed: $($_.Exception.Message)"
        }
    }
}

# The ids of the traces in the store.
function Get-TraceIds { @(Get-TraceList | ForEach-Object { "$($_.id)" }) }

# The primary link of a trace as trace show prints it, or $null.
function Get-PrimaryLink($json) { @($json.trace.links | Where-Object { $_ -and $_.primary })[0] }

function Test-TaskFolder([string]$path) {
    $scheduler = New-Object -ComObject Schedule.Service
    $scheduler.Connect()
    try { $null = $scheduler.GetFolder($path); return $true } catch { return $false }
}

# The newest change set (undo --list), or $null.
function Get-NewestChangeSet([string]$label) {
    $list = Invoke-Avel @('undo', '--list', '--json') $label
    return @(@($list.json) | Where-Object { $_ } | Sort-Object created -Descending)[0]
}

# Uninstalls 7-Zip if a round before left it installed, so Install and trace installs it.
function Remove-SevenZip {
    $root = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'
    foreach ($key in @(Get-ChildItem $root -ErrorAction SilentlyContinue | Where-Object { "$($_.GetValue('DisplayName'))" -match '^7-Zip ' })) {
        Log "  7-Zip is installed ($($key.PSChildName)): uninstalling it first"
        if ($key.PSChildName -match '^\{[0-9A-Fa-f-]+\}$') {
            $null = Start-Process msiexec.exe "/x $($key.PSChildName) /qn /norestart" -Wait -PassThru
        }
    }
}

# --- The review ---------------------------------------------------------------------------------

# The review's counts below its tree ("17 items shown. Nothing is excluded."), once it has read its
# trace; '' before.
function Get-ReviewCounts($review) {
    $label = Find-Element $review 'traceCounts' $CT::Text '[0-9]+ item(s|\(s\))? shown\.' 2
    return Get-Text $label
}

# Waits until the review has read its trace. Returns its tree, its rows (first column) and its groups
# (@{ name; items }, the items as their first column reads, kinds' rows left out).
function Read-Review($review, [int]$seconds = 120) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $counts = ''
    while (-not $counts) {
        try { $counts = Get-ReviewCounts $review } catch { $counts = '' }
        if (-not $counts) {
            if ((Get-Date) -gt $deadline) { throw "the review didn't read its trace within $seconds s: $(Texts $review)" }
            Start-Sleep -Milliseconds 500
        }
    }
    Start-Sleep -Milliseconds 500
    $tree = Find-Element $review 'traceReviewTree' $null '^Traced items$' 10
    $rows = @(Get-ViewRows $tree 4)
    $groups = [System.Collections.Generic.List[object]]::new()
    foreach ($row in $rows) {
        $name = "$($row.Current.Name)"
        if ($name -match $reviewGroupPattern) {
            $groups.Add(@{ name = $name; items = [System.Collections.Generic.List[string]]::new() })
        } elseif ($groups.Count -and $name -notmatch $reviewKindPattern) {
            $groups[$groups.Count - 1].items.Add($name)
        }
    }
    $items = 0
    foreach ($group in $groups) { $items += $group.items.Count }
    $shown = if ($counts -match '^([0-9][0-9,.]*) item') { [int]($Matches[1] -replace '[^0-9]', '') } else { -1 }
    if ($shown -ge 0 -and $items -ne $shown) { Log "    read $items item row(s) of the $shown shown (kinds of more than 50 items are collapsed)" }
    return @{ tree = $tree; rows = $rows; groups = $groups.ToArray(); counts = $counts }
}

function Get-ReviewGroup($read, [string]$pattern) { @($read.groups | Where-Object { $_.name -match $pattern })[0] }

function Write-Review($read) {
    Log "  review: $($read.counts) $($read.rows.Count) row(s)"
    foreach ($group in $read.groups) {
        $shown = @($group.items | Select-Object -First 25)
        $more = if ($group.items.Count -gt 25) { " | ... and $($group.items.Count - 25) more" } else { '' }
        Log "    $($group.name): $($shown -join ' | ')$more"
    }
}

# Unchecks the review's row whose item reads `name`, which excludes it, and says how; $null if
# nothing did. Qt's item views offer no UI Automation pattern that checks a row (a checkable cell's
# Toggle pattern tells its check state, but in the first run its Toggle changed nothing), so: the
# row's Toggle pattern, in case it does; else the keyboard (a click on the first row's text, Down to
# the row, Space); else clicks across the row's check box. Clicks only land inside the tree. The
# counts below the tree tell that a check changed; trace show tells which.
function Clear-ReviewCheck($review, [string]$name) {
    $before = Get-ReviewCounts $review
    $changed = { (Get-ReviewCounts $review) -ne $before }
    $find = {
        $tree = Find-Element $review 'traceReviewTree' $null '^Traced items$' 10
        $rows = @(Get-ViewRows $tree 4)
        for ($i = 0; $i -lt $rows.Count; $i++) {
            if ("$($rows[$i].Current.Name)" -eq $name) { return @{ rows = $rows; at = $i; area = $tree.Current.BoundingRectangle } }
        }
        throw "the review has no row '$name'"
    }
    # A row's place, if all of it is inside the tree: an offscreen row's may fall on other widgets.
    $inside = { param($rect, $area) -not $rect.IsEmpty -and -not $area.IsEmpty -and $rect.Top -ge $area.Top -and $rect.Bottom -le $area.Bottom -and $rect.Left -ge $area.Left - 1 }
    $found = & $find
    try {
        $toggle = $found.rows[$found.at].GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
        Log "    the row's Toggle pattern says $($toggle.Current.ToggleState); toggling"
        $toggle.Toggle()
        Start-Sleep -Seconds 1
        if (& $changed) { return 'its Toggle pattern' }
    } catch {
        Log "    the row has no Toggle pattern ($($_.Exception.GetType().Name))"
    }
    $rect = $found.rows[0].Current.BoundingRectangle
    if ((& $inside $rect $found.area) -and $rect.Width -gt 20) {
        try { $review.SetFocus() } catch { }
        # Right of its check box, on its text: selects the first row and focuses the tree.
        [AvelInput]::Click([int]($rect.Left + $rect.Width * 0.6), [int]($rect.Top + $rect.Height / 2))
        Start-Sleep -Milliseconds 500
        if (& $changed) { throw "the click on the first row's text changed a check: $(Get-ReviewCounts $review)" }
        for ($i = 0; $i -lt $found.at; $i++) {
            [AvelInput]::Key(0x28, $true)  # Down
            Start-Sleep -Milliseconds 80
        }
        Start-Sleep -Milliseconds 300
        [AvelInput]::Key(0x20, $false)  # Space
        Start-Sleep -Seconds 1
        if (& $changed) { return "the keyboard (Down $($found.at) times, Space)" }
        Log "    the keyboard changed nothing (first row at $([int]$rect.Left),$([int]$rect.Top), $([int]$rect.Width)x$([int]$rect.Height))"
    } else {
        Log "    the first row isn't in view inside the tree ($rect; the tree at $($found.area))"
    }
    # The keyboard brought the row into view, if nothing else.
    $found = & $find
    $rect = $found.rows[$found.at].Current.BoundingRectangle
    if (-not (& $inside $rect $found.area)) { Log "    the row isn't in view inside the tree ($rect; the tree at $($found.area))"; return $null }
    $scale = [Math]::Max(1.0, $rect.Height / 20.0)  # Rows are about 20 pixels high at 100 %
    foreach ($offset in 4, 7, 10, 13, 16, 20, 24) {
        $x = [int]($rect.Left + $offset * $scale)
        [AvelInput]::Click($x, [int]($rect.Top + $rect.Height / 2))
        Start-Sleep -Milliseconds 800  # Longer than a double click
        if (& $changed) { return "a click on its check box ($offset px into the row at $([int]$rect.Left),$([int]$rect.Top))" }
    }
    Log "    clicks across the row's start changed nothing (row at $([int]$rect.Left),$([int]$rect.Top), $([int]$rect.Width)x$([int]$rect.Height))"
    return $null
}

# The program list's Trace column for the program named `name` (selected first): its text and
# tooltip, once the list has caught up with the traces.
function Get-TraceMark([string]$name, [int]$seconds = 60) {
    Select-Program $name
    $deadline = (Get-Date).AddSeconds($seconds)
    while ($true) {
        $mark = $null
        $how = ''
        try {
            $cell = Find (All (Cond $A::NameProperty $name) (Cond $A::IsSelectionItemPatternAvailableProperty $true)) 10
            try {
                $item = $cell.GetCurrentPattern([System.Windows.Automation.GridItemPattern]::Pattern)
                $table = $item.Current.ContainingGrid
                $column = 6  # ProgramTableModel::TraceColumn
                try {
                    $headers = @($table.GetCurrentPattern([System.Windows.Automation.TablePattern]::Pattern).Current.GetColumnHeaders() |
                        ForEach-Object { "$($_.Current.Name)" })
                    if ($headers -contains 'Trace') { $column = [Array]::IndexOf([string[]]$headers, 'Trace') }
                } catch { }
                $mark = $table.GetCurrentPattern([System.Windows.Automation.GridPattern]::Pattern).GetItem($item.Current.Row, $column)
                $how = "grid row $($item.Current.Row), column $column"
            } catch {
                # Without the grid patterns: a row's cells follow its name's.
                $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
                $mark = $cell
                for ($i = 0; $i -lt 6 -and $mark; $i++) { $mark = $walker.GetNextSibling($mark) }
                $how = 'the sixth cell after the name'
            }
        } catch { }
        $text = if ($mark) { "$($mark.Current.Name)" } else { '' }
        if ($text -match '^Traced' -or (Get-Date) -gt $deadline) {
            $tip = if ($mark) { "$($mark.Current.HelpText)" } else { '' }
            return @{ text = $text; tip = $tip; how = $how }
        }
        Start-Sleep -Seconds 1
    }
}

# Starts a manual trace from the GUI (Trace > Start tracing...) and dismisses the message that says
# it records. Returns what Avel said.
function Start-GuiTracing([string]$description) {
    Open-Menu 'Trace' ('Start tracing' + $ellipsis)
    $ask = Find-Window '^Start tracing$' 30 'startTracingDialog'
    Set-Text ($ask.FindFirst('Descendants', (Cond $A::ControlTypeProperty $CT::Edit))) $description
    Click (Button $ask '^Start tracing$')
    # The trace starts behind a step dialog; then Avel says it records (or why it can't).
    $ok = Find (All (Cond $A::ControlTypeProperty $CT::Button) (Cond $A::NameProperty 'OK')) 180
    $box = Get-WindowOf $ok
    $said = if ($box) { "$($box.Current.Name): $(Texts $box)" } else { '' }
    Click $ok
    return $said
}

# A leftover review (`window`, its cells `items`) and an item the trace's review excluded: not
# offered, or shown but not checked (an item another scanner finds by name is shown, never
# checked). `what` names the review in the messages.
function Test-ExcludedOffer($window, $items, [string]$where, [string]$what) {
    if ($items -notcontains $where) { Log "  OK: $what doesn't offer $where, which the trace's review excluded"; return }
    $row = Find-ViewRow $window $where 5
    $state = if ($row) { try { Get-ToggleState $row } catch { 'unknown' } } else { 'unknown' }
    if ($state -eq 'Off') { Log "  OK: $what shows $where, which the trace's review excluded, not checked" }
    else { Fail "$($what) shows $where, which the trace's review excluded, and its check is $state, not Off" }
}

# --- The steps ----------------------------------------------------------------------------------

# 1. Install and trace, with 7-Zip's MSI.
function Invoke-GuiInstallAndTrace {
    $msi = Join-Path $installers '7z-x64.msi'
    if (-not (Test-Path -LiteralPath $msi)) { Fail "gui 1: $msi is missing (run Get-SampleInstallers.ps1)"; return }
    Close-OpenTrace
    $known = Get-TraceIds
    Start-Sleep -Seconds $settleSeconds  # Windows' reactions to what came before
    $watch = [AvelNameWatch]::new('C:\')
    $statuses = [System.Collections.Generic.List[string]]::new()
    Start-Hold
    try {
        $journalBefore = [AvelJournal]::Query('C:')
        $preStart = Get-FileTimeNow
        $pre = New-OracleCapture 'gui-7zip-pre'
        $runStart = Get-FileTimeNow
        Open-Menu 'Trace' ('Install and trace' + $ellipsis)
        $dialog = Find-Window '^Install and trace$' 30 'installTraceDialog'
        Set-Text (Find-Element $dialog 'installerPath' $CT::Edit '^Installer$' 10) $msi
        Set-Text (Find-Element $dialog 'installerArguments' $CT::Edit '^Arguments$' 10) '/qn /norestart'
        Start-Sleep -Milliseconds 500
        $description = Get-Text (Find-Element $dialog 'traceDescription' $CT::Edit '^Description$' 10)
        Write-Held "  Install and trace: $msi, arguments /qn /norestart, description '$description'"
        Click (Button $dialog '^Install and trace$')
        $progress = Find-Window '^Install and trace: ' 60 'traceProgressDialog'
        # It stops by itself once the installation is over, links the trace, then opens the review.
        $deadline = (Get-Date).AddSeconds(900)
        $review = $null
        while (-not $review) {
            try {
                $status = Get-Text (ById $progress 'traceStatus' 0 $CT::Text)
                if ($status -and ($statuses.Count -eq 0 -or $statuses[$statuses.Count - 1] -ne $status)) { $statuses.Add($status) }
            } catch { }
            try { $review = Find-Window '^Trace review' 0 'traceReviewDialog' } catch { }
            if (-not $review) {
                if ((Get-Date) -gt $deadline) { throw "the trace didn't stop by itself within 900 s (no review opened): $(Texts $progress)" }
                Start-Sleep -Seconds 1
            }
        }
        $runEnd = Get-FileTimeNow
        $outcome = try { Get-Text (ById $progress 'traceResult' 5) } catch { "(no outcome: $(Texts $progress))" }
        $post = New-OracleCapture 'gui-7zip-post'
        $postEnd = Get-FileTimeNow
        $journalAfter = [AvelJournal]::Query('C:')
    } finally {
        $watch.Dispose()
        Stop-Hold
    }
    Log "  the progress dialog said: $($statuses -join ' / ')"
    Log "  its outcome: $($outcome -replace "`r?`n", ' / ')"
    $new = @(Get-TraceIds | Where-Object { $known -notcontains $_ })
    if ($new.Count -ne 1) { Fail "gui 1: $($new.Count) new trace(s) after Install and trace, not 1"; return }
    $id = $new[0]
    $show = Invoke-Trace @('show', $id, '--json') 'gui-7zip-show'
    if (-not $show.json) { Fail "gui 1: trace show printed no JSON (exit $($show.exit))"; return }
    $json = $show.json
    Write-TraceSummary $json
    if ($json.trace.state -ne 'complete' -or $json.trace.mode -ne 'launch' -or $json.trace.stop_reason -ne 'installer-done') {
        Fail "gui 1: the trace is $($json.trace.state), mode $($json.trace.mode), stop reason $($json.trace.stop_reason), not complete, launch, installer-done"
    } else { Log '  OK: the trace stopped by itself once the installation was over' }
    if ($json.trace.watch.exit_code -notin 0, 3010) { Fail "gui 1: the installer exited with $($json.trace.watch.exit_code)" }
    $link = Get-PrimaryLink $json
    if (-not $link -or $link.name -notmatch '^7-Zip ' -or $link.matched_by -ne 'created-key') {
        Fail "gui 1: the trace's primary link is $(if ($link) { "$($link.name) [$($link.matched_by)]" } else { 'missing' }), not 7-Zip's entry by the key it created"
    } else { Log "  OK: the trace is linked to $($link.name) ($($link.program_id)) by the Uninstall key it created" }
    $folder = @($json.changes | Where-Object { $_ -and $_.change -eq 'created' -and $_.location.kind -eq 'folder' -and $_.location.path -eq 'C:\Program Files\7-Zip' })
    if (-not $folder) { Fail 'gui 1: the trace has no created folder C:\Program Files\7-Zip' }
    elseif (-not $folder[0].tied) { Fail "gui 1: C:\Program Files\7-Zip isn't tied to 7-Zip (tie: $($folder[0].tie))" }

    $read = Read-Review $review
    Write-Review $read
    $tied = Get-ReviewGroup $read '^Tied to '
    if (-not $tied) { Fail "gui 1: the review has no group of items tied to the program" }
    elseif ($tied.items -notcontains 'C:\Program Files\7-Zip') { Fail "gui 1: the review's $($tied.name) doesn't list C:\Program Files\7-Zip" }
    else { Log "  OK: the review lists C:\Program Files\7-Zip in $($tied.name)" }
    Click (Button $review '^Close$')
    Start-Sleep -Seconds 1
    Click (Button (Find-Window '^Install and trace: ' 10 'traceProgressDialog') '^Close$')
    $script:guiState.sevenZip = @{ id = $id; name = "$($link.name)"; description = $description }

    $all = Get-AllChanges $show 'gui-7zip'
    Write-NoiseCounts $all
    $window = Get-TraceWindow $id $runStart $runEnd
    $gaps = @(@{ from = $preStart; to = $window.start; keysTo = $window.keys; what = 'before the trace started'; watch = $watch },
              @{ from = $window.stop; to = $postEnd; what = 'after the trace stopped'; watch = $watch })
    if ($watch.Overflowed) { Log '  the name watch overflowed: items around the trace may lack evidence' }
    $lines = @(Get-CaptureDiff $pre $post)
    Test-Completeness $lines $all 'gui-7zip' -Gaps $gaps -Journal @{ before = $journalBefore; after = $journalAfter }
    Test-Precision $all $lines 'gui-7zip'
}

# 2. Start tracing and Stop tracing around the fixture's installer; the review; one exclusion.
function Invoke-GuiManualTrace {
    Close-OpenTrace
    Remove-TraceFixture
    Initialize-TraceFixture
    $paths = Get-TraceFixturePaths
    $known = Get-TraceIds
    Start-Sleep -Seconds $settleSeconds
    $said = Start-GuiTracing $guiFixtureDescription
    $label = Get-Text (Find-Element (Window 'Avel') 'tracingLabel' $CT::Text '^Tracing since ' 20)
    $open = @(Get-TraceList | Where-Object { $known -notcontains "$($_.id)" -and $_.state -eq 'open' })
    Log "  Avel said: $said"
    if ($label -notmatch '^Tracing since ') { Fail "gui 2: the status bar says '$label', not 'Tracing since ...'" }
    else { Log "  OK: the status bar says '$label'" }
    if ($open.Count -ne 1) { Fail "gui 2: $($open.Count) new open trace(s) after Start tracing, not 1"; return }
    $id = "$($open[0].id)"
    Start-Hold
    try {
        $journalBefore = [AvelJournal]::Query('C:')
        $pre = New-OracleCapture 'gui-fixture-pre'
        $command = @(Get-TraceFixtureCommand)
        $arguments = @($command | Select-Object -Skip 1 | ForEach-Object { Format-Argument $_ }) -join ' '
        $code = (Start-Process $command[0] $arguments -Wait -PassThru -WindowStyle Hidden).ExitCode
        Start-Sleep -Seconds $settleSeconds
        $journalPost = [AvelJournal]::Query('C:')
        $postTime = Get-FileTimeNow
        $post = New-OracleCapture 'gui-fixture-post'
        $journalAfter = [AvelJournal]::Query('C:')
        # The status bar's Stop tracing (the Trace menu has one too).
        Click (Button (Window 'Avel') '^Stop tracing$')
        $review = Find-Window '^Trace review' 300 'traceReviewDialog'
        $stopTime = Get-FileTimeNow
    } finally {
        Stop-Hold
    }
    if ($code -ne 0) { Fail "gui 2: the fixture's installer returned $code" }
    $after = try { $element = ById (Window 'Avel') 'tracingLabel' 0 $CT::Text; "'$(Get-Text $element)'$(if ($element.Current.IsOffscreen) { ', off screen' })" } catch { 'gone' }
    Log "  after Stop tracing the status bar's label is $after"

    $show = Invoke-Trace @('show', $id, '--json') 'gui-fixture-show'
    if (-not $show.json) { Fail "gui 2: trace show printed no JSON (exit $($show.exit))"; return }
    $json = $show.json
    Write-TraceSummary $json
    if ($json.trace.state -ne 'complete' -or $json.trace.mode -ne 'manual' -or $json.trace.stop_reason -ne 'manual') {
        Fail "gui 2: the trace is $($json.trace.state), mode $($json.trace.mode), stop reason $($json.trace.stop_reason), not complete, manual, manual"
    } else { Log '  OK: Stop tracing finished the trace' }
    $link = Get-PrimaryLink $json
    if (-not $link -or $link.name -ne 'Contoso Trace' -or $link.matched_by -ne 'created-key') {
        Fail "gui 2: the trace's primary link is $(if ($link) { "$($link.name) [$($link.matched_by)]" } else { 'missing' }), not the fixture by the key it created"
        if (-not $link) { return }
    } else { Log "  OK: the trace is linked to the fixture ($($link.program_id))" }
    $all = Get-AllChanges $show 'gui-fixture'
    Write-NoiseCounts $all
    $inside = New-InsideWindow $id $journalBefore $journalPost $postTime $stopTime
    $record = @{ id = $id; pre = $pre; post = $post; result = $json; all = $all; lines = @(Get-CaptureDiff $pre $post); paths = $paths
                 journal = @{ before = $journalBefore; after = $journalAfter }; inside = $inside }
    foreach ($change in @($json.changes | Where-Object { $_ })) { Log "    $(Format-Change $change)" }
    Test-Completeness $record.lines $all 'gui-fixture' -Journal $record.journal -Inside $inside
    Test-Precision $json $record.lines 'gui-fixture'
    Test-FixtureTrace $json 'gui 2' $paths

    $read = Read-Review $review
    Write-Review $read
    $tied = Get-ReviewGroup $read '^Tied to '
    foreach ($want in $traceRoot, $guiExcludedItem.where) {
        if (-not $tied -or $tied.items -notcontains $want) { Fail "gui 2: the review doesn't list $want among the items tied to the fixture" }
    }
    $values = Get-ReviewGroup $read '^Environment variables and shared DLLs '
    if (-not $values -or $values.items.Count -ne 5) { Fail "gui 2: the review lists $(if ($values) { $values.items.Count } else { 'no' }) environment and shared DLL value(s), not the fixture's 5" }
    $existing = Get-ReviewGroup $read '^Changes to items that existed before'
    if (-not $existing -or $existing.items -notcontains 'HKLM\SOFTWARE\Contoso Shared : Theme') { Fail "gui 2: the review doesn't show the Theme the fixture changed among the changes to what existed before" }
    if ($tied -and $values -and $existing) { Log '  OK: the review has the fixture''s items tied to it, its 5 values, and Theme shown only' }

    $how = Clear-ReviewCheck $review $guiExcludedItem.where
    if (-not $how) {
        Fail "gui 2: the review's $($guiExcludedItem.where) couldn't be unchecked"
    } else {
        Log "  unchecked $($guiExcludedItem.where) in the review, by $($how): $(Get-ReviewCounts $review)"
        $saved = ''
        $deadline = (Get-Date).AddSeconds(20)
        while ($saved -notmatch 'saved\.$' -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 500
            try { $saved = Get-Text (Find-Element $review 'reviewSaveStatus' $CT::Text 'choices' 2) } catch { $saved = '' }
        }
        if ($saved -notmatch 'saved\.$') { Fail "gui 2: the review didn't save the exclusion: '$saved'" } else { Log "  the review says: $saved" }
    }
    $marked = Invoke-Trace @('show', $id, '--all', '--json') 'gui-fixture-show-reviewed'
    $changes = @($marked.json.changes | Where-Object { $_ -and $_.excluded })
    $knownValues = @($marked.json.known_values | Where-Object { $_ -and $_.excluded })
    $what = @(@($changes | ForEach-Object { Format-Change $_ }) + @($knownValues | ForEach-Object { "known value $($_.key) : $($_.name)" })) -join '; '
    if ($changes.Count -ne 1 -or $knownValues.Count -ne 0 -or $changes[0].location.kind -ne $guiExcludedItem.kind -or
        (Where-Text $changes[0].location) -ne $guiExcludedItem.where) {
        Fail "gui 2: trace show marks as excluded: $(if ($what) { $what } else { 'nothing' }); want only $($guiExcludedItem.kind) $($guiExcludedItem.where)"
    } else { Log "  OK: trace show marks $($guiExcludedItem.kind) $($guiExcludedItem.where) as excluded, and nothing else" }
    Click (Button $review '^Close$')
    if ($marked.json) { $record.all = $marked.json }  # With the review's marks
    $script:guiState.fixture = @{ id = $id; record = $record; program = "$($link.program_id)"
                                  excluded = @{ changes = $changes; known_values = $knownValues } }
}

# 3. The Trace column and the Traces dialog.
function Test-GuiTraceMarks {
    $traces = @()
    if ($script:guiState.sevenZip) { $traces += @{ program = $script:guiState.sevenZip.name; description = $script:guiState.sevenZip.description } }
    if ($script:guiState.fixture) { $traces += @{ program = 'Contoso Trace'; description = $guiFixtureDescription } }
    if (-not $traces) { Log '  skipped: no program was traced through the GUI'; return }
    foreach ($trace in $traces) {
        $mark = Get-TraceMark $trace.program
        if ($mark.text -notmatch '^Traced') { Fail "gui 3: the Trace column of $($trace.program) says '$($mark.text)' ($($mark.how)), not Traced" }
        else { Log "  OK: the Trace column of $($trace.program) says '$($mark.text)'$(if ($mark.tip) { ': ' + ($mark.tip -replace "`r?`n", ' / ') })" }
    }
    Open-Menu 'Trace' ('Traces' + $ellipsis)
    $dialog = Find-Window '^Traces$' 30 'tracesDialog'
    $deadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 500
        $cells = @(Get-ViewCells $dialog)
        $missing = @($traces | Where-Object { $cells -notcontains $_.description })
    } while ($missing -and (Get-Date) -lt $deadline)
    for ($i = 0; $i + 5 -lt $cells.Count; $i += 6) { Log "    $($cells[$i..($i + 5)] -join ' | ')" }
    foreach ($trace in $traces) {
        # The Program column: the link's name, then its version and publisher in parentheses.
        $program = @($cells | Where-Object { $_ -eq $trace.program -or $_.StartsWith("$($trace.program) (") })
        if ($cells -notcontains $trace.description) { Fail "gui 3: the Traces dialog doesn't list the trace '$($trace.description)'" }
        elseif (-not $program) { Fail "gui 3: the Traces dialog doesn't name $($trace.program) as a trace's program" }
        else { Log "  OK: the Traces dialog lists '$($trace.description)' of $($program[0])" }
    }
    $newest = if ($script:guiState.fixture) { $script:guiState.fixture.id } else { $script:guiState.sevenZip.id }
    $details = Get-Text (Find-Element $dialog 'traceDetails' $null '^Trace [0-9]{8}-' 10)
    $first = ($details -split "`r?`n")[0]
    if ($newest -and $first -ne "Trace $newest") { Fail "gui 3: the Traces dialog shows '$first', not the newest trace $newest" }
    else { Log "  OK: the Traces dialog shows the newest trace: $($details -replace "`r?`n", ' / ')" }
    Click (Button $dialog '^Close$')
}

# 4 and 5. The fixture uninstalled with its trace from the program list; the Undo Center.
function Invoke-GuiUninstallByTrace {
    $f = $script:guiState.fixture
    if (-not $f) { Log '  skipped: the fixture was not traced through the GUI (step 2)'; return }
    if (-not (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso Trace')) { Fail 'gui 4: the fixture is not installed'; return }
    $record = $f.record
    $fixture = New-FixtureContext (Get-TraceFixtureRuleId)
    $excludedKeys = @($f.excluded.changes | ForEach-Object { "$($_.location.kind)|$(Where-Text $_.location)" })
    $planned = $fixture.Clone()
    $planned.created = @($fixture.created | Where-Object { $excludedKeys -notcontains "$($_.kind)|$($_.where)" })
    $specs = @($fixture.removedSpecs) + @($fixture.stayingSpecs) + @($fixture.valueSpecs)
    foreach ($spot in $userPathEntries.Keys) { Set-UserPathEntry $spot $userPathEntries[$spot] }
    $script:guiState.userPaths = $true
    Log "  the user added their own Path entries: $(@($userPathEntries.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', ')"
    $shell = New-ShellCoverage $record.all $fixture $record.lines

    # What the GUI's leftover scan offers, as avel-cli sees it while the program is installed: what
    # Test-Revert asks about what the removal left.
    $preview = Invoke-Avel @('scan', $f.program, '--all-levels', '--json') 'gui-scan' 300
    if (-not $preview.json) { Fail "gui 4: scan printed no JSON (exit $($preview.exit))"; return }
    $programs = @($preview.json.programs | Where-Object { $_ })
    if ($programs.Count -ne 1 -or $programs[0].trace -ne $f.id) {
        Fail "gui 4: scan's programs are $(@($programs | ForEach-Object { "$($_.id) by trace $($_.trace)" }) -join '; '), not $($f.program) by trace $($f.id)"
    }
    # The excluded items never reach the scan, so the trace use doesn't count them.
    $counted = @{ all = [pscustomobject]@{ changes = @($record.all.changes | Where-Object { $_ -and -not $_.excluded }) } }
    $null = Test-TraceUse $preview.json 'gui 4 scan' $f.id $f.program $counted
    foreach ($change in @($f.excluded.changes)) {
        $where = Where-Text $change.location
        if (@($preview.json.leftovers | Where-Object { $_ -and $_.location.kind -eq $change.location.kind -and (Where-Text $_.location) -eq $where })) {
            Fail "gui 4: scan offers $($change.location.kind) $where, which the review excluded"
        } else { Log "  OK: scan doesn't offer $($change.location.kind) $where, which the review excluded" }
    }

    Select-Program 'Contoso Trace'
    Click (Find (All (Cond $A::ControlTypeProperty $CT::Button) (Cond $A::NameProperty 'Uninstall')))
    $confirm = Window 'Uninstall Contoso Trace'
    Set-Check 'Uninstall silently where possible' $true
    Set-Check 'Create a System Restore point first' $false
    Set-Check 'Look for leftovers afterwards' $true
    Set-Check 'Simulate only: show what would happen, change nothing' $false
    $box = Find-Element $confirm 'useTraceBox' $CT::CheckBox '^Use the install trace' 10
    $state = Get-ToggleState $box
    $tip = "$($box.Current.HelpText)" -replace "`r?`n", ' / '
    if ($state -ne 'On') {
        Fail "gui 4: the uninstall dialog's '$($box.Current.Name)' is $state, not checked"
        Set-Check $box.Current.Name $true
    } else { Log "  OK: the uninstall dialog offers '$($box.Current.Name)', checked$(if ($box.Current.IsOffscreen) { ' (off screen)' })$(if ($tip) { ": $tip" })" }
    Click (Button (Window 'Uninstall Contoso Trace') '^Uninstall$')

    # The leftover review opens by itself once the uninstaller ran and the scan is done.
    $leftovers = Window 'Leftovers of Contoso Trace' 300
    Start-Sleep -Seconds 1
    $use = try { Get-Text (Find-Element $leftovers 'traceUseLabel' $CT::Text '^Install trace of ' 5) } catch { '' }
    if ($use -notmatch '^Install trace of Contoso Trace: ') { Fail "gui 4: the leftover review doesn't say it used the fixture's trace: '$use'" }
    else { Log "  OK: the leftover review says: $use" }
    $traceBox = Find-Element $leftovers 'useTraceBox' $CT::CheckBox '^Use the install trace$' 10
    if ((Get-ToggleState $traceBox) -ne 'On') { Fail "gui 4: the leftover review's 'Use the install trace' isn't checked" }
    $items = @(Get-ViewCells $leftovers)
    $wanted = @('C:\ProgramData\Contoso Trace', "$env:APPDATA\Contoso Trace", $traceStartMenu, 'HKLM\SOFTWARE\Contoso Trace',
                'HKCU\Software\Contoso Trace', $traceService, $traceTask)
    $missing = @($wanted | Where-Object { $items -notcontains $_ })
    if ($missing) { Fail "gui 4: the leftover review doesn't list $($missing -join ', ')" }
    else { Log "  OK: the leftover review lists the fixture's leftovers ($($items.Count) cells)" }
    $edits = @($items | Where-Object { $_ -match 'Contoso Trace\\bin from ' -or $_ -match '^(Lower|Delete) the shared DLL count|^Delete the system variable CONTOSO_HOME' })
    Log "  value edits in the review: $($edits -join ' | ')"
    if ($edits.Count -lt 5) { Fail "gui 4: the leftover review lists $($edits.Count) of the fixture's 5 value edits" }
    foreach ($change in @($f.excluded.changes)) { Test-ExcludedOffer $leftovers $items (Where-Text $change.location) 'gui 4: the leftover review' }

    # Windows' reactions to the uninstaller first; then the watch trace, and the state before the
    # removal.
    Start-Sleep -Seconds $settleSeconds
    Close-OpenTrace
    $start = Invoke-Trace @('start', '--description', 'watch: gui', '--json') 'gui-watch-start'
    $watching = $start.exit -eq 0 -and $start.json.trace.state -eq 'open'
    if (-not $watching) { Fail "gui 4: the watch trace didn't start (exit $($start.exit)); Windows' own activity can't be told apart" }
    $tasks = @()
    $stop = $null
    Start-Hold
    try {
        $tasks += @(Get-TaskTargets $traceTask)
        $dumpBefore = Split-StateDump (Get-StateDump $specs)
        $journal = @{ before = [AvelJournal]::Query('C:'); after = $null }
        $before = New-OracleCapture 'gui-before'
        $remove = Button (Window 'Leftovers of Contoso Trace') '^Remove [0-9]+ item'
        Write-Held "  pressing '$($remove.Current.Name)'"
        Click $remove
        $null = Window 'Removing leftovers' 60
        $null = Button (Window 'Removing leftovers') '^Close$' 300
        $removalEnd = Get-FileTimeNow
        Write-Held "  removal: $(Texts (Window 'Removing leftovers'))"
        Start-Sleep -Seconds $settleSeconds
        $values = Read-FixtureValues
        $dumpRemoved = Split-StateDump (Get-StateDump $specs)
        $after = New-OracleCapture 'gui-after'
        $kept = Test-TaskFolder $guiExcludedItem.where
        $set = Get-NewestChangeSet 'gui-undo-list'
        $shown = if ($set) { Invoke-Avel @('undo', '--show', $set.id, '--json') 'gui-undo-show' } else { $null }
        Click (Button (Window 'Removing leftovers') '^Close$')
        Start-Sleep -Seconds 1
        Click (Button (Window 'Uninstalling') '^Close$')
        Start-Sleep -Seconds 2

        Open-Menu 'Tools' ('Undo Center' + $ellipsis)
        $center = Window 'Undo Center'
        Start-Sleep -Seconds 1
        Write-Held "  Undo Center: $(Texts $center)"
        $undoStart = Get-FileTimeNow
        Click (Button $center '^Restore all$')
        $null = Window 'Restoring' 60
        $null = Button (Window 'Restoring') '^Close$' 300
        Write-Held "  restore: $(Texts (Window 'Restoring'))"
        Click (Button (Window 'Restoring') '^Close$')
        Start-Sleep -Seconds 1
        Click (Button (Window 'Undo Center') '^Close$')
        Start-Sleep -Seconds $settleSeconds
        $undone = New-OracleCapture 'gui-undone'
        $journal.after = [AvelJournal]::Query('C:')
        $dumpUndone = Split-StateDump (Get-StateDump $specs)
        $tasks += @(Get-TaskTargets $traceTask)
    } finally {
        if ($watching) { $stop = Invoke-Trace @('stop', '--json') 'gui-watch-stop' }
        Stop-Hold
    }
    $watchAll = $null
    if ($watching -and $stop.json) {
        Log '  the watch trace around the removal and the restore:'
        Write-TraceSummary $stop.json
        $watchAll = Get-AllChanges $stop 'gui-watch'
        Write-NoiseCounts $watchAll
        $null = Invoke-Trace @('delete', $stop.json.trace.id) 'gui-watch-delete'
    } elseif ($watching) {
        Fail "gui 5: the watch trace's stop printed no JSON (exit $($stop.exit))"
    }

    if (-not $set -or -not $shown -or -not $shown.json) { Fail 'gui 4: the removal''s change set can''t be read (undo --list, undo --show)'; return }
    Log "  change set $($set.id): $($set.description), $($set.state)"
    $removal = [pscustomobject]@{ set_id = $set.id; items = @($shown.json.items | Where-Object { $_ }); quarantined_bytes = $shown.json.quarantined_bytes }
    $json = [pscustomobject]@{ leftover_scan = $preview.json; leftover_removal = $removal }
    Test-RemovalDone $removal 'gui 4'
    Check-Safe @($removal.items) 'gui 4 removal'
    Test-FixturePlan $json $planned 'gui 4' -ProgramGone
    $plan = New-LocationMap $removal.items
    foreach ($change in @($f.excluded.changes)) {
        if (Find-LocationItem $plan $change.location.kind (Where-Text $change.location)) { Fail "gui 4: planned $(Where-Text $change.location), which the trace's review excluded" }
    }
    foreach ($spec in $fixture.removedSpecs) { if ($dumpRemoved[$spec] -notmatch '^<missing>') { Fail "gui 4: not removed: $spec" } }
    foreach ($spec in $fixture.stayingSpecs) { if ($dumpRemoved[$spec] -ne $dumpBefore[$spec]) { Fail "gui 4: changed what must stay: $spec" } }
    if (-not $kept) { Fail "gui 4: the task folder $($guiExcludedItem.where), which the review excluded, is gone" }
    else { Log "  OK: the task folder $($guiExcludedItem.where), which the review excluded, is still there" }
    $pathVerdicts = Get-FixturePathVerdicts $values $record.paths 'gui 4'
    Test-Revert $record $after $json 'gui-revert' -Fixture $fixture -PathVerdicts $pathVerdicts -ReviewExcluded $f.excluded
    Compare-State $dumpBefore $dumpUndone 'gui 5: the Undo Center gave back exactly what was there (state dumps)'
    Test-Undo $before $undone $watchAll $removal 'gui-undo' $tasks $shell $journal -Idle @{ from = $removalEnd; to = $undoStart }
    $script:guiState.restored = $true
}

# 6a. Revert by trace from the Traces dialog.
function Invoke-GuiRevertByTrace {
    $f = $script:guiState.fixture
    if (-not $f -or -not $script:guiState.restored) { Log '  skipped: needs the fixture''s trace and its leftovers back (steps 2, 4 and 5)'; return }
    Close-OpenTrace
    $fixture = New-FixtureContext (Get-TraceFixtureRuleId)
    $specs = @($fixture.removedSpecs) + @($fixture.stayingSpecs) + @($fixture.valueSpecs)
    $dumpBefore = Split-StateDump (Get-StateDump $specs)
    $newest = Get-NewestChangeSet 'gui-revert-list-before'
    Open-Menu 'Trace' ('Traces' + $ellipsis)
    $dialog = Find-Window '^Traces$' 30 'tracesDialog'
    $details = Get-Text (Find-Element $dialog 'traceDetails' $null '^Trace [0-9]{8}-' 20)
    $first = ($details -split "`r?`n")[0]
    if ($first -ne "Trace $($f.id)") {
        # The dialog acts on its current trace, the newest, which UI Automation can't change.
        Fail "gui 6a: the Traces dialog's current trace is '$first', not the fixture's $($f.id)"
        Click (Button $dialog '^Close$')
        return
    }
    Click (Button $dialog ('^Revert by trace' + $ellipsis + '$'))
    $review = Find-Window '^Forced removal of ' 180
    Start-Sleep -Seconds 1
    Log "  $($review.Current.Name): $(Texts $review)"
    $items = @(Get-ViewCells $review)
    $missing = @(@('HKCU\Software\Contoso Trace', 'C:\ProgramData\Contoso Trace', $traceService, $traceTask) | Where-Object { $items -notcontains $_ })
    if ($missing) { Fail "gui 6a: the forced removal's review doesn't list $($missing -join ', ')" }
    else { Log "  OK: the forced removal's review lists the fixture's leftovers ($($items.Count) cells)" }
    foreach ($change in @($f.excluded.changes)) { Test-ExcludedOffer $review $items (Where-Text $change.location) "gui 6a: the forced removal's review" }
    $remove = Button $review '^Remove [0-9]+ item'
    Log "  pressing '$($remove.Current.Name)'"
    Click $remove
    # Nothing runs from the program's folders; should Avel list something, it is ended.
    try {
        $running = Find-Window '^Running programs$' 5
        Log "  running programs: $((Tree-Items $running) -join ' / ')"
        Click (Button $running '^End processes$')
    } catch { }
    $null = Button (Find-Window '^Forced removal$' 120) '^Close$' 300
    Log "  removal: $(Texts (Find-Window '^Forced removal$' 10))"
    Start-Sleep -Seconds 2
    $values = Read-FixtureValues
    $dumpAfter = Split-StateDump (Get-StateDump $specs)
    $kept = Test-TaskFolder $guiExcludedItem.where
    Click (Button (Find-Window '^Forced removal$' 10) '^Close$')
    foreach ($spec in $fixture.removedSpecs) { if ($dumpAfter[$spec] -notmatch '^<missing>') { Fail "gui 6a: not removed: $spec" } }
    foreach ($spec in $fixture.stayingSpecs) { if ($dumpAfter[$spec] -ne $dumpBefore[$spec]) { Fail "gui 6a: changed what must stay: $spec" } }
    if (-not $kept) { Fail "gui 6a: the task folder $($guiExcludedItem.where), which the review excluded, is gone" }
    foreach ($verdict in (Get-FixturePathVerdicts $values $f.record.paths 'gui 6a').GetEnumerator()) {
        if ($verdict.Value.class -ne 'KNOWN') { Fail "gui 6a: $($verdict.Key) Path: $($verdict.Value.reason)" }
    }
    $set = Get-NewestChangeSet 'gui-revert-list'
    if (-not $set -or ($newest -and $set.id -eq $newest.id)) { Fail 'gui 6a: Revert by trace made no change set'; return }
    $shown = Invoke-Avel @('undo', '--show', $set.id, '--json') 'gui-revert-show'
    $removal = [pscustomobject]@{ set_id = $set.id; items = @($shown.json.items | Where-Object { $_ }); quarantined_bytes = $shown.json.quarantined_bytes }
    Log "  change set $($set.id): $($set.description)"
    Test-RemovalDone $removal 'gui 6a'
    $plan = New-LocationMap $removal.items
    foreach ($key in @("firewall-rule|$($fixture.ruleId)", 'registry-value|HKLM\SOFTWARE\Contoso Shared : Theme') +
                     @($f.excluded.changes | ForEach-Object { "$($_.location.kind)|$(Where-Text $_.location)" })) {
        if ($plan.ContainsKey($key)) { Fail "gui 6a: planned what must stay: $key" }
    }
    $edits = @($removal.items | Where-Object { $_.location.kind -eq 'value-edit' })
    if ($edits.Count -ne 5) { Fail "gui 6a: $($edits.Count) value edit(s) planned, not the fixture's 5" }
    $undo = Invoke-Avel @('undo', $set.id, '--json') 'gui-revert-undo' 600
    if ($undo.json.state -ne 'restored') { Fail "gui 6a: undo: state $($undo.json.state)" }
    Compare-State $dumpBefore (Split-StateDump (Get-StateDump $specs)) 'gui 6a: undo gave back exactly what Revert by trace took (state dumps)'
}

# 6b. A trace left open when avel.exe ends: the prompt at the next start.
function Invoke-GuiStartupPrompt {
    Close-OpenTrace
    $known = Get-TraceIds
    $said = Start-GuiTracing 'Avel startup prompt check'
    $open = @(Get-TraceList | Where-Object { $known -notcontains "$($_.id)" -and $_.state -eq 'open' })
    if ($open.Count -ne 1) { Fail "gui 6b: $($open.Count) new open trace(s) after Start tracing, not 1 ($said)"; return }
    $id = "$($open[0].id)"
    Stop-TraceGui
    $state = @(Get-TraceList | Where-Object { "$($_.id)" -eq $id })[0].state
    Log "  trace $id started in the GUI; avel.exe ended, the trace is $state"
    Start-TraceGui
    $finish = Find (All (Cond $A::ControlTypeProperty $CT::Button) (Cond $A::NameProperty 'Finish the trace')) 60
    $box = Get-WindowOf $finish
    Log "  at its start Avel asks: $(if ($box) { "$($box.Current.Name): $(Texts $box)" })"
    Click $finish
    $review = Find-Window '^Trace review' 300 'traceReviewDialog'
    Log "  OK: Finish the trace opened '$($review.Current.Name)'"
    $null = Read-Review $review
    Click (Button $review '^Close$')
    $show = Invoke-Trace @('show', $id, '--json') 'gui-startup-show'
    if ($show.json.trace.state -ne 'complete' -or $show.json.trace.stop_reason -ne 'manual') {
        Fail "gui 6b: the trace is $($show.json.trace.state), stop reason $($show.json.trace.stop_reason), not complete, manual"
    } else { Log '  OK: the prompt finished the trace avel.exe left open' }
    $null = Invoke-Trace @('delete', $id) 'gui-startup-delete'
}

# --- The round ----------------------------------------------------------------------------------

function Invoke-TraceGuiRound {
    Log ''
    Log 'GUI round (5c): avel.exe through UI Automation - Install and trace, Start and Stop tracing, the review, the Trace column and Traces, uninstall with the trace, the Undo Center'
    # UI Automation's rectangles and the mouse in the same pixels, before any UI Automation call.
    [AvelInput]::UsePhysicalPixels()
    if ($diffExclude -notcontains $guiSettingsPattern) {
        $script:diffExclude += $guiSettingsPattern
        $script:diffExcludeRegex = [regex]::new('(?:' + ($script:diffExclude -join ')|(?:') + ')', 'IgnoreCase')
    }
    $script:guiState = @{}
    Close-OpenTrace
    Remove-SevenZip
    Start-TraceGui
    try {
        Invoke-GuiStep 'GUI 1: Trace > Install and trace... with 7-Zip''s MSI, the review' { Invoke-GuiInstallAndTrace }
        Invoke-GuiStep 'GUI 2: Trace > Start tracing... and Stop tracing around Contoso Trace Fixture, the review, an exclusion' { Invoke-GuiManualTrace }
        Invoke-GuiStep 'GUI 3: the Trace column, Trace > Traces...' { Test-GuiTraceMarks }
        Invoke-GuiStep 'GUI 4-5: the fixture uninstalled with its trace from the program list, then Tools > Undo Center... > Restore all' { Invoke-GuiUninstallByTrace }
        if ($guiOptionalSteps -contains 'revert') {
            Invoke-GuiStep 'GUI 6a (optional): Trace > Traces... > Revert by trace... for the fixture' { Invoke-GuiRevertByTrace }
        }
        if ($guiOptionalSteps -contains 'startup') {
            Invoke-GuiStep 'GUI 6b (optional): a trace left open when avel.exe ends, finished from the prompt at its next start' { Invoke-GuiStartupPrompt }
        }
    } finally {
        Stop-Hold
        Stop-TraceGui
        try { Close-OpenTrace } catch { Log "  cleanup failed: $($_.Exception.Message)" }
        if ($script:guiState.userPaths) {
            foreach ($spot in $userPathEntries.Keys) { Set-UserPathEntry $spot $userPathEntries[$spot] -Remove }
        }
    }
}
