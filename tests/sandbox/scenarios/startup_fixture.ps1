# Dot-sourced by the startup scenario (and the GUI round that may follow it). "Contoso Startup" is
# a fake program whose file starts with Windows from every source Avel's Startup manager lists, for
# the machine, the current user and a second local user who is signed out (their NTUSER.DAT is not
# loaded): Run values (the machine's 64- and 32-bit keys, the user's, the user's WOW6432Node key),
# RunOnce values, a RunOnceEx subkey, Startup folder files (common and per user), a scheduled task
# with a logon trigger and two automatic services (one delayed). A Run value that starts a file of
# Windows' own stands for the entries Avel must never change.
#
# Expects common.ps1 to be dot-sourced first (Fail, Log, Get-StateDump, Split-StateDump).

$startupRoot = 'C:\Program Files\Contoso Startup'
$startupExe = "$startupRoot\contoso.exe"
$startupTaskPath = '\Contoso Startup\'
$startupTaskName = 'Contoso Logon'
$startupService = 'ContosoStartupService'
$startupDelayed = 'ContosoStartupDelayed'
$otherUser = 'AvelOther'
# Where the scenario loads the signed-out user's hive while it writes fixtures to it or dumps it.
# Avel itself never sees it there: the hive is unloaded again before each avel-cli run.
$offlineMount = 'AvelStartupOffline'
$currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$otherSid = $null
$otherProfile = $null
$otherHive = $null

$machineCv = 'SOFTWARE\Microsoft\Windows\CurrentVersion'
$userCv = 'Software\Microsoft\Windows\CurrentVersion'
$commonStartup = 'C:\ProgramData\Microsoft\Windows\Start Menu\Programs\StartUp'
$userStartup = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Startup'

Add-Type -Namespace AvelTest -Name UserEnv -MemberDefinition @'
[DllImport("userenv.dll", CharSet = CharSet.Unicode)]
public static extern int CreateProfile(string sid, string userName, System.Text.StringBuilder profilePath, uint size);
'@

# Runs a command line through cmd.exe with its output thrown away; returns the exit code. Under
# 'Stop', Windows PowerShell turns a native program's stderr into an error, so it never sees it.
function Invoke-Quiet([string]$commandLine) {
    & cmd.exe /d /c "$commandLine >nul 2>&1"
    return $LASTEXITCODE
}

# Creates the second local user and their profile without signing them in: CreateProfile copies
# the Default profile and registers it in ProfileList, then unloads the new NTUSER.DAT again.
function New-OtherUser {
    $password = 'Av3l!' + [guid]::NewGuid().ToString('N').Substring(0, 12)
    try {
        New-LocalUser -Name $otherUser -Password (ConvertTo-SecureString $password -AsPlainText -Force) `
            -AccountNeverExpires -PasswordNeverExpires | Out-Null
    } catch {
        Log "  New-LocalUser failed ($($_.Exception.Message)); through ADSI"
        $user = ([ADSI]"WinNT://$env:COMPUTERNAME,computer").Create('User', $otherUser)
        $user.SetPassword($password)
        $user.SetInfo()
    }
    $script:otherSid = (New-Object Security.Principal.NTAccount($otherUser)).Translate(
        [Security.Principal.SecurityIdentifier]).Value
    $path = New-Object Text.StringBuilder 260
    $hr = [AvelTest.UserEnv]::CreateProfile($otherSid, $otherUser, $path, 260)
    if ($hr -ne 0) { throw ('CreateProfile for {0} returned 0x{1:X8}' -f $otherUser, $hr) }
    $script:otherProfile = $path.ToString()
    $script:otherHive = Join-Path $otherProfile 'NTUSER.DAT'
    if (-not (Test-Path $otherHive)) { throw "no NTUSER.DAT in $otherProfile" }
    # The profile service may hold the hive a moment longer.
    $deadline = (Get-Date).AddSeconds(60)
    while ((Test-Path "Registry::HKEY_USERS\$otherSid") -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
    Log "  user $otherUser ($otherSid), profile $otherProfile"
}

# Fails when the signed-out user's hive is loaded, by anyone: mounted under HKU, or held open
# (RegLoadAppKey keeps NTUSER.DAT open exclusively).
function Test-OtherHiveFree([string]$label) {
    if (-not $otherHive) { return }
    if (Test-Path "Registry::HKEY_USERS\$otherSid") {
        Fail "$($label): $otherUser's hive is loaded (HKU\$otherSid)"
        return
    }
    if (Test-Path "Registry::HKEY_USERS\$offlineMount") {
        Fail "$($label): $otherUser's hive is still mounted by the scenario"
        return
    }
    try {
        $stream = [IO.File]::Open($otherHive, 'Open', 'Read', 'None')
        $stream.Dispose()
    } catch {
        Fail "$($label): $otherUser's NTUSER.DAT is in use: $($_.Exception.Message)"
    }
}

function Mount-OtherHive {
    $code = Invoke-Quiet "reg.exe load HKU\$offlineMount `"$otherHive`""
    if ($code -ne 0) { throw "reg load of $otherHive returned $code" }
}

function Dismount-OtherHive {
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        [GC]::Collect()
        [GC]::WaitForPendingFinalizers()
        if ((Invoke-Quiet "reg.exe unload HKU\$offlineMount") -eq 0) { return }
        Start-Sleep -Milliseconds 500
    }
    throw "reg unload HKU\$offlineMount failed"
}

