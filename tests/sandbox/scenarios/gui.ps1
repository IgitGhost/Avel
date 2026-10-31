# Dot-sourced by the scenarios that drive avel.exe through UI Automation, in English. Expects $bin
# to be set. Start-AvelGui starts the GUI; the other helpers look only inside its process. Element
# lookups are fresh every time: Qt's provider invalidates element references when the UI changes.

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$A = [System.Windows.Automation.AutomationElement]
$CT = [System.Windows.Automation.ControlType]

function Cond($property, $value) { New-Object System.Windows.Automation.PropertyCondition($property, $value) }
function All { New-Object System.Windows.Automation.AndCondition(, [System.Windows.Automation.Condition[]]$args) }

function Start-AvelGui {
    New-Item -Path 'HKCU:\Software\Avel\Avel\ui' -Force | Out-Null
    Set-ItemProperty 'HKCU:\Software\Avel\Avel\ui' -Name language -Value en
    $process = Start-Process (Join-Path $bin 'avel.exe') -PassThru
    $script:inApp = Cond $A::ProcessIdProperty $process.Id
    return $process
}

function Find($condition, [int]$seconds = 20, $root = $A::RootElement) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        try { $e = $root.FindFirst('Descendants', (All $script:inApp $condition)); if ($e) { return $e } } catch { }
        Start-Sleep -Milliseconds 300
    }
    throw "GUI element not found: $($condition.Value)"
}

function Window([string]$name, [int]$seconds = 20) {
    Find (All (Cond $A::ControlTypeProperty $CT::Window) (Cond $A::NameProperty $name)) $seconds
}

function Click($element) { $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }

# An enabled button whose name matches `pattern`; with -Disabled, one in any state.
function Button($window, [string]$pattern, [int]$seconds = 20, [switch]$Disabled) {
    $condition = if ($Disabled) { Cond $A::ControlTypeProperty $CT::Button }
                 else { All (Cond $A::ControlTypeProperty $CT::Button) (Cond $A::IsEnabledProperty $true) }
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        try {
            $found = @($window.FindAll('Descendants', $condition) | Where-Object { $_.Current.Name -match $pattern })
            if ($found) { return $found[0] }
        } catch { }
        Start-Sleep -Milliseconds 300
    }
    throw "no enabled button matching '$pattern' in '$($window.Current.Name)'"
}

function Texts($window) {
    @($window.FindAll('Descendants', (Cond $A::ControlTypeProperty $CT::Text)) | ForEach-Object { $_.Current.Name } |
        Where-Object { $_ }) -join ' / '
}

# The names of a window's tree cells in view (the leftover dialog's items); Get-ViewCells reads them
# all.
function Tree-Items($window) {
    @($window.FindAll('Descendants', (Cond $A::ControlTypeProperty $CT::TreeItem)) | ForEach-Object { $_.Current.Name })
}

function Set-Check($name, [bool]$on) {
    $box = Find (Cond $A::NameProperty $name)
    $toggle = $box.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
    if (($toggle.Current.ToggleState -eq 'On') -ne $on) { $toggle.Toggle() }
}

function Open-Menu([string]$menu, [string]$item) {
    $top = Find (All (Cond $A::ControlTypeProperty $CT::MenuItem) (Cond $A::NameProperty $menu))
    $top.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    Start-Sleep -Milliseconds 500
    Click (Find (All (Cond $A::ControlTypeProperty $CT::MenuItem) (Cond $A::NameProperty $item)))
}

# --- For the dialogs that name their widgets (Phase 5c, docs/trace.md "The GUI") ---

# Whether `element` is the widget whose object name is `id`: Qt's automation id joins an object's
# name with those of its named parents ("traceReviewDialog.traceReviewTree").
function Test-AutomationId($element, [string]$id) {
    $value = "$($element.Current.AutomationId)"
    return $value -eq $id -or $value.EndsWith(".$id")
}

