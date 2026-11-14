# Dot-sourced by the extensions scenario, which runs Invoke-ExtensionsGuiRound last: Phase 6c's GUI
# round, the Browser extensions window (Tools > Browser extensions...) driven in English through UI
# Automation (gui.ps1) by its object names (docs/tools.md, "GUI (Tools > Browser extensions...)"),
# on the fixtures as rounds 1-5 leave them (all there; ids in $fixtures). Each step is judged with
# the state dumps, avel-cli (undo --list) and the window itself (its status bar's result):
#   1. The window lists every user's entries; the signed-out user's hive is free again afterwards.
#   2. The search (the shared machine policy's extension id) shows its three entries; the current
#      user's Default one, selected, Remove...: the confirmation names the other two profiles
#      ("Work", and AvelOther's "Other Person") as removed too; confirmed, the forcelist is
#      renumbered (one new change set of two items); Undo last change gives back identical dumps.
#   3. Compare-and-set: the current user's forcelist entry "2" selected, then its value "3" changed
#      behind the window's back (after the window listed it); Remove: the edit of "3" is skipped,
#      so the changed value stays ("Removed in part"); Undo last change puts "2" back and leaves
#      the changed "3" alone. The scenario then writes "3" back as it was.
#   4. Open extensions page: AvelOther's row leaves it disabled; Edge's row (the current user's
#      Default profile) starts msedge.exe through Explorer (its parent), on edge://extensions with
#      --profile-directory=Default, not elevated (no elevated token, integrity below high). Should
#      Explorer itself run elevated (no UAC), Edge can't be less: then (KNOWN) Edge's token must be
#      Explorer's level and its parent Explorer, not avel.exe. Edge is closed again.
#   5. avel.exe closes (the window, then the main window) and exits with 0; every key and file is
#      as at the start (Edge's own profile aside, which Edge wrote in step 4).
# Expects extensions.ps1's helpers (Get-ExtensionsDump, Test-StateAfter, Get-ChangeSets...) and
# extensions_fixture.ps1.

. "$PSScriptRoot\gui.ps1"

$ellipsis = [string][char]0x2026

if (-not ('AvelTokenInfo' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class AvelTokenInfo {
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetTokenInformation(IntPtr token, int infoClass, IntPtr info, int length, out int returned);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthority(IntPtr sid, uint index);
    [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
    [DllImport("user32.dll")] static extern IntPtr GetShellWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out int pid);

    // The desktop shell's process id, or 0.
    public static int ShellProcessId() {
        int pid = 0;
        IntPtr shell = GetShellWindow();
        if (shell != IntPtr.Zero) GetWindowThreadProcessId(shell, out pid);
        return pid;
    }

    // { TokenIsElevated, TokenElevationType (1 default, 2 full, 3 limited), integrity RID }, or null.
    public static int[] Query(int pid) {
        IntPtr process = OpenProcess(0x1000, false, pid);  // PROCESS_QUERY_LIMITED_INFORMATION
        if (process == IntPtr.Zero) return null;
        IntPtr token;
        bool opened = OpenProcessToken(process, 0x0008, out token);  // TOKEN_QUERY
        CloseHandle(process);
        if (!opened) return null;
        IntPtr buffer = Marshal.AllocHGlobal(256);
        try {
            int returned;
            int[] result = new int[3];
            // TokenElevation wants its exact size (ERROR_BAD_LENGTH otherwise).
            if (!GetTokenInformation(token, 20, buffer, 4, out returned)) return null;  // TokenElevation
            result[0] = Marshal.ReadInt32(buffer);
            if (!GetTokenInformation(token, 18, buffer, 4, out returned)) return null;  // TokenElevationType
            result[1] = Marshal.ReadInt32(buffer);
            if (!GetTokenInformation(token, 25, buffer, 256, out returned)) return null;  // TokenIntegrityLevel
            IntPtr sid = Marshal.ReadIntPtr(buffer);
            int count = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
            result[2] = Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(count - 1)));
            return result;
        } finally {
            Marshal.FreeHGlobal(buffer);
            CloseHandle(token);
        }
    }
}
'@
}

function Get-ExtensionsWindow([int]$seconds = 30) { Find-Window '^Browser extensions$' $seconds 'extensionsWindow' }

function Get-ExtensionsLabel($window, [string]$id) {
    try { return Get-Text (ById $window $id 2 $CT::Text) } catch { return '' }
}

