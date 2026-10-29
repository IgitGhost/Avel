# Dot-sourced by the trace scenario. "Contoso Trace Fixture" is a scripted fake installer. Its
# install step is a separate script run as its own process (so `avel-cli trace run` can launch
# it). It creates one item of every kind Avel removes (a folder, an Uninstall key, HKLM and HKCU
# keys, a shortcut, a Run value, App Paths, a COM class with a context-menu handler, a service, a
# task in a new task folder and a firewall rule), edits values in the spots the tracker saves
# exactly (it appends to the machine PATH, REG_EXPAND_SZ, and the user's, creates CONTOSO_HOME,
# raises a shared DLL's SharedDLLs count from 1 to 2 and creates its own) and changes one unrelated
# value (Theme). Its uninstaller removes only its folder and its Uninstall key, like many real
# ones. Some of what it finds is already there: the shared DLL, counted once by another program,
# and the key whose Theme it changes; Initialize-TraceFixture puts those in place, untraced.

$traceRoot = 'C:\Program Files\Contoso Trace'
$traceSetup = 'C:\ContosoTraceSetup'
$traceClsid = '{C0475F1C-0000-4A11-9C2B-5A4E2B1F0002}'
$traceService = 'ContosoTraceService'
$traceTask = '\Contoso Trace\Updater'
$traceRule = 'ContosoTrace-App-In'
$traceShared = 'C:\Program Files\Common Files\Contoso\shared.dll'
$traceStartMenu = 'C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Contoso Trace'
$traceUserSid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$traceMachineEnvironment = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
$traceSharedDlls = 'SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs'

# The installer. It runs as its own PowerShell process, so it is written out by
# Initialize-TraceFixture; `-RunAfter` starts the app at the end, detached, like "Run X now".
$traceInstallScript = @'
param([switch]$RunAfter)
# Contoso Trace Fixture's installer, written by tests/sandbox/scenarios/trace_fixture.ps1.
$ErrorActionPreference = 'Stop'
$root = 'C:\Program Files\Contoso Trace'
$clsid = '{C0475F1C-0000-4A11-9C2B-5A4E2B1F0002}'
$machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
$user = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', 'Registry64')

function Set-Values($key, [hashtable]$values) {
    foreach ($name in $values.Keys) { $key.SetValue($name, $values[$name]) }
    $key.Close()
}

# Appends an entry to a PATH value, keeping it REG_EXPAND_SZ with the other entries unexpanded.
function Add-PathEntry($key, [string]$entry) {
    $old = $key.GetValue('Path', $null, 'DoNotExpandEnvironmentNames')
    $new = if (-not $old) { $entry } elseif ($old.EndsWith(';')) { $old + $entry } else { $old + ';' + $entry }
    $key.SetValue('Path', $new, 'ExpandString')
    $key.Close()
}

# The program.
New-Item -ItemType Directory -Force "$root\bin" | Out-Null
Copy-Item "$env:WINDIR\System32\cmd.exe" "$root\app.exe"
Copy-Item "$env:WINDIR\System32\version.dll" "$root\shell.dll"
Copy-Item "$env:WINDIR\System32\where.exe" "$root\bin\tool.exe"
Set-Content "$root\uninstall.ps1" -Encoding ascii -Value @(
    "Remove-Item 'C:\Program Files\Contoso Trace' -Recurse -Force",
    "Remove-Item 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso Trace' -Recurse -Force")
Set-Values $machine.CreateSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso Trace') @{
    DisplayName = 'Contoso Trace'; DisplayVersion = '2.0'; Publisher = 'Contoso'; InstallLocation = $root
    DisplayIcon = "$root\app.exe"
    UninstallString = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + $root + '\uninstall.ps1"' }