# The first element below `root` (of `type`, when given) whose object name is `id`, or whose name
# matches `pattern` (its accessible name, or a label's text).
function Find-Element($root, [string]$id, $type = $null, [string]$pattern = '', [int]$seconds = 20) {
    $condition = if ($type) { Cond $A::ControlTypeProperty $type } else { [System.Windows.Automation.Condition]::TrueCondition }
    $deadline = (Get-Date).AddSeconds($seconds)
    do {
        try {
            foreach ($element in $root.FindAll('Descendants', $condition)) {
                if (($id -and (Test-AutomationId $element $id)) -or ($pattern -and "$($element.Current.Name)" -match $pattern)) {
                    return $element
                }
            }
        } catch { }
        Start-Sleep -Milliseconds 300
    } while ((Get-Date) -lt $deadline)
    throw "GUI element not found: $id $pattern"
}

function ById($root, [string]$id, [int]$seconds = 20, $type = $null) { Find-Element $root $id $type '' $seconds }

# A window of the GUI whose title matches `pattern`, or whose object name is `id`: a top-level one,
# or one UI Automation lists below its owner.
function Find-Window([string]$pattern, [int]$seconds = 20, [string]$id = '') {
    $isWindow = Cond $A::ControlTypeProperty $CT::Window
    $deadline = (Get-Date).AddSeconds($seconds)
    do {
        try {
            $level = @($A::RootElement.FindAll('Children', $script:inApp))
            for ($depth = 0; $depth -lt 4 -and $level.Count; $depth++) {
                $next = @()
                foreach ($element in $level) {
                    if (($pattern -and "$($element.Current.Name)" -match $pattern) -or ($id -and (Test-AutomationId $element $id))) {
                        return $element
                    }
                    $next += @($element.FindAll('Children', $isWindow))
                }
                $level = $next
            }
        } catch { }
        Start-Sleep -Milliseconds 300
    } while ((Get-Date) -lt $deadline)
    throw "GUI window not found: $pattern $id"
}

# The nearest window above `element` (a message box's, for its button).
function Get-WindowOf($element) {
    $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $current = $element
    while ($current -and $current.Current.ControlType -ne $CT::Window) { $current = $walker.GetParent($current) }
    return $current
}

# An edit's text, or another element's name (a label's text).
function Get-Text($element) {
    if ($element.Current.ControlType -eq $CT::Edit) {
        return "$($element.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value)"
    }
    return "$($element.Current.Name)"
}

function Set-Text($element, [string]$text) {
    $element.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($text)
}

function Get-ToggleState($element) {
    return "$($element.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Current.ToggleState)"
}

# An item view's cell at `row` and `column` through its Grid pattern, or $null.
function Get-GridCell($grid, [int]$row, [int]$column) {
    try { return $grid.GetItem($row, $column) } catch { return $null }
}

# The rows of an item view (a tree's or a table's), as their first column's cells, in order: every
# row of a tree's expanded branches, scrolled into view or not. Qt makes only the cells inside the
# view's viewport its children in UI Automation, so the rows come from its Grid pattern; without
# one, from the cells in view, `columns` (how many the view has) telling the rows apart when the
# cells don't tell their column themselves.
function Get-ViewRows($view, [int]$columns) {
    try {
        $grid = $view.GetCurrentPattern([System.Windows.Automation.GridPattern]::Pattern)
        $count = $grid.Current.RowCount
        $rows = [System.Collections.Generic.List[object]]::new()
        for ($row = 0; $row -lt $count; $row++) {
            $cell = Get-GridCell $grid $row 0
            if (-not $cell) { break }
            $rows.Add($cell)
        }
        if ($rows.Count -eq $count) { return $rows.ToArray() }
    } catch { }
    if (Get-Command Log -ErrorAction SilentlyContinue) { Log '    (the view has no Grid pattern: only its rows in view are read)' }
    $cells = @($view.FindAll('Descendants', (Cond $A::ControlTypeProperty $CT::TreeItem)))
    if (-not $cells) { $cells = @($view.FindAll('Descendants', (Cond $A::ControlTypeProperty $CT::DataItem))) }
    $first = [System.Collections.Generic.List[object]]::new()
    $byColumn = $true
    foreach ($cell in $cells) {
        try {
            if ($cell.GetCurrentPattern([System.Windows.Automation.GridItemPattern]::Pattern).Current.Column -eq 0) { $first.Add($cell) }
        } catch {
            $byColumn = $false
            break
        }
    }
    if (-not $byColumn) {
        $first.Clear()
        for ($i = 0; $i -lt $cells.Count; $i += [Math]::Max(1, $columns)) { $first.Add($cells[$i]) }
    }
    return $first.ToArray()
}

