# Dot-sourced by the removal scenario. "Contoso Fixture" is a scripted fake program whose
# uninstaller removes only its folder and its Uninstall key, leaving behind one leftover of every
# kind Avel removes; "Contoso Keeper" is a program of the same publisher that stays installed; the
# canaries are decoys named after the fixture in places Avel must never touch.

$fixtureRoot = 'C:\Program Files\Contoso Fixture'
$keeperRoot = 'C:\Program Files\Contoso Keeper'
$fixtureClsid = '{C0475F1C-0000-4A11-9C2B-5A4E2B1F0001}'
$fixtureService = 'ContosoFixtureService'
$fixtureTask = '\Contoso\Contoso Fixture Updater'
$startMenu = 'C:\ProgramData\Microsoft\Windows\Start Menu\Programs'
$runKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run'
$approvedKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved'
$uninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'

# What the fixture leaves behind, as avel_state_dump specs: removal must take all of it away and
# undo must bring it back exactly.
function Get-FixtureLeftoverSpecs {
    @(
        'file:C:\ProgramData\Contoso Fixture',
        "file:$env:APPDATA\Contoso Fixture",
        "file:$startMenu\Contoso Fixture",
        'key:HKLM\SOFTWARE\Contoso Fixture',
        'key:HKCU\Software\Contoso Fixture',
        # Values only: the samples' uninstallers change these shared keys meanwhile.
        'value:HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run|ContosoFixtureUpdater',
        "key:HKLM\SOFTWARE\Classes\CLSID\$fixtureClsid",
        'key:HKLM\SOFTWARE\Classes\*\shellex\ContextMenuHandlers\ContosoFixture',
        "value:HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved|$fixtureClsid",
        "service:$fixtureService",
        "task:$fixtureTask"
    )
}

# What must never change.
function Get-UntouchableSpecs {
    @(
        'file:C:\Windows\Contoso Fixture',
        'file:C:\ProgramData\Microsoft\Contoso Fixture',
        "file:$env:APPDATA\Microsoft\Contoso Fixture",
        'key:HKLM\SOFTWARE\Microsoft\Contoso Fixture',
        "file:$keeperRoot",
        'key:HKLM\SOFTWARE\Contoso Keeper'
    )
}

function New-Shortcut([string]$path, [string]$target) {
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($path)
    $link.TargetPath = $target
    $link.Save()
}

