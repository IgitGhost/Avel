<#
.SYNOPSIS
Compares Avel's program list with Settings > Apps > Installed apps.

.DESCRIPTION
Opens Settings, reads every entry of the installed-apps list through UI Automation, and compares
each entry ("Name, version, publisher, date") with the programs avel-cli reports as listed in
Settings. Read-only: nothing on the system is changed.

Run it from a non-elevated PowerShell, as the user whose apps Settings shows.

.PARAMETER AvelCli
Path to avel-cli.exe. Defaults to the release build.

.PARAMETER SettingsDump
A text file with one Settings entry per line, as written by -SaveDump, used instead of reading
Settings live.

.PARAMETER SaveDump
Writes the entries read from Settings to this file.

.EXAMPLE
.\scripts\Compare-SettingsApps.ps1
#>
param(
    [string]$AvelCli = (Join-Path $PSScriptRoot '..\build\release\bin\avel-cli.exe'),
    [string]$SettingsDump,
    [string]$SaveDump
)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$Automation = [System.Windows.Automation.AutomationElement]

function Read-SettingsApps {
    Start-Process 'ms-settings:appsfeatures'
    $deadline = [DateTime]::Now.AddSeconds(30)

    # Settings' content lives in a SystemSettings.exe window hosted by ApplicationFrameHost.
    $list = $null
    while (-not $list -and [DateTime]::Now -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $process = Get-Process SystemSettings -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $process) { continue }
        $content = $Automation::RootElement.FindFirst('Descendants',
            (New-Object System.Windows.Automation.PropertyCondition($Automation::ProcessIdProperty, $process.Id)))
        if (-not $content) { continue }
        # The installed-apps list is the List whose items read "Name, version, publisher, date".
        $lists = $content.FindAll('Descendants', (New-Object System.Windows.Automation.PropertyCondition(
            $Automation::ControlTypeProperty, [System.Windows.Automation.ControlType]::List)))
        foreach ($candidate in $lists) {
            $first = $candidate.FindFirst('Children', [System.Windows.Automation.Condition]::TrueCondition)
            if ($first -and $first.Current.Name -match ', ' -and
                $candidate.GetSupportedPatterns() -contains [System.Windows.Automation.ItemContainerPattern]::Pattern) {
                $list = $candidate
                break
            }
        }
    }
    if (-not $list) { throw 'Could not find the installed-apps list in Settings.' }
    Start-Sleep -Seconds 2  # Let Settings finish populating the list

    # ItemContainerPattern walks virtualized items too, not just the visible ones.
    $container = $list.GetCurrentPattern([System.Windows.Automation.ItemContainerPattern]::Pattern)
    $entries = New-Object System.Collections.Generic.List[string]
    $item = $container.FindItemByProperty($null, $null, $null)
    while ($item) {
        $name = $item.Current.Name
        if (-not $name) {
            try {
                $item.GetCurrentPattern([System.Windows.Automation.VirtualizedItemPattern]::Pattern).Realize()
                $name = $item.Current.Name
            } catch { }
        }
        if ($name) { $entries.Add($name) }
        $item = $container.FindItemByProperty($item, $null, $null)
    }
    Get-Process SystemSettings -ErrorAction SilentlyContinue | Stop-Process -ErrorAction SilentlyContinue
    return $entries
}

# Settings writes bidirectional marks around date parts and keeps trailing spaces in names.
$BidiMarks = '[{0}{1}]' -f [char]0x200E, [char]0x200F  # Built in code: this file must stay ASCII for Windows PowerShell
function Normalize([string]$line) { ($line -replace $BidiMarks, '') -replace ' +,', ',' }

$settingsEntries = if ($SettingsDump) { Get-Content $SettingsDump -Encoding utf8 | Where-Object { $_ } } else { Read-SettingsApps }
if ($SaveDump) { $settingsEntries | Set-Content $SaveDump -Encoding utf8 }
$settingsLines = @($settingsEntries | ForEach-Object { Normalize $_ })

$inventory = (& $AvelCli list --json) -join "`n" | ConvertFrom-Json
$listed = @($inventory.programs | Where-Object { $_.listed_in_settings })
# Format each program the way Settings does, dates in the user's short date format.
$avelLines = @{}
foreach ($program in $listed) {
    $date = if ($program.install_date) { [datetime]::ParseExact($program.install_date, 'yyyy-MM-dd', $null).ToString('d') } else { '' }
    $line = (@($program.name, $program.version, $program.publisher, $date) | Where-Object { $_ }) -join ', '
    $avelLines[$line] = $program
}

$equal = @($settingsLines | Where-Object { $avelLines.ContainsKey($_) })
$onlySettings = @($settingsLines | Where-Object { -not $avelLines.ContainsKey($_) })
$onlyAvel = @($avelLines.Keys | Where-Object { $settingsLines -notcontains $_ } | Sort-Object)

"Settings entries: $($settingsLines.Count)"
"Avel entries listed in Settings: $($listed.Count)"
"Identical: $($equal.Count)"
if ($onlySettings) {
    ''
    "In Settings but not identical in Avel ($($onlySettings.Count)):"
    $onlySettings | ForEach-Object { "  $_" }
}
if ($onlyAvel) {
    ''
    "In Avel but not in Settings ($($onlyAvel.Count)):"
    $onlyAvel | ForEach-Object { '  {0}  [{1}]' -f $_, $avelLines[$_].source }
}
