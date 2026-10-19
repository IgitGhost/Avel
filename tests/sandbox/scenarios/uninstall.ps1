# Runs inside Windows Sandbox (Start-AvelSandbox.ps1 -Scenario uninstall). Phase 2 acceptance test:
# installs one sample per installer technology (see Get-SampleInstallers.ps1), then removes them
# with avel-cli twice:
#   round 1, interactive: one program at a time; a UI Automation helper clicks through each
#            uninstaller's dialogs the way a user would;
#   round 2, quiet: all five in one batch, with a System Restore point.
# A sample passes a round when avel-cli reports it removed and it's gone from the inventory.
# Writes results\uninstall.json (avel-cli's output), results\clicks.log and, last,
# results\uninstall.txt (the report the host waits for).

$ErrorActionPreference = 'Stop'
$bin = 'C:\Avel\bin'
$installers = 'C:\Avel\sandbox\installers'
$results = 'C:\Avel\results'
$cli = Join-Path $bin 'avel-cli.exe'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$report = [System.Collections.Generic.List[string]]::new()
$raw = [ordered]@{}
$failures = 0
$progressLog = Join-Path $results 'uninstall.log'  # Grows while the scenario runs
Remove-Item $progressLog -ErrorAction SilentlyContinue
function Log([string]$text) {
    $report.Add($text)
    Write-Host $text
    # Best effort: a host process reading the file through the mapped folder can lock it for a
    # moment, and a lost progress line must not stop the scenario (the report has every line).
    try { Add-Content $progressLog ('{0:HH:mm:ss} {1}' -f (Get-Date), $text) -Encoding utf8 } catch { }
}
function Fail([string]$text) { $script:failures++; Log "  FAIL: $text" }

$samples = @(
    @{ kind = 'msi';  pattern = '^7-Zip ' },
    # Notepad++ also registers a sparse package named "Notepad++" for the Windows 11 context menu,
    # hence the separate pattern for "gone". Its uninstaller leaves that package behind, silent or
    # not; it's reported as a known leftover until Phase 3's leftover scan removes it.
    @{ kind = 'nsis'; pattern = '^Notepad\+\+ \('; gone = '^Notepad\+\+'; known = '^Notepad\+\+$' },
    @{ kind = 'inno'; pattern = '^Inno Setup' },
    @{ kind = 'burn'; pattern = '^Microsoft Visual C\+\+ .*Redistributable \(x64\)' },
    @{ kind = 'msix'; pattern = '^Avel Test App$' }
)

