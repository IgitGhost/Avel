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

# The names of a window's tree rows (the leftover dialog's items).
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