function Install-ContosoFixture {
    # The program itself: what its uninstaller removes.
    New-Item -ItemType Directory -Force $fixtureRoot | Out-Null
    Copy-Item "$env:WINDIR\System32\cmd.exe" "$fixtureRoot\app.exe" -Force
    Copy-Item "$env:WINDIR\System32\version.dll" "$fixtureRoot\shell.dll" -Force
    @'
param([switch]$Quiet)
Remove-Item 'C:\Program Files\Contoso Fixture' -Recurse -Force
Remove-Item 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ContosoFixture' -Recurse -Force
'@ | Set-Content "$fixtureRoot\uninstall.ps1" -Encoding utf8
    $uninstall = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$fixtureRoot\uninstall.ps1`""
    $key = New-Item "$uninstallKey\ContosoFixture" -Force
    $key | New-ItemProperty -Name DisplayName -Value 'Contoso Fixture' -Force | Out-Null
    $key | New-ItemProperty -Name DisplayVersion -Value '1.0' -Force | Out-Null
    $key | New-ItemProperty -Name Publisher -Value 'Contoso Ltd' -Force | Out-Null
    $key | New-ItemProperty -Name InstallLocation -Value $fixtureRoot -Force | Out-Null
    $key | New-ItemProperty -Name DisplayIcon -Value "$fixtureRoot\app.exe" -Force | Out-Null
    $key | New-ItemProperty -Name UninstallString -Value $uninstall -Force | Out-Null
    $key | New-ItemProperty -Name QuietUninstallString -Value "$uninstall -Quiet" -Force | Out-Null

    # What it leaves behind.
    New-Item -ItemType Directory -Force 'C:\ProgramData\Contoso Fixture' | Out-Null
    Set-Content 'C:\ProgramData\Contoso Fixture\settings.ini' "[fixture]`r`nlevel=3" -Encoding ascii
    New-Item -ItemType Directory -Force "$env:APPDATA\Contoso Fixture" | Out-Null
    Set-Content "$env:APPDATA\Contoso Fixture\config.json" '{"theme":"dark"}' -Encoding ascii
    New-Shortcut "$startMenu\Contoso Fixture\Contoso Fixture.lnk" "$fixtureRoot\app.exe"
    $software = New-Item 'HKLM:\SOFTWARE\Contoso Fixture' -Force
    $software | New-ItemProperty -Name InstallDir -Value $fixtureRoot -Force | Out-Null
    $software | New-ItemProperty -Name Version -Value '1.0' -Force | Out-Null
    New-Item 'HKLM:\SOFTWARE\Contoso Fixture\Settings' -Force | New-ItemProperty -Name Level -Value 3 -PropertyType DWord -Force | Out-Null
    New-Item 'HKCU:\Software\Contoso Fixture' -Force | New-ItemProperty -Name Theme -Value 'dark' -Force | Out-Null
    New-ItemProperty $runKey -Name 'ContosoFixtureUpdater' -Value "`"$fixtureRoot\app.exe`" /background" -Force | Out-Null
    $clsid = New-Item "HKLM:\SOFTWARE\Classes\CLSID\$fixtureClsid\InprocServer32" -Force
    Set-ItemProperty $clsid.PSPath -Name '(default)' -Value "$fixtureRoot\shell.dll"
    $clsid | New-ItemProperty -Name ThreadingModel -Value 'Apartment' -Force | Out-Null
    Set-ItemProperty "HKLM:\SOFTWARE\Classes\CLSID\$fixtureClsid" -Name '(default)' -Value 'Contoso Fixture shell extension'
    # Through .NET: the registry provider treats the "*" key's name as a wildcard.
    $handler = [Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\Classes\*\shellex\ContextMenuHandlers\ContosoFixture')
    $handler.SetValue('', $fixtureClsid)
    $handler.Close()
    New-ItemProperty $approvedKey -Name $fixtureClsid -Value 'Contoso Fixture shell extension' -Force | Out-Null
    if (-not (Get-Service $fixtureService -ErrorAction SilentlyContinue)) {
        New-Service -Name $fixtureService -BinaryPathName "`"$fixtureRoot\app.exe`" /service" `
            -DisplayName 'Contoso Fixture Service' -Description 'Keeps Contoso Fixture up to date.' `
            -StartupType Manual | Out-Null
    }
    $action = New-ScheduledTaskAction -Execute "$fixtureRoot\app.exe" -Argument '/update'
    $trigger = New-ScheduledTaskTrigger -Daily -At '03:00'
    Register-ScheduledTask -TaskPath '\Contoso\' -TaskName 'Contoso Fixture Updater' -Action $action -Trigger $trigger `
        -User 'SYSTEM' -Force | Out-Null
    if (-not (Get-NetFirewallRule -DisplayName 'Contoso Fixture' -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName 'Contoso Fixture' -Program "$fixtureRoot\app.exe" -Direction Inbound `
            -Action Allow | Out-Null
    }
}

function Install-Untouchables {
    foreach ($folder in 'C:\Windows\Contoso Fixture', 'C:\ProgramData\Microsoft\Contoso Fixture',
                        "$env:APPDATA\Microsoft\Contoso Fixture") {
        New-Item -ItemType Directory -Force $folder | Out-Null
        Set-Content "$folder\canary.txt" 'must stay' -Encoding ascii
    }
    New-Item 'HKLM:\SOFTWARE\Microsoft\Contoso Fixture' -Force | New-ItemProperty -Name Canary -Value 'must stay' -Force | Out-Null

    New-Item -ItemType Directory -Force $keeperRoot | Out-Null
    Copy-Item "$env:WINDIR\System32\cmd.exe" "$keeperRoot\keeper.exe" -Force
    $key = New-Item "$uninstallKey\ContosoKeeper" -Force
    $key | New-ItemProperty -Name DisplayName -Value 'Contoso Keeper' -Force | Out-Null
    $key | New-ItemProperty -Name Publisher -Value 'Contoso Ltd' -Force | Out-Null
    $key | New-ItemProperty -Name InstallLocation -Value $keeperRoot -Force | Out-Null
    $key | New-ItemProperty -Name UninstallString -Value "`"$keeperRoot\keeper.exe`" /uninstall" -Force | Out-Null
    New-Item 'HKLM:\SOFTWARE\Contoso Keeper' -Force | New-ItemProperty -Name InstallDir -Value $keeperRoot -Force | Out-Null
}