function Install-Sample([string]$kind) {
    switch ($kind) {
        'msi'  { $p = Start-Process msiexec.exe "/i `"$installers\7z-x64.msi`" /qn /norestart" -Wait -PassThru }
        'nsis' { $p = Start-Process "$installers\npp-installer-x64.exe" '/S' -Wait -PassThru }
        'inno' { $p = Start-Process "$installers\innosetup-x64.exe" '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /ALLUSERS' -Wait -PassThru }
        'burn' { $p = Start-Process "$installers\vc_redist.x64.exe" '/install /quiet /norestart' -Wait -PassThru }
        'msix' { Add-AppxPackage -Path "$installers\AvelTestPackage.msix" -AllowUnsigned; return 0 }
    }
    return $p.ExitCode
}

function Get-Inventory {
    $output = & $cli list --json
    if ($LASTEXITCODE -ne 0) { throw "avel-cli list failed ($LASTEXITCODE)" }
    return ($output -join "`n" | ConvertFrom-Json).programs
}

function Find-Samples($programs) {
    foreach ($sample in $samples) {
        $found = @($programs | Where-Object { $_.listed_in_settings -and $_.name -match $sample.pattern })
        $sample.program = if ($found.Count -eq 1) { $found[0] } else { $null }
        $sample.matches = $found.Count
    }
}

# Runs avel-cli uninstall with a time limit, returning its parsed JSON (or $null).
function Invoke-Uninstall([string[]]$ids, [string[]]$options, [string]$label, [int]$timeoutSeconds) {
    $out = Join-Path $env:TEMP "avel-$label.json"
    $arguments = @('uninstall') + ($ids | ForEach-Object { "`"$_`"" }) + $options + @('--yes', '--json')
    $process = Start-Process $cli ($arguments -join ' ') -NoNewWindow -PassThru -RedirectStandardOutput $out
    if (-not $process.WaitForExit($timeoutSeconds * 1000)) {
        # Take the uninstaller down with avel-cli, or it would block the next one.
        & taskkill.exe /T /F /PID $process.Id | Out-Null
        Fail "$label timed out after $timeoutSeconds s"
        return $null
    }
    $text = Get-Content $out -Raw -Encoding utf8
    $raw[$label] = $text
    try { return $text | ConvertFrom-Json } catch { Fail "$label printed no JSON: $text"; return $null }
}

function Check-Result($sample, $result, [string]$round) {
    $line = '  {0,-5} {1,-45} {2}' -f $sample.kind, $sample.program.name, $result.outcome
    if ($result.exit_code -ne $null) { $line += ", exit $($result.exit_code)" }
    $line += ', {0:N1} s, {1}' -f ($result.elapsed_ms / 1000), $result.plan.command
    if ($result.plan.method -eq 'package') { $line += "remove package $($result.plan.package_full_name)" }
    if ($result.install_location_remains) { $line += ' [install folder remains]' }
    Log $line
    if ($result.outcome -notin 'removed', 'reboot-required') { Fail "$round $($sample.kind): $($result.outcome)" }
    if ($round -eq 'quiet' -and -not $result.plan.quiet) { Fail "quiet $($sample.kind): no silent plan" }
}

function Install-All([string]$round) {
    Log "Installing samples ($round)"
    foreach ($sample in $samples) {
        $code = Install-Sample $sample.kind
        if ($code -notin 0, 3010) { Fail "installing $($sample.kind) returned $code" }
    }
    Find-Samples (Get-Inventory)
    foreach ($sample in $samples) {
        if (-not $sample.program) { Fail "$($sample.kind) not found in the inventory ($($sample.matches) matches)" }
        else { Log ('  {0,-5} {1}  [{2}]' -f $sample.kind, $sample.program.name, $sample.program.id) }
    }
}

function Check-Gone([string]$round) {
    $programs = Get-Inventory
    foreach ($sample in $samples) {
        $pattern = if ($sample.gone) { $sample.gone } else { $sample.pattern }
        $left = @($programs | Where-Object { $_.name -match $pattern })
        $known = @($left | Where-Object { $sample.known -and $_.name -match $sample.known })
        $left = @($left | Where-Object { $known -notcontains $_ })
        if ($known) { Log "  KNOWN: $round $($sample.kind): leftover for Phase 3: $($known.name -join ', ')" }
        if ($left) { Fail "$round $($sample.kind): still in the inventory: $($left.name -join ', ')" }
    }
}

# Drives avel.exe through UI Automation: search for the program, select it, Uninstall, confirm,
# wait for the progress dialog to finish, and report what it shows. The GUI runs in English.
function Invoke-GuiUninstall($sample, [switch]$Quiet, [switch]$RestorePoint) {
    if (-not $sample.program) { Fail "gui $($sample.kind): not installed"; return }
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $A = [System.Windows.Automation.AutomationElement]
    $CT = [System.Windows.Automation.ControlType]
    New-Item -Path 'HKCU:\Software\Avel\Avel\ui' -Force | Out-Null
    Set-ItemProperty 'HKCU:\Software\Avel\Avel\ui' -Name language -Value en
    $gui = Start-Process (Join-Path $bin 'avel.exe') -PassThru
    $inApp = New-Object System.Windows.Automation.PropertyCondition($A::ProcessIdProperty, $gui.Id)
    function Cond($property, $value) { New-Object System.Windows.Automation.PropertyCondition($property, $value) }
    function All { New-Object System.Windows.Automation.AndCondition(, [System.Windows.Automation.Condition[]]$args) }
    # Fresh lookups every time: Qt's provider invalidates element references when the UI changes.
    function Find($condition, [int]$seconds = 20) {
        $deadline = (Get-Date).AddSeconds($seconds)
        while ((Get-Date) -lt $deadline) {
            try { $e = $A::RootElement.FindFirst('Descendants', (All $inApp $condition)); if ($e) { return $e } } catch { }
            Start-Sleep -Milliseconds 300
        }
        throw "GUI element not found: $($condition.Value)"
    }
    function Click($element) { $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke() }
    function Set-Check($name, [bool]$on) {
        $box = Find (Cond $A::NameProperty $name)
        $toggle = $box.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
        if (($toggle.Current.ToggleState -eq 'On') -ne $on) { $toggle.Toggle() }
    }
    try {
        $null = Find (Cond $A::NameProperty 'Avel')
        Start-Sleep -Seconds 2
        # The search box is the window's only editable text field.
        $null = Find (Cond $A::ControlTypeProperty $CT::Edit)
        $search = $A::RootElement.FindAll('Descendants', (All $inApp (Cond $A::ControlTypeProperty $CT::Edit))) |
            Where-Object { -not $_.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.IsReadOnly } |
            Select-Object -First 1
        $search.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($sample.program.name)
        Start-Sleep -Milliseconds 800
        $row = Find (All (Cond $A::NameProperty $sample.program.name) (Cond $A::IsSelectionItemPatternAvailableProperty $true))
        $row.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
        Start-Sleep -Milliseconds 500
        Click (Find (All (Cond $A::ControlTypeProperty $CT::Button) (Cond $A::NameProperty 'Uninstall')))
        Set-Check 'Uninstall silently where possible' $Quiet.IsPresent
        Set-Check 'Create a System Restore point first' $RestorePoint.IsPresent
        Set-Check 'Simulate only: show what would happen, change nothing' $false
        # The confirmation's OK button is also called "Uninstall", like the toolbar button behind it.
        $confirm = Find (All (Cond $A::ControlTypeProperty $CT::Window) (Cond $A::NameProperty "Uninstall $($sample.program.name)"))
        $ok = $confirm.FindFirst('Descendants', (All (Cond $A::ControlTypeProperty $CT::Button) (Cond $A::NameProperty 'Uninstall')))
        if (-not $ok) { throw 'no Uninstall button in the confirmation dialog' }
        Click $ok
        $null = Find (Cond $A::NameProperty 'Uninstalling')
        $close = Find (All (Cond $A::ControlTypeProperty $CT::Button) (Cond $A::NameProperty 'Close') (Cond $A::IsEnabledProperty $true)) 300
        $dialog = Find (Cond $A::NameProperty 'Uninstalling')
        $texts = @($dialog.FindAll('Descendants', (Cond $A::ControlTypeProperty $CT::Text)) | ForEach-Object { $_.Current.Name } | Where-Object { $_ })
        $cells = @($dialog.FindAll('Descendants', (Cond $A::ControlTypeProperty $CT::DataItem)) | ForEach-Object { $_.Current.Name })
        Log ('  {0,-5} {1,-45} GUI: {2} | {3}' -f $sample.kind, $sample.program.name, ($cells -join ' / '), ($texts -join ' / '))
        if (-not ($cells -match '^Removed')) { Fail "gui $($sample.kind): progress dialog shows $($cells -join ' / ')" }
        Click $close
        Start-Sleep -Seconds 3  # The main window refreshes its list
    } catch {
        Fail "gui $($sample.kind): $($_.Exception.Message)"
    } finally {
        Stop-Process -Id $gui.Id -ErrorAction SilentlyContinue
    }
}

try {
    Log "Avel Phase 2 uninstall scenario, $(Get-Date -Format s)"
    Log "OS: $([Environment]::OSVersion.VersionString), user: $env:USERNAME"
    Log "avel-cli: $((& $cli version) -join ' ')"

    # Round 1: interactive, one at a time.
    Install-All 'round 1'
    $stopFile = Join-Path $env:TEMP 'avel-clicker.stop'
    $clickLog = Join-Path $results 'clicks.log'
    Remove-Item $stopFile, $clickLog -ErrorAction SilentlyContinue
    $clicker = Start-Process powershell.exe -WindowStyle Minimized -PassThru -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSScriptRoot\clicker.ps1`"",
        '-StopFile', "`"$stopFile`"", '-LogFile', "`"$clickLog`"")
    Log 'Round 1: interactive uninstall, one program at a time'
    foreach ($sample in $samples | Where-Object { $_.program }) {
        $json = Invoke-Uninstall @($sample.program.id) @('--no-restore-point') "interactive-$($sample.kind)" 300
        if ($json) { Check-Result $sample $json.results[0] 'interactive' }
    }
    New-Item $stopFile -ItemType File | Out-Null
    if (-not $clicker.WaitForExit(10000)) { $clicker.Kill() }
    Check-Gone 'interactive'

    # Round 2: quiet batch of all five, with a restore point. System Restore is off in the sandbox
    # by default; turn it on, and lift the one-restore-point-per-day limit.
    Install-All 'round 2'
    try {
        Enable-ComputerRestore -Drive 'C:\'
        New-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\SystemRestore' `
            -Name SystemRestorePointCreationFrequency -Value 0 -PropertyType DWord -Force | Out-Null
        Log 'System Restore enabled for C:'
    } catch {
        Log "System Restore could not be enabled: $($_.Exception.Message)"
    }
    Log 'Round 2: quiet uninstall of all five in one batch'
    $ids = @($samples | Where-Object { $_.program } | ForEach-Object { $_.program.id })
    $json = Invoke-Uninstall $ids @('--quiet') 'quiet-batch' 900
    if ($json) {
        Log "  restore point: $($json.restore_point.status) (error $($json.restore_point.error))"
        $index = 0
        foreach ($sample in $samples | Where-Object { $_.program }) {
            Check-Result $sample $json.results[$index] 'quiet'
            $index++
        }
    }
    Check-Gone 'quiet'
    $points = @(Get-ComputerRestorePoint -ErrorAction SilentlyContinue | Where-Object { $_.Description -like 'Avel:*' })
    Log "  restore points created by Avel: $($points.Count)$(if ($points) { ' (' + $points[-1].Description + ')' })"

    # Round 3: the GUI, one program at a time, found through the search box.
    Log 'Round 3: uninstall through the GUI'
    foreach ($kind in 'msi', 'msix') {
        if ((Install-Sample $kind) -ne 0) { Fail "reinstalling $kind failed" }
    }
    Find-Samples (Get-Inventory)
    Invoke-GuiUninstall ($samples | Where-Object { $_.kind -eq 'msi' }) -Quiet -RestorePoint
    Invoke-GuiUninstall ($samples | Where-Object { $_.kind -eq 'msix' })
    $programs = Get-Inventory
    foreach ($kind in 'msi', 'msix') {
        $sample = $samples | Where-Object { $_.kind -eq $kind }
        if ($programs | Where-Object { $_.name -match $sample.pattern }) { Fail "gui $($kind): still in the inventory" }
    }
} catch {
    Fail "scenario error: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
    Log ''
    Log $(if ($failures -eq 0) { 'RESULT: PASS' } else { "RESULT: FAIL ($failures problem(s))" })
    $raw | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $results 'uninstall.json') -Encoding utf8
    # Written last and in one go: the host waits for this file.
    Set-Content -Path (Join-Path $results 'uninstall.txt') -Value $report -Encoding utf8
}