# Its data and settings.
Set-Values $machine.CreateSubKey('SOFTWARE\Contoso Trace') @{ InstallDir = $root; Version = '2.0' }
$settings = $machine.CreateSubKey('SOFTWARE\Contoso Trace\Settings')
$settings.SetValue('Level', 3, 'DWord')
$settings.Close()
Set-Values $user.CreateSubKey('Software\Contoso Trace') @{ Theme = 'dark' }
New-Item -ItemType Directory -Force 'C:\ProgramData\Contoso Trace', "$env:APPDATA\Contoso Trace" | Out-Null
Set-Content 'C:\ProgramData\Contoso Trace\settings.ini' "[trace]`r`nlevel=3" -Encoding ascii
Set-Content "$env:APPDATA\Contoso Trace\user.ini" "[user]`r`ntheme=dark" -Encoding ascii

# Its integration with Windows.
$menu = 'C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Contoso Trace'
New-Item -ItemType Directory -Force $menu | Out-Null
$link = (New-Object -ComObject WScript.Shell).CreateShortcut("$menu\Contoso Trace.lnk")
$link.TargetPath = "$root\app.exe"
$link.Save()
Set-Values $machine.CreateSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Run') @{
    ContosoTraceUpdater = '"' + $root + '\app.exe" /background' }
Set-Values $machine.CreateSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\ContosoTrace.exe') @{
    '' = "$root\app.exe"; Path = $root }
Set-Values $machine.CreateSubKey("SOFTWARE\Classes\CLSID\$clsid") @{ '' = 'Contoso Trace shell extension' }
Set-Values $machine.CreateSubKey("SOFTWARE\Classes\CLSID\$clsid\InprocServer32") @{
    '' = "$root\shell.dll"; ThreadingModel = 'Apartment' }
Set-Values $machine.CreateSubKey('SOFTWARE\Classes\*\shellex\ContextMenuHandlers\ContosoTrace') @{ '' = $clsid }
Set-Values $machine.CreateSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved') @{
    $clsid = 'Contoso Trace shell extension' }
$sc = Start-Process sc.exe -Wait -PassThru -NoNewWindow -ArgumentList ('create ContosoTraceService binPath= "\"' +
    $root + '\app.exe\" /service" start= demand DisplayName= "Contoso Trace Service"')
if ($sc.ExitCode -ne 0) { throw "sc.exe create returned $($sc.ExitCode)" }
$action = New-ScheduledTaskAction -Execute "$root\app.exe" -Argument '/update'
$trigger = New-ScheduledTaskTrigger -Daily -At '03:00'
Register-ScheduledTask -TaskPath '\Contoso Trace\' -TaskName 'Updater' -Action $action -Trigger $trigger -User 'SYSTEM' `
    -Force | Out-Null