# Waits until the window is idle with a listing: Refresh enabled, no work under way, the count
# shown. Returns the count's text.
function Wait-ExtensionsIdle($window, [int]$seconds = 120) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ($true) {
        try {
            $refresh = ById $window 'extensionsRefreshButton' 2
            $progress = Get-ExtensionsLabel $window 'extensionsProgress'
            $count = Get-ExtensionsLabel $window 'extensionsCount'
            if ($refresh.Current.IsEnabled -and -not $progress -and $count -match '[0-9]') { return $count }
        } catch { }
        if ((Get-Date) -gt $deadline) { throw "the Browser extensions window isn't idle after $seconds s: $(Texts $window)" }
        Start-Sleep -Milliseconds 400
    }
}

# Waits for the status bar's result to read `pattern` (and differ from `previous`), then for the
# listing that follows a change. Returns the result.
function Wait-ExtensionsResult($window, [string]$pattern, [string]$previous, [int]$seconds = 120) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $text = ''
    while ($true) {
        $text = Get-ExtensionsLabel $window 'extensionsResult'
        if ($text -ne $previous -and $text -match $pattern) { break }
        if ((Get-Date) -gt $deadline) { throw "the result reads '$text', not /$pattern/, after $seconds s" }
        Start-Sleep -Milliseconds 300
    }
    $null = Wait-ExtensionsIdle $window
    Log "  window: $text"
    return $text
}

# The table's rows, through its Grid pattern: name, browser, profile and user, and the name's cell.
function Get-ExtensionsRows($window) {
    $table = ById $window 'extensionsTable' 10
    $grid = $table.GetCurrentPattern([System.Windows.Automation.GridPattern]::Pattern)
    $rows = [System.Collections.Generic.List[object]]::new()
    for ($row = 0; $row -lt $grid.Current.RowCount; $row++) {
        $cells = @(for ($column = 0; $column -lt 4; $column++) { Get-GridCell $grid $row $column })
        $texts = @($cells | ForEach-Object { if ($_) { "$($_.Current.Name)" } else { '' } })
        $rows.Add([pscustomobject]@{ name = $texts[0]; browser = $texts[1]; profile = $texts[2]; user = $texts[3]; cell = $cells[0] })
    }
    return $rows.ToArray()
}

function Format-Rows($rows) { return (@($rows) | ForEach-Object { "$($_.name) ($($_.browser), $($_.profile), $($_.user))" }) -join '; ' }

# Types `text` into the search box; returns the rows it leaves, once there are `count` of them.
function Search-Extensions($window, [string]$text, [int]$count) {
    Set-Text (ById $window 'extensionsSearch' 10) $text
    $deadline = (Get-Date).AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 400
        $rows = @(Get-ExtensionsRows $window)
    } while ($rows.Count -ne $count -and (Get-Date) -lt $deadline)
    if ($rows.Count -ne $count) { throw "the search '$text' shows $($rows.Count) row(s), not $($count): $(Format-Rows $rows)" }
    return $rows
}

function Test-RowSelected($row) {
    try { return [bool]$row.cell.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected }
    catch { return $false }
}

# Selects the one row `pick` (a filter on Get-ExtensionsRows' rows) chooses: through its cell's
# SelectionItem pattern, else a click.
function Select-ExtensionRow($window, [scriptblock]$pick, [string]$what) {
    $find = {
        $found = @(Get-ExtensionsRows $window | Where-Object $pick)
        if ($found.Count -ne 1) { throw "$($found.Count) row(s) are $what" }
        return $found[0]
    }
    (& $find).cell.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Start-Sleep -Milliseconds 700
    if (Test-RowSelected (& $find)) { return }
    Log "    UI Automation didn't select the row $what; clicking it"
    try { $window.SetFocus() } catch { }
    $rect = (& $find).cell.Current.BoundingRectangle
    if ($rect.IsEmpty) { throw "the row $what has no place on screen" }
    [AvelInput]::Click([int]($rect.Left + [Math]::Min(60, $rect.Width / 2)), [int]($rect.Top + $rect.Height / 2))
    Start-Sleep -Milliseconds 700
    if (-not (Test-RowSelected (& $find))) { throw "the row $what isn't selected" }
}

function Get-ExtensionsButton($window, [string]$id) { ById $window $id 10 }