# Sets a value through .NET, whose key handles close on Dispose (the registry provider's may stay
# open and keep a mounted hive from unloading).
function Set-RegistryValue([Microsoft.Win32.RegistryKey]$root, [string]$path, [string]$name, $value,
                           [Microsoft.Win32.RegistryValueKind]$kind = 'String') {
    $key = $root.CreateSubKey($path)
    try { $key.SetValue($name, $value, $kind) } finally { $key.Dispose() }
}

function New-StartupShortcut([string]$path, [string]$target, [string]$arguments) {
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($path)
    $link.TargetPath = $target
    $link.Arguments = $arguments
    $link.Save()
}

# A file of Windows' own in System32, for the entry Avel must refuse to change.
function Get-WindowsOwnFile {
    foreach ($name in 'SecurityHealthSystray.exe', 'ctfmon.exe', 'notepad.exe') {
        if (Test-Path "$env:windir\System32\$name") { return $name }
    }
    throw 'no file of Windows own found in System32'
}

# The fixtures, as the list must show them. kind/name/sid/wow64 find the entry; toggle says how it
# is disabled ('approval': a StartupApproved value, 'task', 'service', '': delete only); item and
# approval are avel_state_dump specs (the signed-out user's under HKU\$offlineMount); approvalKey
# ends the entry's StartupApproved key; target is the file it runs.
function Get-StartupFixtures {
    $off = "HKU\$offlineMount"
    $otherStartup = Join-Path $otherProfile 'AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup'
    $windowsOwn = Get-WindowsOwnFile
    function F($label, $kind, $owner, $name, $toggle, $item, $approval, $approvalKey, $target, [bool]$wow64 = $false,
               [string]$protection = 'none') {
        $sid = switch ($owner) { 'machine' { '' } 'current' { $currentSid } 'other' { $otherSid } }
        [pscustomobject]@{ label = $label; kind = $kind; owner = $owner; sid = $sid; name = $name; toggle = $toggle
                           item = $item; approval = $approval; approvalKey = $approvalKey; target = $target
                           wow64 = $wow64; protection = $protection; id = $null }
    }
    @(
        F 'run-machine' 'run' 'machine' 'ContosoStartupMachine' 'approval' `
            "value:HKLM\$machineCv\Run|ContosoStartupMachine" `
            "value:HKLM\$machineCv\Explorer\StartupApproved\Run|ContosoStartupMachine" 'StartupApproved\Run' $startupExe
        F 'run-machine32' 'run' 'machine' 'ContosoStartup32' 'approval' `
            "value:HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run|ContosoStartup32" `
            "value:HKLM\$machineCv\Explorer\StartupApproved\Run32|ContosoStartup32" 'StartupApproved\Run32' $startupExe $true
        F 'run-user' 'run' 'current' 'ContosoStartupUser' 'approval' `
            "value:HKCU\$userCv\Run|ContosoStartupUser" `
            "value:HKCU\$userCv\Explorer\StartupApproved\Run|ContosoStartupUser" 'StartupApproved\Run' $startupExe
        F 'run-user32' 'run' 'current' 'ContosoStartupUser32' '' `
            "value:HKCU\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Run|ContosoStartupUser32" `
            $null $null $startupExe $true
        F 'run-other' 'run' 'other' 'ContosoStartupOther' 'approval' `
            "value:$off\$userCv\Run|ContosoStartupOther" `
            "value:$off\$userCv\Explorer\StartupApproved\Run|ContosoStartupOther" 'StartupApproved\Run' $startupExe
        F 'runonce-machine' 'run-once' 'machine' 'ContosoRunOnce' '' "value:HKLM\$machineCv\RunOnce|ContosoRunOnce" `
            $null $null $startupExe
        F 'runonce-user' 'run-once' 'current' 'ContosoRunOnceUser' '' "value:HKCU\$userCv\RunOnce|ContosoRunOnceUser" `
            $null $null $startupExe
        F 'runonceex' 'run-once-ex' 'machine' 'ContosoStartup' '' "key:HKLM\$machineCv\RunOnceEx\ContosoStartup" `
            $null $null $startupExe
        F 'folder-common' 'startup-folder' 'machine' 'Contoso Common.lnk' 'approval' "file:$commonStartup\Contoso Common.lnk" `
            "value:HKLM\$machineCv\Explorer\StartupApproved\StartupFolder|Contoso Common.lnk" 'StartupApproved\StartupFolder' $startupExe
        F 'folder-user' 'startup-folder' 'current' 'Contoso User.lnk' 'approval' "file:$userStartup\Contoso User.lnk" `
            "value:HKCU\$userCv\Explorer\StartupApproved\StartupFolder|Contoso User.lnk" 'StartupApproved\StartupFolder' $startupExe
        F 'folder-user-cmd' 'startup-folder' 'current' 'Contoso Script.cmd' 'approval' "file:$userStartup\Contoso Script.cmd" `
            "value:HKCU\$userCv\Explorer\StartupApproved\StartupFolder|Contoso Script.cmd" 'StartupApproved\StartupFolder' `
            "$userStartup\Contoso Script.cmd"
        F 'folder-other' 'startup-folder' 'other' 'Contoso Other.lnk' 'approval' "file:$otherStartup\Contoso Other.lnk" `
            "value:$off\$userCv\Explorer\StartupApproved\StartupFolder|Contoso Other.lnk" 'StartupApproved\StartupFolder' $startupExe
        F 'task' 'task' 'machine' "$startupTaskPath$startupTaskName" 'task' "task:$startupTaskPath$startupTaskName" `
            $null $null $startupExe
        F 'service' 'service' 'machine' $startupService 'service' "service:$startupService" $null $null $startupExe
        F 'service-delayed' 'service' 'machine' $startupDelayed 'service' "service:$startupDelayed" $null $null $startupExe
        F 'windows-own' 'run' 'machine' 'ContosoWindowsOwn' 'approval' "value:HKLM\$machineCv\Run|ContosoWindowsOwn" `
            "value:HKLM\$machineCv\Explorer\StartupApproved\Run|ContosoWindowsOwn" 'StartupApproved\Run' `
            "$env:windir\System32\$windowsOwn" $false 'windows-own'
    )
}

function Get-FixtureSpecs($fixtures) {
    @($fixtures | ForEach-Object { $_.item; $_.approval } | Where-Object { $_ })
}

# avel_state_dump of `specs`, split into sections; mounts the signed-out user's hive while it runs.
function Get-StartupDump([string[]]$specs) {
    $mount = @($specs | Where-Object { $_ -match "HKU\\$offlineMount\\" }).Count -gt 0
    if ($mount) { Mount-OtherHive }
    try {
        return Split-StateDump (Get-StateDump $specs)
    } finally {
        if ($mount) { Dismount-OtherHive }
    }
}

function Install-StartupFixtures {
    New-Item -ItemType Directory -Force $startupRoot | Out-Null
    # An unsigned file: Windows' own services aren't listed at all.
    Copy-Item (Join-Path (Split-Path $cli) 'avel_test_spawner.exe') $startupExe -Force
    $uninstall = Join-Path $machineCv 'Uninstall\ContosoStartup'
    $hklm = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    Set-RegistryValue $hklm $uninstall 'DisplayName' 'Contoso Startup'
    Set-RegistryValue $hklm $uninstall 'Publisher' 'Contoso Ltd'
    Set-RegistryValue $hklm $uninstall 'InstallLocation' $startupRoot
    Set-RegistryValue $hklm $uninstall 'UninstallString' "`"$startupExe`" uninstall"

    $command = "`"$startupExe`" sleep 1000"
    Set-RegistryValue $hklm "$machineCv\Run" 'ContosoStartupMachine' $command
    Set-RegistryValue $hklm 'SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run' 'ContosoStartup32' $command
    Set-RegistryValue $hklm "$machineCv\RunOnce" 'ContosoRunOnce' $command
    Set-RegistryValue $hklm "$machineCv\RunOnceEx\ContosoStartup" '' 'Contoso Startup setup'
    Set-RegistryValue $hklm "$machineCv\RunOnceEx\ContosoStartup" '1' "||$command"
    Set-RegistryValue $hklm "$machineCv\Run" 'ContosoWindowsOwn' "%windir%\system32\$(Get-WindowsOwnFile)" ExpandString

    $hkcu = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', 'Registry64')
    Set-RegistryValue $hkcu "$userCv\Run" 'ContosoStartupUser' $command
    Set-RegistryValue $hkcu 'Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Run' 'ContosoStartupUser32' $command
    Set-RegistryValue $hkcu "$userCv\RunOnce" 'ContosoRunOnceUser' $command

    New-StartupShortcut "$commonStartup\Contoso Common.lnk" $startupExe 'sleep 1000'
    New-StartupShortcut "$userStartup\Contoso User.lnk" $startupExe 'sleep 1000'
    Set-Content "$userStartup\Contoso Script.cmd" "@start `"`" `"$startupExe`" sleep 1000" -Encoding ascii
    $otherStartup = Join-Path $otherProfile 'AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup'
    New-StartupShortcut "$otherStartup\Contoso Other.lnk" $startupExe 'sleep 1000'

    Mount-OtherHive
    try {
        Set-RegistryValue ([Microsoft.Win32.Registry]::Users) "$offlineMount\$userCv\Run" 'ContosoStartupOther' $command
    } finally {
        Dismount-OtherHive
    }

    $action = New-ScheduledTaskAction -Execute $startupExe -Argument 'sleep 1000'
    $trigger = New-ScheduledTaskTrigger -AtLogOn
    Register-ScheduledTask -TaskPath $startupTaskPath -TaskName $startupTaskName -Action $action -Trigger $trigger `
        -User 'SYSTEM' -Force | Out-Null
    foreach ($name in $startupService, $startupDelayed) {
        New-Service -Name $name -BinaryPathName "`"$startupExe`" sleep 1000" -DisplayName "Contoso Startup ($name)" `
            -Description 'Starts Contoso Startup.' -StartupType Automatic | Out-Null
    }
    $code = Invoke-Quiet "sc.exe config $startupDelayed start= delayed-auto"
    if ($code -ne 0) { throw "sc config $startupDelayed returned $code" }
}