New-NetFirewallRule -Name 'ContosoTrace-App-In' -DisplayName 'Contoso Trace' -Program "$root\app.exe" `
    -Direction Inbound -Action Allow | Out-Null

# Values in the spots the tracker saves exactly, and an unrelated one.
$environment = $machine.OpenSubKey('SYSTEM\CurrentControlSet\Control\Session Manager\Environment', $true)
$environment.SetValue('CONTOSO_HOME', $root, 'String')
Add-PathEntry $environment "$root\bin"
Add-PathEntry $user.CreateSubKey('Environment') "$root\bin"
$shared = $machine.CreateSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs')
$count = [int]$shared.GetValue('C:\Program Files\Common Files\Contoso\shared.dll', 0)
$shared.SetValue('C:\Program Files\Common Files\Contoso\shared.dll', $count + 1, 'DWord')
$shared.SetValue("$root\shell.dll", 1, 'DWord')
$shared.Close()
Set-Values $machine.OpenSubKey('SOFTWARE\Contoso Shared', $true) @{ Theme = 'dark' }

if ($RunAfter) { Start-Process "$root\app.exe" '/k' -WindowStyle Hidden }
exit 0
'@

# Untraced: writes the installer and puts in place what it finds on the system.
function Initialize-TraceFixture {
    New-Item -ItemType Directory -Force $traceSetup, (Split-Path $traceShared) | Out-Null
    Set-Content "$traceSetup\install.ps1" $traceInstallScript -Encoding ascii
    Copy-Item "$env:WINDIR\System32\version.dll" $traceShared -Force
    $machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    $shared = $machine.CreateSubKey($traceSharedDlls)
    $shared.SetValue($traceShared, 1, 'DWord')
    $shared.Close()
    $contoso = $machine.CreateSubKey('SOFTWARE\Contoso Shared')
    $contoso.SetValue('Theme', 'light')
    $contoso.Close()
}

# The installer's command line, for `trace run` (or Start-Process): the program, then its arguments.
function Get-TraceFixtureCommand([switch]$RunAfter) {
    $command = @("$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe", '-NoProfile', '-ExecutionPolicy', 'Bypass',
                 '-File', "$traceSetup\install.ps1")
    if ($RunAfter) { $command += '-RunAfter' }
    return $command
}

# What the install creates or changes, as the trace must report it: change, location kind and the
# location as Where-Text (common.ps1) spells it.
function Get-TraceFixtureItems {
    $windows = 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion'
    @(
        @{ change = 'created'; kind = 'folder'; where = $traceRoot },
        @{ change = 'created'; kind = 'registry-key'; where = "$windows\Uninstall\Contoso Trace" },
        @{ change = 'created'; kind = 'registry-key'; where = 'HKLM\SOFTWARE\Contoso Trace' },
        @{ change = 'created'; kind = 'registry-key'; where = "HKU\$traceUserSid\Software\Contoso Trace" },
        @{ change = 'created'; kind = 'folder'; where = 'C:\ProgramData\Contoso Trace' },
        @{ change = 'created'; kind = 'folder'; where = "$env:APPDATA\Contoso Trace" },
        @{ change = 'created'; kind = 'folder'; where = $traceStartMenu },
        @{ change = 'created'; kind = 'registry-value'; where = "$windows\Run : ContosoTraceUpdater" },
        @{ change = 'created'; kind = 'registry-key'; where = "$windows\App Paths\ContosoTrace.exe" },
        @{ change = 'created'; kind = 'registry-key'; where = "HKLM\SOFTWARE\Classes\CLSID\$traceClsid" },
        @{ change = 'created'; kind = 'registry-key'; where = 'HKLM\SOFTWARE\Classes\*\shellex\ContextMenuHandlers\ContosoTrace' },
        @{ change = 'created'; kind = 'registry-value'; where = "$windows\Shell Extensions\Approved : $traceClsid" },
        @{ change = 'created'; kind = 'service'; where = $traceService },
        @{ change = 'created'; kind = 'task'; where = $traceTask },
        @{ change = 'created'; kind = 'task-folder'; where = (Split-Path $traceTask) },
        @{ change = 'created'; kind = 'firewall-rule'; where = $traceRule },
        @{ change = 'modified'; kind = 'registry-value'; where = 'HKLM\SOFTWARE\Contoso Shared : Theme' }
    )
}

# The values the install changes in the known spots. `append`: a PATH entry added to the value's
# text; otherwise `before`/`after` are the data as hex ($null: no value) or `after_text` the text.
function Get-TraceFixtureKnownValues {
    $environment = "HKLM\$traceMachineEnvironment"
    $shared = "HKLM\$traceSharedDlls"
    @(
        @{ spot = 'machine-environment'; key = $environment; name = 'Path'; append = "$traceRoot\bin" },
        @{ spot = 'user-environment'; key = "HKU\$traceUserSid\Environment"; name = 'Path'; append = "$traceRoot\bin" },
        @{ spot = 'machine-environment'; key = $environment; name = 'CONTOSO_HOME'; before = $null; after_text = $traceRoot },
        @{ spot = 'shared-dlls'; key = $shared; name = $traceShared; before = '01000000'; after = '02000000' },
        @{ spot = 'shared-dlls'; key = $shared; name = "$traceRoot\shell.dll"; before = $null; after = '01000000' }
    )
}

# The firewall rule's id, the name of its value under FirewallRules: the -Name the installer gives
# New-NetFirewallRule, unless Windows named the value otherwise.
function Get-TraceFixtureRuleId {
    $rules = Get-Item 'HKLM:\SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\FirewallRules' `
        -ErrorAction SilentlyContinue
    if ($rules) {
        foreach ($name in $rules.GetValueNames()) {
            if ("$($rules.GetValue($name))" -match '\|Name=Contoso Trace\|') { return $name }
        }
    }
    return $traceRule
}