# The one change set made since `before`; fails unless it has `items` items, `done` of them done.
function Get-GuiNewSet($before, [string]$label, [int]$items, [int]$done) {
    $known = @($before | ForEach-Object { $_.id })
    $new = @(Get-ChangeSets "$label-sets" | Where-Object { $_.id -notin $known })
    if ($new.Count -ne 1) { Fail "$($label): $($new.Count) new change set(s), not 1"; return $null }
    Log "  change set $($new[0].id) '$($new[0].description)': $($new[0].state), $($new[0].items) item(s), $($new[0].done) done"
    if ($new[0].items -ne $items -or $new[0].done -ne $done) { Fail "$($label): the set has $($new[0].items) item(s), $($new[0].done) done, not $items and $done" }
    return $new[0]
}

function Test-SetState($setId, [string]$state, [string]$label) {
    $set = @(Get-ChangeSets "$label-sets" | Where-Object { $_.id -eq $setId })[0]
    if (-not $set) { Fail "$($label): set $setId is gone" }
    elseif ($set.state -ne $state) { Fail "$($label): set $setId is $($set.state), not $state" }
    else { Log "  OK: $label (set $setId $state)" }
}

# Remove... on the selected rows, then the confirmation: returns its text, after `confirm` (the
# confirm button, or nothing when $false).
function Invoke-GuiRemove($window, [bool]$confirm = $true) {
    $button = Get-ExtensionsButton $window 'removeExtensionButton'
    if (-not $button.Current.IsEnabled) { throw 'Remove is disabled' }
    Click $button
    $box = Find-Window '' 20 'removeExtensionsConfirmation'
    $text = Texts $box
    Log "  confirmation '$($box.Current.Name)': $($text -replace "`r?`n", ' | ')"
    if ($confirm) { Click (ById $box 'removeExtensionsConfirmButton' 10) }
    return $text
}

function Get-TokenText($info) {
    if (-not $info) { return 'unreadable' }
    return ('elevated={0} type={1} integrity=0x{2:X4}' -f $info[0], $info[1], $info[2])
}

# One step of the round; what it throws fails it, and the round goes on.
function Invoke-ExtensionsGuiStep([string]$stepName, [scriptblock]$body) {
    Log "  -- $stepName"
    try { & $body }
    catch { Fail "gui, $($stepName): $($_.Exception.Message) at $(Get-ErrorPlace $_)" }
}