# The item views (trees, tables) in `window`.
function Get-Views($window) {
    $types = @($CT::Tree, $CT::DataGrid, $CT::Table) | ForEach-Object { Cond $A::ControlTypeProperty $_ }
    @($window.FindAll('Descendants', (New-Object System.Windows.Automation.OrCondition(, [System.Windows.Automation.Condition[]]$types))))
}

# The texts of every cell of the item views in `window`, row by row (as Tree-Items, which sees only
# the rows in view): through each view's Grid pattern, else the cells in view.
function Get-ViewCells($window) {
    $texts = [System.Collections.Generic.List[string]]::new()
    foreach ($view in @(Get-Views $window)) {
        $read = $false
        try {
            $grid = $view.GetCurrentPattern([System.Windows.Automation.GridPattern]::Pattern)
            $rows = $grid.Current.RowCount
            $columns = $grid.Current.ColumnCount
            $found = [System.Collections.Generic.List[string]]::new()
            for ($row = 0; $row -lt $rows; $row++) {
                for ($column = 0; $column -lt $columns; $column++) {
                    $cell = Get-GridCell $grid $row $column
                    $found.Add($(if ($cell) { "$($cell.Current.Name)" } else { '' }))
                }
            }
            $texts.AddRange($found)
            $read = $true
        } catch { }
        if (-not $read) {
            if (Get-Command Log -ErrorAction SilentlyContinue) { Log '    (a view has no Grid pattern: only its rows in view are read)' }
            foreach ($type in $CT::TreeItem, $CT::DataItem) {
                foreach ($cell in @($view.FindAll('Descendants', (Cond $A::ControlTypeProperty $type)))) { $texts.Add("$($cell.Current.Name)") }
            }
        }
    }
    return $texts.ToArray()
}

# The first-column cell of the row of an item view in `window` that reads `name`, or $null.
function Find-ViewRow($window, [string]$name, [int]$columns = 4) {
    foreach ($view in @(Get-Views $window)) {
        foreach ($row in @(Get-ViewRows $view $columns)) { if ("$($row.Current.Name)" -eq $name) { return $row } }
    }
    return $null
}

# Mouse and keyboard input, for what UI Automation can't do in Qt's item views (checking a row). In
# physical pixels: UsePhysicalPixels, before any UI Automation call, makes UI Automation report them.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class AvelInput {
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);

    public static void UsePhysicalPixels() {
        try {
            if (SetProcessDpiAwarenessContext(new IntPtr(-4))) return;  // Per monitor, version 2
        } catch (EntryPointNotFoundException) { }
        SetProcessDPIAware();
    }

    public static void Click(int x, int y) {
        SetCursorPos(x, y);
        Thread.Sleep(100);
        mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);  // Left button down
        Thread.Sleep(50);
        mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);  // Left button up
    }

    // A key press; `extended` for the arrow keys.
    public static void Key(byte key, bool extended) {
        uint flags = extended ? 1u : 0u;
        keybd_event(key, 0, flags, UIntPtr.Zero);
        Thread.Sleep(40);
        keybd_event(key, 0, flags | 2u, UIntPtr.Zero);  // Key up
    }
}
'@

# Waits for the main window, types `name` into the search box and selects its row.
function Select-Program([string]$name) {
    $null = Window 'Avel'
    Start-Sleep -Seconds 2
    $null = Find (Cond $A::ControlTypeProperty $CT::Edit)
    $search = $A::RootElement.FindAll('Descendants', (All $script:inApp (Cond $A::ControlTypeProperty $CT::Edit))) |
        Where-Object { -not $_.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.IsReadOnly } |
        Select-Object -First 1
    $search.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($name)
    Start-Sleep -Milliseconds 800
    $row = Find (All (Cond $A::NameProperty $name) (Cond $A::IsSelectionItemPatternAvailableProperty $true))
    $row.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Start-Sleep -Milliseconds 500
}
