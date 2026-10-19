# Clicks through uninstaller dialogs the way a user would, until -StopFile appears. Started by
# uninstall.ps1 as its own PowerShell process (background jobs don't see the desktop's windows).
#
# Presses "Uninstall", "Yes" or "Next" first; "Finish", "Close" or "OK" only when nothing else is
# offered, so a maintenance page's "Close" isn't hit before its "Uninstall". The sandbox runs in the
# host's Windows language, so labels are matched in English and Russian (built from code points to
# keep this file ASCII), and the standard Yes button (control ID 6) is recognized by its automation ID.
param(
    [Parameter(Mandatory)][string]$StopFile,
    [Parameter(Mandatory)][string]$LogFile
)

function Write-Log([string]$text) { Add-Content $LogFile ('{0:HH:mm:ss} {1}' -f (Get-Date), $text) -Encoding utf8 }

function From-CodePoints([int[]]$codes) { -join ($codes | ForEach-Object { [char]$_ }) }
$ru = @{
    yes = From-CodePoints 0x414, 0x430
    uninstall = From-CodePoints 0x423, 0x434, 0x430, 0x43B, 0x438, 0x442, 0x44C
    next = From-CodePoints 0x414, 0x430, 0x43B, 0x435, 0x435
    deinstall = From-CodePoints 0x414, 0x435, 0x438, 0x43D, 0x441, 0x442, 0x430, 0x43B, 0x43B, 0x438, 0x440, 0x43E, 0x432, 0x430, 0x442, 0x44C
    done = From-CodePoints 0x413, 0x43E, 0x442, 0x43E, 0x432, 0x43E
    close = From-CodePoints 0x417, 0x430, 0x43A, 0x440, 0x44B, 0x442, 0x44C
    ok = From-CodePoints 0x41E, 0x41A
}
$forwardLabels = "^(Uninstall|Yes|Next.*|Remove|$($ru.yes)|$($ru.uninstall)|$($ru.next).*|$($ru.deinstall))$"
$finishLabels = "^(Finish|Close|OK|$($ru.done)|$($ru.close)|$($ru.ok))$"

try {
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [System.Windows.Automation.AutomationElement]
    $isButton = New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button)
} catch {
    Write-Log "clicker failed to start: $($_.Exception.Message)"
    exit 1
}
Write-Log 'clicker started'

$ignored = 'explorer', 'powershell', 'conhost', 'ShellExperienceHost', 'SearchHost', 'StartMenuExperienceHost',
    'TextInputHost', 'ApplicationFrameHost', 'SystemSettings', 'avel-cli', 'avel', 'sihost', 'ctfmon',
    'OpenConsole', 'WindowsTerminal'
$seen = @{}  # Windows already reported, to log each new one once
while (-not (Test-Path $StopFile)) {
    try {
        $windows = $A::RootElement.FindAll('Children', [System.Windows.Automation.Condition]::TrueCondition)
    } catch {
        Write-Log "listing windows failed: $($_.Exception.Message)"
        Start-Sleep -Seconds 1
        continue
    }
    foreach ($window in $windows) {
        try {
            $name = (Get-Process -Id $window.Current.ProcessId).ProcessName
            if ($ignored -contains $name) { continue }
            $buttons = @($window.FindAll('Descendants', $isButton) | Where-Object { $_.Current.IsEnabled })
            $key = "$name|$($window.Current.Name)|$(($buttons | ForEach-Object { $_.Current.Name }) -join ',')"
            if (-not $seen.ContainsKey($key)) {
                $seen[$key] = $true
                Write-Log ('window {0} "{1}", buttons: {2}' -f $name, $window.Current.Name,
                    (($buttons | ForEach-Object { '{0}[{1}]' -f $_.Current.Name, $_.Current.AutomationId }) -join ', '))
            }
            $target = $buttons | Where-Object {
                $_.Current.Name -match $forwardLabels -or $_.Current.AutomationId -eq '6'
            } | Select-Object -First 1
            if (-not $target) {
                $target = $buttons | Where-Object { $_.Current.Name -match $finishLabels } | Select-Object -First 1
            }
            if ($target) {
                Write-Log ('click {0} "{1}": {2}' -f $name, $window.Current.Name, $target.Current.Name)
                $target.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                Start-Sleep -Milliseconds 1500
            }
        } catch {
            # Windows come and go while they're inspected.
        }
    }
    Start-Sleep -Milliseconds 700
}
Write-Log 'clicker stopped'