# The PATH values as they are now, unexpanded: what the trace must save as their `before_text`.
function Get-TraceFixturePaths {
    $machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    $user = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', 'Registry64')
    $paths = @{}
    foreach ($entry in @(@('machine-environment', $machine, $traceMachineEnvironment), @('user-environment', $user, 'Environment'))) {
        $key = $entry[1].OpenSubKey($entry[2])
        $paths[$entry[0]] = if ($key) { $key.GetValue('Path', $null, 'DoNotExpandEnvironmentNames') } else { $null }
        if ($key) { $key.Close() }
    }
    return $paths
}

# Takes away everything the install created and undoes its value edits, so the next install starts
# fresh. Leaves Initialize-TraceFixture's part in place.
function Remove-TraceFixture {
    Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith("$traceRoot\", 'OrdinalIgnoreCase') } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    if (Get-Service $traceService -ErrorAction SilentlyContinue) {
        $null = Start-Process sc.exe "delete $traceService" -Wait -PassThru -WindowStyle Hidden
    }
    $scheduler = New-Object -ComObject Schedule.Service
    $scheduler.Connect()
    try { $scheduler.GetFolder('\Contoso Trace').DeleteTask('Updater', 0) } catch { }
    try { $scheduler.GetFolder('\').DeleteFolder('Contoso Trace', 0) } catch { }
    Get-NetFirewallRule -Name $traceRule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    foreach ($folder in $traceRoot, 'C:\ProgramData\Contoso Trace', "$env:APPDATA\Contoso Trace", $traceStartMenu) {
        if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force }
    }
    $machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    $user = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', 'Registry64')
    # Through .NET: the registry provider reads the "*" key's name as a wildcard.
    foreach ($key in 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso Trace', 'SOFTWARE\Contoso Trace',
                     'SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\ContosoTrace.exe', "SOFTWARE\Classes\CLSID\$traceClsid",
                     'SOFTWARE\Classes\*\shellex\ContextMenuHandlers\ContosoTrace') {
        $machine.DeleteSubKeyTree($key, $false)
    }
    $user.DeleteSubKeyTree('Software\Contoso Trace', $false)
    foreach ($value in @(@('SOFTWARE\Microsoft\Windows\CurrentVersion\Run', 'ContosoTraceUpdater'),
                         @('SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved', $traceClsid),
                         @($traceSharedDlls, "$traceRoot\shell.dll"),
                         @($traceMachineEnvironment, 'CONTOSO_HOME'))) {
        $key = $machine.OpenSubKey($value[0], $true)
        if ($key) { $key.DeleteValue($value[1], $false); $key.Close() }
    }
    foreach ($key in @($machine.OpenSubKey($traceMachineEnvironment, $true), $user.OpenSubKey('Environment', $true))) {
        if (-not $key) { continue }
        $path = $key.GetValue('Path', $null, 'DoNotExpandEnvironmentNames')
        if ($path) {
            $kept = @($path -split ';' | Where-Object { $_ -ne "$traceRoot\bin" })
            if ($kept.Count -ne @($path -split ';').Count) { $key.SetValue('Path', ($kept -join ';'), 'ExpandString') }
        }
        $key.Close()
    }
}