function Invoke-ExtensionsGuiRound {
    # UI Automation's rectangles and the mouse in the same pixels, before any UI Automation call.
    [AvelInput]::UsePhysicalPixels()
    $forced = Get-Fixture 'chrome-forced'
    $userQ = Get-Fixture 'user-q'
    $edge = Get-Fixture 'edge-forced'
    $process = Start-AvelGui
    $null = $process.Handle  # Without this, ExitCode stays empty once the process has exited
    try {
        Invoke-ExtensionsGuiStep '1: Tools > Browser extensions...' {
            $null = Window 'Avel' 60
            Open-Menu 'Tools' ('Browser extensions' + $ellipsis)
            $script:extensionsWindowElement = Get-ExtensionsWindow
            $count = Wait-ExtensionsIdle $extensionsWindowElement
            Log "  window: $count; $(Get-ExtensionsLabel $extensionsWindowElement 'extensionsWarnings')"
            Test-OtherHiveFree 'the window listed every user'
        }
        $window = $script:extensionsWindowElement
        if (-not $window) { throw 'the Browser extensions window did not open' }

        Invoke-ExtensionsGuiStep '2: the shared machine policy: Remove, its confirmation, Undo last change' {
            $rows = Search-Extensions $window $forced.ext 3
            Log "  rows: $(Format-Rows $rows)"
            Select-ExtensionRow $window { $_.browser -eq 'Google Chrome' -and $_.profile -eq 'Person 1' } "Person 1's"
            $sets = Get-ChangeSets 'gui-forced-before'
            $previous = Get-ExtensionsLabel $window 'extensionsResult'
            $text = Invoke-GuiRemove $window
            $bad = $failures
            if ($text -notmatch 'The same policies or registrations install it here too') { Fail 'gui confirmation: no "here too" list' }
            foreach ($named in 'Work', 'Other Person', $otherUser) {
                if ($text -notmatch [regex]::Escape($named)) { Fail "gui confirmation: '$named' isn't named" }
            }
            if ($text -notmatch 'ExtensionInstallForcelist') { Fail 'gui confirmation: the forcelist edits aren''t listed' }
            if ($failures -eq $bad) { Log '  OK: the confirmation lists the edits and names Work and AvelOther''s Other Person as removed too' }
            $null = Wait-ExtensionsResult $window '^1 browser extension removed; the browser drops it at its next start\.' $previous
            $list = 'V Chrome\ExtensionInstallForcelist'
            Test-StateAfter (Get-ExtensionsDump $allSpecs) @{ hklmChrome = @{ copy = @{ "$list|2" = "$list|3" }; delete = @("$list|3") } } `
                'gui remove: the forcelist renumbered, nothing else changed'
            $set = Get-GuiNewSet $sets 'gui-forced' 2 2
            $previous = Get-ExtensionsLabel $window 'extensionsResult'
            Click (Get-ExtensionsButton $window 'extensionsUndoButton')
            $null = Wait-ExtensionsResult $window '^Undone: Remove browser extension Contoso Forced\.' $previous
            Compare-State $start (Get-ExtensionsDump $allSpecs) 'gui undo: every key and file as at the start'
            if ($set) { Test-SetState $set.id 'restored' 'gui undo' }
        }

        Invoke-ExtensionsGuiStep '3: a value changed behind the window''s back is skipped, not overwritten' {
            $null = Search-Extensions $window $userQ.ext 1
            Select-ExtensionRow $window { $_.name -eq $userQ.ext } "'$($userQ.ext)'"
            $sets = Get-ChangeSets 'gui-changed-before'
            $changed = "$($ids.userR);https://contoso.example/changed"
            Set-RegistryValue (Get-Hkcu) 'Software\Policies\Google\Chrome\ExtensionInstallForcelist' '3' $changed
            Log "  forcelist '3' changed behind the window's back to '$changed'"
            $previous = Get-ExtensionsLabel $window 'extensionsResult'
            $null = Invoke-GuiRemove $window
            $null = Wait-ExtensionsResult $window '^.*Removed in part: 1 change couldn''t be made\.' $previous
            $list = 'V Chrome\ExtensionInstallForcelist'
            Test-StateAfter (Get-ExtensionsDump $allSpecs) @{ hkcuChrome = @{ copy = @{ "$list|2" = "$list|3" }; set = @{ "$list|3" = (Get-StringData $changed) } } } `
                'gui compare-and-set: "2" edited, the changed "3" left as it was'
            $set = Get-GuiNewSet $sets 'gui-changed' 2 1
            $previous = Get-ExtensionsLabel $window 'extensionsResult'
            Click (Get-ExtensionsButton $window 'extensionsUndoButton')
            $null = Wait-ExtensionsResult $window ('^Undone: Remove browser extension ' + $userQ.ext + '\.') $previous
            Test-StateAfter (Get-ExtensionsDump $allSpecs) @{ hkcuChrome = @{ set = @{ "$list|3" = (Get-StringData $changed) } } } `
                'gui undo: "2" back, the changed "3" still left alone'
            if ($set) { Test-SetState $set.id 'restored' 'gui undo' }
            Set-RegistryValue (Get-Hkcu) 'Software\Policies\Google\Chrome\ExtensionInstallForcelist' '3' "$($ids.userR);$storeUrl"
            Compare-State $start (Get-ExtensionsDump $allSpecs) 'gui: with "3" written back, every key and file as at the start'
        }

        Invoke-ExtensionsGuiStep '4: Open extensions page: Edge through Explorer, not elevated' {
            $null = Search-Extensions $window $forced.ext 3
            Select-ExtensionRow $window { $_.profile -eq 'Other Person' } "AvelOther's"
            if ((Get-ExtensionsButton $window 'openExtensionsPageButton').Current.IsEnabled) { Fail "gui: Open extensions page is enabled for $otherUser's profile" }
            else { Log "  OK: Open extensions page is disabled for $otherUser's profile" }

            Get-Process msedge -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            $before = @(Get-Process msedge -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
            $null = Search-Extensions $window $edge.ext 1
            Select-ExtensionRow $window { $_.browser -eq 'Microsoft Edge' } 'Edge''s'
            $button = Get-ExtensionsButton $window 'openExtensionsPageButton'
            if (-not $button.Current.IsEnabled) { throw "Open extensions page is disabled for Edge's row: $($button.Current.HelpText)" }
            $shellPid = [AvelTokenInfo]::ShellProcessId()
            $shellToken = [AvelTokenInfo]::Query($shellPid)
            Log "  the desktop's Explorer: process $shellPid, $(Get-TokenText $shellToken); avel.exe: $(Get-TokenText ([AvelTokenInfo]::Query($process.Id)))"
            $previous = Get-ExtensionsLabel $window 'extensionsResult'
            Click $button
            $null = Wait-ExtensionsResult $window '^Opening Microsoft Edge''s extensions page' $previous 30
            $browser = $null
            $deadline = (Get-Date).AddSeconds(60)
            while (-not $browser -and (Get-Date) -lt $deadline) {
                Start-Sleep -Milliseconds 500
                $browser = @(Get-CimInstance Win32_Process -Filter "Name = 'msedge.exe'" -ErrorAction SilentlyContinue |
                             Where-Object { $_.ProcessId -notin $before -and "$($_.CommandLine)" -notmatch '--type=' }) | Select-Object -First 1
            }
            if (-not $browser) { throw 'no msedge.exe started within 60 s' }
            $token = [AvelTokenInfo]::Query([int]$browser.ProcessId)
            $parent = Get-Process -Id $browser.ParentProcessId -ErrorAction SilentlyContinue
            Log "  msedge.exe $($browser.ProcessId): $(Get-TokenText $token); parent $($browser.ParentProcessId) ($($parent.ProcessName)); $($browser.CommandLine)"
            $bad = $failures
            if ("$($browser.CommandLine)" -notmatch 'edge://extensions') { Fail 'gui: Edge''s command line doesn''t carry edge://extensions' }
            if ("$($browser.CommandLine)" -notmatch '--profile-directory=Default') { Fail 'gui: Edge''s command line doesn''t name the Default profile' }
            if ($browser.ParentProcessId -eq $process.Id) { Fail 'gui: avel.exe started Edge itself' }
            elseif ($browser.ParentProcessId -ne $shellPid) { Fail "gui: Edge's parent is $($browser.ParentProcessId) ($($parent.ProcessName)), not the desktop's Explorer ($shellPid)" }
            if (-not $token) { Fail 'gui: Edge''s token couldn''t be read' }
            elseif ($shellToken -and ($shellToken[0] -eq 1 -or $shellToken[2] -ge 0x3000)) {
                # Explorer itself runs elevated (no UAC): nothing it starts can be less.
                Log "  KNOWN: the desktop's Explorer runs elevated here ($(Get-TokenText $shellToken)), so Edge can only run at its level"
                if ($token[0] -ne $shellToken[0] -or $token[2] -ne $shellToken[2]) { Fail "gui: Edge's token ($(Get-TokenText $token)) isn't Explorer's ($(Get-TokenText $shellToken))" }
            } elseif ($token[0] -ne 0 -or $token[2] -ge 0x3000) {
                Fail "gui: Edge runs elevated ($(Get-TokenText $token))"
            }
            if ($failures -eq $bad) { Log '  OK: Edge started through Explorer on edge://extensions, at Explorer''s level, not avel.exe''s' }

            Get-Process msedge -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            $deadline = (Get-Date).AddSeconds(30)
            while (@(Get-Process msedge -ErrorAction SilentlyContinue).Count -and (Get-Date) -lt $deadline) {
                Start-Sleep -Milliseconds 500
                Get-Process msedge -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
            }
            if (@(Get-Process msedge -ErrorAction SilentlyContinue).Count) { Fail 'gui: Edge is still running after it was closed' }
            else { Log '  Edge closed' }
        }

        Invoke-ExtensionsGuiStep '5: avel.exe closes' {
            $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
            $deadline = (Get-Date).AddSeconds(20)
            $open = $true
            while ($open -and (Get-Date) -lt $deadline) {
                try { $null = Get-ExtensionsWindow 0 } catch { $open = $false }
                if ($open) { Start-Sleep -Milliseconds 300 }
            }
            if ($open) { Fail 'gui: the Browser extensions window is still open after its Close' }
            (Window 'Avel' 10).GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
            if (-not $process.WaitForExit(30000)) { Fail 'gui: avel.exe is still running 30 s after its main window closed' }
            elseif ($process.ExitCode -ne 0) { Fail "gui: avel.exe exited with $($process.ExitCode)" }
            else { Log '  OK: the Browser extensions window and the main window closed, avel.exe exited with 0' }
        }
    } finally {
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $null = $process.WaitForExit(15000)
        }
        Get-Process msedge -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    Test-OtherHiveFree 'after avel.exe'
    Test-StateAfter (Get-ExtensionsDump $allSpecs) @{} 'after the GUI round: every key and file as at the start (Edge''s profile aside)' @('edge')
}
