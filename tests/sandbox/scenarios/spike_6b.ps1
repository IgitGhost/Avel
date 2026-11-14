# Runs inside Windows Sandbox (Start-AvelSandbox.ps1 -Scenario spike_6b, which starts it without
# networking). The Phase 6b spike (docs/adr/0008): what works offline for removing packaged apps
# for some users, undoing that, deprovisioning, and Windows features and capabilities through
# DismAPI. It is exploratory: it records what happened rather than judging it, and fails only when
# its own infrastructure breaks. Every finding that matters is made twice where it can be: through
# the Appx/DISM cmdlets, and through avel_package_probe (tests/helpers/package_probe.cpp), which
# makes the public calls Avel itself would make (PackageManager, DismAPI).
#   0. Inventory: the image's packages (all users, provisioned), the Appx services, the
#      AppxAllUserStore and the StateRepository's tables (read from a copy with winsqlite3).
#   1. A signed-out second user, AvelOther (New-LocalUser + CreateProfile, as the startup
#      scenario), and running the probe as them through scheduled tasks: S4U (no password, what
#      Avel could do) and with their password.
#   2. The test MSIX (unsigned) for the current user (AddPackageByUriAsync) and for AvelOther (a
#      task running the probe as them), with a marker file in each user's package data.
#   3. Removal for AvelOther alone while signed out: Remove-AppxPackage -User, the probe's
#      RemovePackageAsync run as them (with and without PreserveApplicationData),
#      CleanupPackageForUserAsync; each undone by registering again as them.
#   4. Removal for the current user while AvelOther keeps the package, its undo
#      (RegisterPackageByFullNameAsync), then for the last user: is the staged package
#      garbage-collected, and when; RemoveForAllUsers.
#   5. Provisioning: the test MSIX staged without users, provisioned (public API, then the
#      cmdlet), removed for a user, deprovisioned (public API and cmdlet), provisioned again.
#   6. An inbox package that is provisioned: soft removal for each user and its undo, deprovision
#      and re-provision, and RemoveForAllUsers last.
#   7. DismAPI: features and capabilities, an enable/disable round trip each way, a feature whose
#      payload is removed, a capability removed and added back, and one never present; pending
#      restarts.
#   8. What a NonRemovable package's removal returns.
# Every step logs the package's users (FindUsers), whether it is staged (WindowsApps) and
# provisioned, each user's data folder and marker, and the AppxAllUserStore keys and StateRepository
# rows that changed. Writes results\spike_6b.json (every probe's output), results\spike_6b.log,
# results\spike_6b-dism.log and -cbs.log (tails of DISM's and CBS's logs) and, last,
# results\spike_6b.txt.

$ErrorActionPreference = 'Stop'
$bin = 'C:\Avel\bin'
$results = 'C:\Avel\results'
$cli = Join-Path $bin 'avel-cli.exe'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$scenario = 'spike_6b'
. "$PSScriptRoot\common.ps1"

$testMsix = 'C:\Avel\sandbox\installers\AvelTestPackage.msix'
$work = 'C:\AvelSpike'
$probeDir = "$work\bin"
$outDir = "$work\out"
$probe = "$probeDir\avel_package_probe.exe"
$otherUser = 'AvelOther'
$otherPassword = 'Av3l!' + [guid]::NewGuid().ToString('N').Substring(0, 12)
$otherSid = $null
$otherProfile = $null
$currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$storeRoot = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Appx\AppxAllUserStore'
$srdSource = 'C:\ProgramData\Microsoft\Windows\AppRepository\StateRepository-Machine.srd'
$callNumber = 0
$findings = [ordered]@{}
$raw['findings'] = $findings
$storeLines = @()
$srdLines = @{}
$srdWorks = $false
$srdExtraTables = @()
$s4uWorks = $false
$otherRegister = $null   # How the package gets registered for AvelOther: @{ logon; unsigned }
$test = $null            # The test package: full, family, name
$inbox = $null           # The inbox package of round 6

Add-Type -Namespace AvelSpike -Name UserEnv -MemberDefinition @'
[DllImport("userenv.dll", CharSet = CharSet.Unicode)]
public static extern int CreateProfile(string sid, string userName, System.Text.StringBuilder profilePath, uint size);
'@

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
namespace AvelSpike {
public static class Sqlite {
    [DllImport("winsqlite3.dll", CharSet = CharSet.Unicode)] static extern int sqlite3_open16(string name, out IntPtr db);
    [DllImport("winsqlite3.dll")] static extern int sqlite3_close(IntPtr db);
    [DllImport("winsqlite3.dll", CharSet = CharSet.Unicode)]
    static extern int sqlite3_prepare16_v2(IntPtr db, string sql, int bytes, out IntPtr stmt, IntPtr tail);
    [DllImport("winsqlite3.dll")] static extern int sqlite3_step(IntPtr stmt);
    [DllImport("winsqlite3.dll")] static extern int sqlite3_finalize(IntPtr stmt);
    [DllImport("winsqlite3.dll")] static extern int sqlite3_column_count(IntPtr stmt);
    [DllImport("winsqlite3.dll")] static extern int sqlite3_column_type(IntPtr stmt, int i);
    [DllImport("winsqlite3.dll")] static extern IntPtr sqlite3_column_name16(IntPtr stmt, int i);
    [DllImport("winsqlite3.dll")] static extern IntPtr sqlite3_column_text16(IntPtr stmt, int i);
    [DllImport("winsqlite3.dll")] static extern IntPtr sqlite3_column_blob(IntPtr stmt, int i);
    [DllImport("winsqlite3.dll")] static extern int sqlite3_column_bytes(IntPtr stmt, int i);
    [DllImport("winsqlite3.dll")] static extern IntPtr sqlite3_errmsg16(IntPtr db);

    // Each row as "column=value; ...". Blobs are shown as hex (up to 64 bytes).
    public static List<string> Query(string path, string sql, int max) {
        var rows = new List<string>();
        IntPtr db;
        if (sqlite3_open16(path, out db) != 0) { rows.Add("error: open"); return rows; }
        try {
            IntPtr stmt;
            if (sqlite3_prepare16_v2(db, sql, -1, out stmt, IntPtr.Zero) != 0) {
                rows.Add("error: " + Marshal.PtrToStringUni(sqlite3_errmsg16(db)));
                return rows;
            }
            try {
                int count = sqlite3_column_count(stmt);
                int rc;
                while ((rc = sqlite3_step(stmt)) == 100 && rows.Count < max) {
                    var text = new StringBuilder();
                    for (int i = 0; i < count; i++) {
                        if (i > 0) text.Append("; ");
                        text.Append(Marshal.PtrToStringUni(sqlite3_column_name16(stmt, i))).Append('=');
                        int type = sqlite3_column_type(stmt, i);
                        if (type == 4) {
                            int n = Math.Min(sqlite3_column_bytes(stmt, i), 64);
                            IntPtr blob = sqlite3_column_blob(stmt, i);
                            for (int b = 0; b < n; b++) text.Append(Marshal.ReadByte(blob, b).ToString("x2"));
                        } else if (type != 5) {
                            text.Append(Marshal.PtrToStringUni(sqlite3_column_text16(stmt, i)));
                        }
                    }
                    rows.Add(text.ToString());
                }
                if (rc != 100 && rc != 101) rows.Add("error: step " + rc + " " + Marshal.PtrToStringUni(sqlite3_errmsg16(db)));
            } finally { sqlite3_finalize(stmt); }
        } finally { sqlite3_close(db); }
        return rows;
    }
}
}
'@

function Invoke-Round([string]$name, [scriptblock]$body) {
    Log ''
    Log $name
    $started = Get-Date
    try {
        & $body
    } catch {
        Fail "$($name): $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
    }
    Log ('  ({0:N0} s)' -f ((Get-Date) - $started).TotalSeconds)
}

function Note([string]$key, [string]$text) {
    $findings[$key] = $text
    Log "  FINDING $($key): $text"
}

function Quote-Argument([string]$argument) {
    if ($argument -eq '' -or $argument -match '[\s"]') { return '"' + ($argument -replace '"', '\"') + '"' }
    return $argument
}

# One line for a probe's result.
function Format-Probe($json) {
    if (-not $json) { return 'no output' }
    $parts = @("ok=$($json.ok)")
    foreach ($name in 'hr', 'error_code', 'extended_error', 'is_registered', 'reboot_required', 'elapsed_ms',
                      'call_ms', 'stage') {
        if ($null -ne $json.$name -and "$($json.$name)" -ne '') { $parts += "$name=$($json.$name)" }
    }
    $text = $parts -join ' '
    if ($json.error_text) { $text += " text='" + ($json.error_text -replace '\s+', ' ').Trim() + "'" }
    if ($json.message) { $text += " message='" + ($json.message -replace '\s+', ' ').Trim() + "'" }
    return $text
}

# Runs the probe as the current user (elevated); returns its JSON (or $null).
function Invoke-Probe([string[]]$arguments, [string]$label, [int]$timeoutSeconds = 900) {
    $script:callNumber++
    $tag = '{0:000}-{1}' -f $script:callNumber, $label
    $out = Join-Path $outDir "$tag.json"
    $err = Join-Path $outDir "$tag.err"
    $line = ($arguments | ForEach-Object { Quote-Argument $_ }) -join ' '
    $process = Start-Process $probe $line -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $null = $process.Handle
    if (-not $process.WaitForExit($timeoutSeconds * 1000)) {
        & taskkill.exe /T /F /PID $process.Id | Out-Null
        Log "  probe $($label): timed out after $timeoutSeconds s"
        return $null
    }
    $text = Get-Content $out -Raw -Encoding utf8
    $raw["probe-$tag"] = @{ user = 'current'; command = $line; exit = $process.ExitCode; stdout = $text }
    $json = $null
    if ($text) { try { $json = $text | ConvertFrom-Json } catch { } }
    Log "  probe $label [$($arguments[0])]: $(Format-Probe $json)"
    return $json
}

# Runs a command line as AvelOther through a scheduled task (S4U: no password stored, the way Avel
# could; Password: their password). Returns @{ exit; text; json; task_result }.
function Invoke-AsOther([string]$commandLine, [string]$label, [string]$logon = 'S4U', [int]$timeoutSeconds = 300) {
    $script:callNumber++
    $tag = '{0:000}-{1}' -f $script:callNumber, $label
    $out = Join-Path $outDir "$tag.json"
    $err = Join-Path $outDir "$tag.err"
    $done = Join-Path $outDir "$tag.done"
    $cmdFile = Join-Path $outDir "$tag.cmd"
    $content = "@echo off`r`n$commandLine > `"$out`" 2> `"$err`"`r`necho %ERRORLEVEL%> `"$done`"`r`n"
    [IO.File]::WriteAllText($cmdFile, $content, [Text.Encoding]::ASCII)
    $taskName = "p$tag"
    $action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument "/d /c `"$cmdFile`""
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 15)
    $started = Get-Date
    $registered = $false
    try {
        if ($logon -eq 'S4U') {
            $principal = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\$otherUser" -LogonType S4U -RunLevel Limited
            Register-ScheduledTask -TaskPath '\AvelSpike\' -TaskName $taskName -Action $action -Principal $principal `
                -Settings $settings -Force | Out-Null
        } else {
            Register-ScheduledTask -TaskPath '\AvelSpike\' -TaskName $taskName -Action $action -Settings $settings `
                -User "$env:COMPUTERNAME\$otherUser" -Password $otherPassword -RunLevel Limited -Force | Out-Null
        }
        $registered = $true
    } catch {
        Log ("  as {0} ({1}) {2}: Register-ScheduledTask failed: 0x{3:X8} {4}" -f $otherUser, $logon, $label,
             $_.Exception.HResult, ($_.Exception.Message -replace '\s+', ' ').Trim())
    }
    if (-not $registered -and $logon -eq 'S4U') {
        # The same through the Task Scheduler's COM API (TASK_LOGON_S4U), for its HRESULT, then
        # schtasks /NP (no password: S4U).
        try {
            $service = New-Object -ComObject Schedule.Service
            $service.Connect()
            $definition = $service.NewTask(0)
            $definition.Principal.UserId = "$env:COMPUTERNAME\$otherUser"
            $definition.Principal.LogonType = 2  # TASK_LOGON_S4U
            $definition.Principal.RunLevel = 0
            $exec = $definition.Actions.Create(0)
            $exec.Path = 'cmd.exe'
            $exec.Arguments = "/d /c `"$cmdFile`""
            try { $service.GetFolder('\').CreateFolder('AvelSpike') | Out-Null } catch { }
            $folder = $service.GetFolder('\AvelSpike')
            $folder.RegisterTaskDefinition($taskName, $definition, 6, $null, $null, 2) | Out-Null
            $registered = $true
            Log "  as $otherUser (S4U) $($label): registered through the COM API"
        } catch {
            Log ("  as {0} (S4U) {1}: COM RegisterTaskDefinition(TASK_LOGON_S4U) failed: 0x{2:X8} {3}" -f $otherUser, $label,
                 $_.Exception.HResult, ($_.Exception.Message -replace '\s+', ' ').Trim())
        }
    }
    if (-not $registered -and $logon -eq 'S4U') {
        $output = & cmd.exe /d /c "schtasks.exe /create /tn \AvelSpike\$taskName /tr `"cmd.exe /d /c \`"$cmdFile\`"`" /sc once /st 00:00 /ru $env:COMPUTERNAME\$otherUser /np /rl LIMITED /f 2>&1"
        if ($LASTEXITCODE -eq 0) {
            $registered = $true
            Log "  as $otherUser (S4U) $($label): registered by schtasks /NP"
        } else {
            Log "  as $otherUser (S4U) $($label): schtasks /NP failed ($LASTEXITCODE): $(($output -join ' ').Trim())"
        }
    }
    if (-not $registered) { return @{ exit = -1; text = ''; json = $null; task_result = 'not-registered' } }
    try {
        Start-ScheduledTask -TaskPath '\AvelSpike\' -TaskName $taskName
    } catch {
        Log "  as $otherUser ($logon) $($label): task failed to start: $($_.Exception.Message)"
        Unregister-ScheduledTask -TaskPath '\AvelSpike\' -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
        return @{ exit = -1; text = ''; json = $null; task_result = 'not-started' }
    }
    $deadline = $started.AddSeconds($timeoutSeconds)
    $taskResult = $null
    while (-not (Test-Path $done) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $info = Get-ScheduledTaskInfo -TaskPath '\AvelSpike\' -TaskName $taskName
        $task = Get-ScheduledTask -TaskPath '\AvelSpike\' -TaskName $taskName
        # 0x41301 running, 0x41303 not yet run
        if ($task.State -eq 'Ready' -and $info.LastTaskResult -notin 0x41301, 0x41303 -and
            ((Get-Date) - $started).TotalSeconds -gt 5 -and -not (Test-Path $done)) {
            Start-Sleep -Seconds 1
            if (-not (Test-Path $done)) { $taskResult = $info.LastTaskResult; break }
        }
    }
    if ($null -eq $taskResult) {
        $taskResult = (Get-ScheduledTaskInfo -TaskPath '\AvelSpike\' -TaskName $taskName).LastTaskResult
    }
    Unregister-ScheduledTask -TaskPath '\AvelSpike\' -TaskName $taskName -Confirm:$false
    $exit = -1
    for ($attempt = 0; $attempt -lt 20 -and (Test-Path $done); $attempt++) {
        $doneText = "$([IO.File]::ReadAllText($done))".Trim()
        if ([int]::TryParse($doneText, [ref]$exit)) { break }
        Start-Sleep -Milliseconds 250
    }
    $text = if (Test-Path $out) { Get-Content $out -Raw -Encoding utf8 } else { '' }
    $errors = if (Test-Path $err) { Get-Content $err -Raw -Encoding utf8 } else { '' }
    $raw["other-$tag"] = @{ user = $otherUser; logon = $logon; command = $commandLine; exit = $exit; stdout = $text
                            stderr = $errors; task_result = ('0x{0:X8}' -f $taskResult) }
    $json = $null
    if ($text) { try { $json = $text | ConvertFrom-Json } catch { } }
    $summary = if ($json) { Format-Probe $json } elseif ($text) { ($text -replace '\s+', ' ').Trim() } else { 'no output' }
    if ($errors) { $summary += " stderr='" + ($errors -replace '\s+', ' ').Trim() + "'" }
    Log ("  as {0} ({1}) {2}: exit={3} task=0x{4:X8} {5} ({6:N1} s)" -f $otherUser, $logon, $label, $exit, $taskResult,
         $summary, ((Get-Date) - $started).TotalSeconds)
    return @{ exit = $exit; text = $text; json = $json; task_result = $taskResult }
}

function Invoke-ProbeAsOther([string[]]$arguments, [string]$label, [string]$logon = 'S4U') {
    $line = (@("`"$probe`"") + ($arguments | ForEach-Object { Quote-Argument $_ })) -join ' '
    return Invoke-AsOther $line $label $logon
}

# Runs a cmdlet step; logs ok or the error with its HRESULT. Returns @{ ok; value; error; hr }.
function Invoke-Cmdlet([string]$label, [scriptblock]$body) {
    $started = Get-Date
    try {
        $value = & $body
        Log ("  cmdlet {0}: ok ({1:N1} s)" -f $label, ((Get-Date) - $started).TotalSeconds)
        return @{ ok = $true; value = $value; error = ''; hr = 0 }
    } catch {
        $hr = '0x{0:X8}' -f $_.Exception.HResult
        $message = ($_.Exception.Message -replace '\s+', ' ').Trim()
        Log ("  cmdlet {0}: FAILED {1} '{2}' ({3:N1} s)" -f $label, $hr, $message, ((Get-Date) - $started).TotalSeconds)
        return @{ ok = $false; value = $null; error = $message; hr = $hr }
    }
}

# --- Observation -------------------------------------------------------------------------------

function Get-StoreLines {
    $lines = [System.Collections.Generic.List[string]]::new()
    $keys = @(Get-Item $storeRoot) + @(Get-ChildItem $storeRoot -Recurse -ErrorAction SilentlyContinue)
    foreach ($key in $keys) {
        $path = $key.Name -replace '^HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore', 'Store'
        $lines.Add("K $path")
        foreach ($name in $key.GetValueNames()) {
            $data = $key.GetValue($name)
            $text = if ($data -is [byte[]]) { 'bin(' + $data.Length + ')' } else { ($data -join ',') }
            if ($text.Length -gt 160) { $text = $text.Substring(0, 160) + '...' }
            $lines.Add("V $path|$name = $text")
        }
    }
    return $lines.ToArray()
}

# Logs the AppxAllUserStore's keys and values that changed since the last call.
function Show-StoreChanges {
    $now = Get-StoreLines
    $before = $script:storeLines
    $script:storeLines = $now
    $added = @($now | Where-Object { $before -notcontains $_ })
    $removed = @($before | Where-Object { $now -notcontains $_ })
    if (-not $added.Count -and -not $removed.Count) { Log '    store: no change'; return }
    $shown = 0
    foreach ($line in $added) { if ($shown++ -lt 40) { Log "    store + $line" } }
    foreach ($line in $removed) { if ($shown++ -lt 60) { Log "    store - $line" } }
    if ($shown -gt 60) { Log "    store: ... $($added.Count) added, $($removed.Count) removed" }
}

function Copy-StateRepository {
    $target = "$work\srd"
    New-Item -ItemType Directory -Force $target | Out-Null
    Remove-Item "$target\*" -Force -ErrorAction SilentlyContinue
    foreach ($suffix in '', '-wal', '-shm') {
        if (Test-Path "$srdSource$suffix") {
            try { Copy-Item "$srdSource$suffix" "$target\repo.srd$suffix" -Force } catch { }
        }
    }
    return "$target\repo.srd"
}

function Invoke-Srd([string]$sql, [int]$max = 50) {
    $copy = Copy-StateRepository
    return @([AvelSpike.Sqlite]::Query($copy, $sql, $max))
}

# Logs the StateRepository rows about `name` that changed since the last call for it.
function Show-SrdChanges([string]$name) {
    if (-not $script:srdWorks) { return }
    $copy = Copy-StateRepository
    $now = @()
    # Rows tied to the package by its id, its family or its identity, each with the name it joins
    # on (a failing query returns an "error:" row, which never holds the name).
    $queries = [ordered]@{
        'PackageUser' = 'SELECT x.*, p.PackageFullName AS _Name FROM PackageUser x JOIN Package p ON x.Package = p._PackageID'
        'PackageUserStatus' = 'SELECT x.*, p.PackageFullName AS _Name FROM PackageUserStatus x JOIN Package p ON x.Package = p._PackageID'
        'PackageMachineStatus' = 'SELECT x.*, p.PackageFullName AS _Name FROM PackageMachineStatus x JOIN Package p ON x.Package = p._PackageID'
        'PackageFamilyUser' = 'SELECT x.*, f.PackageFamilyName AS _Name FROM PackageFamilyUser x JOIN PackageFamily f ON x.PackageFamily = f._PackageFamilyID'
        'EndOfLifePackage' = 'SELECT x.*, f.PackageFamilyName AS _Name FROM EndOfLifePackage x JOIN PackageFamily f ON x.PackageFamily = f._PackageFamilyID'
        'ProvisionedPackageExclude' = 'SELECT x.*, f.PackageFamilyName AS _Name FROM ProvisionedPackageExclude x JOIN PackageFamily f ON x.PackageFamily = f._PackageFamilyID'
        'ProvisionedPackage' = 'SELECT x.*, i.PackageFullName AS _Name FROM ProvisionedPackage x JOIN PackageIdentity i ON x.PackageIdentity = i._PackageIdentityID'
        'ProvisionedPackageDeleted' = 'SELECT x.*, i.PackageFullName AS _Name FROM ProvisionedPackageDeleted x JOIN PackageIdentity i ON x.PackageIdentity = i._PackageIdentityID'
        'Package' = 'SELECT _PackageID, _Revision, PackageFullName, IsInbox, PackageType, Flags, Flags2 FROM Package'
    }
    foreach ($entry in $queries.GetEnumerator()) {
        foreach ($row in [AvelSpike.Sqlite]::Query($copy, $entry.Value, 5000)) {
            if ($row -like "*$name*") { $now += "$($entry.Key) : $row" }
        }
    }
    $before = if ($script:srdLines.ContainsKey($name)) { $script:srdLines[$name] } else { @() }
    $script:srdLines[$name] = $now
    $added = @($now | Where-Object { $before -notcontains $_ })
    $removed = @($before | Where-Object { $now -notcontains $_ })
    if (-not $added.Count -and -not $removed.Count) { Log '    srd: no change'; return }
    $shown = 0
    foreach ($line in $added) { if ($shown++ -lt 30) { Log "    srd + $line" } }
    foreach ($line in $removed) { if ($shown++ -lt 45) { Log "    srd - $line" } }
}

function Get-DataText($package) {
    $parts = @()
    foreach ($who in @(@('current', $env:LOCALAPPDATA), @('other', "$otherProfile\AppData\Local"))) {
        if (-not $who[1] -or $who[1] -eq '\AppData\Local') { continue }
        $folder = Join-Path $who[1] "Packages\$($package.family)"
        $marker = Join-Path $folder 'LocalState\avel-marker.txt'
        $parts += ('{0}: folder={1} marker={2}' -f $who[0], (Test-Path $folder), (Test-Path $marker))
    }
    return $parts -join '; '
}

# Logs everything the spike watches about a package.
function Show-PackageState($package, [string]$label) {
    Log "    [$label] $($package.full)"
    $find = $null
    try {
        $output = & $probe find $package.full
        $find = ($output -join "`n") | ConvertFrom-Json
    } catch { }
    $users = if ($find -and $find.users -is [array] -and $find.users.Count) {
        (@($find.users) | ForEach-Object { $who = if ($_.sid -eq $currentSid) { 'current' } elseif ($_.sid -eq $otherSid) { 'other' } else { $_.sid }; "${who}:$($_.state)" }) -join ', '
    } elseif ($find -and $find.users.error) { "error $($find.users.error)" } else { 'none' }
    $known = if ($find -and $find.any_user -and $find.any_user.full_name) { 'yes' } else { 'no' }
    $staged = Test-Path "C:\Program Files\WindowsApps\$($package.full)"
    $provisioned = 'unknown'
    try {
        $list = (& $probe provisioned --filter $package.name) -join "`n" | ConvertFrom-Json
        $provisioned = if (@($list.packages | Where-Object { $_.family_name -eq $package.family }).Count) {
            (@($list.packages | Where-Object { $_.family_name -eq $package.family }) | ForEach-Object { $_.full_name }) -join ','
        } else { 'no' }
    } catch { }
    $cmdlet = ''
    try {
        $cmdlet = (@(Get-AppxPackage -AllUsers -Name $package.name) | ForEach-Object {
            "$($_.PackageFullName){" + ((@($_.PackageUserInformation) | ForEach-Object {
                $sid = "$($_.UserSecurityId.Sid)"
                $who = if ($sid -eq $currentSid) { 'current' } elseif ($sid -eq $otherSid) { 'other' } else { "$($_.UserSecurityId.Username)" }
                "${who}:$($_.InstallState)" }) -join ',') + '}' }) -join ' '
    } catch { $cmdlet = "error $($_.Exception.Message)" }
    # IsPackageRemovalPendingForUser, the public view of the cmdlet's "pending removal".
    $pending = @()
    foreach ($who in @(@('current', $currentSid), @('other', $otherSid))) {
        if (-not $who[1]) { continue }
        try {
            $answer = (& $probe removal-pending $package.full --user $who[1]) -join "`n" | ConvertFrom-Json
            $pending += "$($who[0])=$(if ($answer.ok) { $answer.pending } else { $answer.hr })"
        } catch { }
    }
    Log "    users(FindUsers)=[$users] FindPackage=$known staged-folder=$staged provisioned=$provisioned removal-pending: $($pending -join ' ')"
    Log "    Get-AppxPackage -AllUsers: $(if ($cmdlet) { $cmdlet } else { 'none' })"
    Log "    data: $(Get-DataText $package)"
    Show-StoreChanges
    Show-SrdChanges $package.name
}

function Set-Marker($package, [string]$who) {
    $root = if ($who -eq 'current') { $env:LOCALAPPDATA } else { "$otherProfile\AppData\Local" }
    $folder = Join-Path $root "Packages\$($package.family)"
    if (-not (Test-Path $folder)) { Log "    marker ($who): no data folder $folder"; return }
    New-Item -ItemType Directory -Force "$folder\LocalState" | Out-Null
    Set-Content "$folder\LocalState\avel-marker.txt" "Avel spike marker for $who" -Encoding ascii
    Log "    marker ($who) written in $folder\LocalState"
}

function Get-OtherProcesses {
    @(Get-Process -IncludeUserName -ErrorAction SilentlyContinue | Where-Object { $_.UserName -like "*\$otherUser" })
}

# Waits for AvelOther's hive to be unloaded (the user signed out as far as Windows is concerned).
# After `seconds` it logs who still runs as them and the profile's reference count, ends their
# processes and waits as long again.
function Wait-OtherSignedOut([int]$seconds = 20, [switch]$Unload) {
    $started = Get-Date
    for ($pass = 0; $pass -lt 2; $pass++) {
        $until = (Get-Date).AddSeconds($seconds)
        while ((Test-Path "Registry::HKEY_USERS\$otherSid") -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 500 }
        if (-not (Test-Path "Registry::HKEY_USERS\$otherSid")) { break }
        if ($pass -eq 0) {
            $processes = Get-OtherProcesses
            $profile = Get-CimInstance Win32_UserProfile -Filter "SID='$otherSid'" -ErrorAction SilentlyContinue
            Log ("    {0}'s hive still loaded after {1} s: profile Loaded={2}; their processes: {3}" -f $otherUser,
                 $seconds, $profile.Loaded, $(if ($processes) { ($processes | ForEach-Object { "$($_.Name)($($_.Id))" }) -join ', ' } else { 'none' }))
            $processes | Stop-Process -Force -ErrorAction SilentlyContinue
        }
    }
    $loaded = Test-Path "Registry::HKEY_USERS\$otherSid"
    if ($loaded -and $Unload) {
        # A batch logon's hive stays loaded (runs 2 and 3), and reg unload is denied (run 3): who
        # holds it? Stopping the AppX deployment service tells whether it is AppX.
        foreach ($service in 'AppXSvc', 'ClipSVC') {
            $stop = Invoke-Cmdlet "Stop-Service $service" { Stop-Service $service -Force -ErrorAction Stop }
            Start-Sleep -Seconds 5
            Log "    after stopping $($service): hive loaded=$(Test-Path "Registry::HKEY_USERS\$otherSid")"
            if (-not (Test-Path "Registry::HKEY_USERS\$otherSid")) { break }
        }
        # Unload it as the profile service would at sign-out, so the next step meets a user who is
        # signed out with the hive unloaded.
        $classes = & cmd.exe /d /c "reg.exe unload HKU\${otherSid}_Classes 2>&1"
        $classesCode = $LASTEXITCODE
        $main = & cmd.exe /d /c "reg.exe unload HKU\$otherSid 2>&1"
        Log ("    reg unload: {0}_Classes -> {1} '{2}'; {0} -> {3} '{4}'" -f $otherSid, $classesCode, ($classes -join ' ').Trim(),
             $LASTEXITCODE, ($main -join ' ').Trim())
        $loaded = Test-Path "Registry::HKEY_USERS\$otherSid"
    }
    Log ("    {0}'s hive {1} after {2:N0} s" -f $otherUser, $(if ($loaded) { 'STILL LOADED' } else { 'unloaded' }),
         ((Get-Date) - $started).TotalSeconds)
    return -not $loaded
}

# Registers the package for AvelOther the way round 2 found to work.
function Register-ForOther($package, [string]$label, [switch]$Family) {
    if (-not $otherRegister) { Log "    no way found to register for $otherUser"; return $null }
    $arguments = if ($Family) { @('register-family', $package.family) } else { @('register-full', $package.full) }
    if ($otherRegister.unsigned -and -not $Family) { $arguments += '--allow-unsigned' }
    return Invoke-ProbeAsOther $arguments $label $otherRegister.logon
}

function Get-PackageRecord([string]$fullName) {
    $parts = $fullName -split '_'
    $family = $parts[0] + '_' + $parts[-1]
    return [pscustomobject]@{ full = $fullName; family = $family; name = $parts[0] }
}

function Get-PendingRestart {
    $flags = @()
    $cbs = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing'
    foreach ($name in 'RebootPending', 'RebootInProgress', 'PackagesPending', 'SessionsPending') {
        if (Test-Path "$cbs\$name") { $flags += "CBS\$name" }
    }
    if (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired') {
        $flags += 'WU\RebootRequired'
    }
    $pending = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -ErrorAction SilentlyContinue).PendingFileRenameOperations
    if ($pending) { $flags += "PendingFileRenameOperations($(@($pending).Count))" }
    $xml = Test-Path 'C:\Windows\WinSxS\pending.xml'
    if ($xml) { $flags += 'WinSxS\pending.xml' }
    if (-not $flags) { return 'none' }
    return $flags -join ', '
}

# --- Rounds -------------------------------------------------------------------------------------

function Invoke-InventoryRound {
    Log "  network adapters: $((@(Get-NetAdapter -ErrorAction SilentlyContinue) | ForEach-Object { "$($_.Name)=$($_.Status)" }) -join ', ')"
    $online = $false
    try { $online = Test-Connection 1.1.1.1 -Count 1 -Quiet -ErrorAction Stop } catch { }
    Log "  internet reachable (ping 1.1.1.1): $online"
    if ($online) { Note 'offline' 'NOT offline: the sandbox reached the internet' } else { Note 'offline' 'the sandbox had no network' }
    foreach ($name in 'AppXSvc', 'StateRepository', 'ClipSVC', 'AppReadiness', 'InstallService', 'wuauserv',
                      'TrustedInstaller', 'camsvc') {
        $service = Get-Service $name -ErrorAction SilentlyContinue
        if ($service) { Log "  service $($name): $($service.Status), $($service.StartType)" } else { Log "  service $($name): absent" }
    }
    $unlock = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -ErrorAction SilentlyContinue
    Log "  AppModelUnlock: AllowAllTrustedApps=$($unlock.AllowAllTrustedApps) AllowDevelopmentWithoutDevLicense=$($unlock.AllowDevelopmentWithoutDevLicense)"

    # Every package of every user, as the cmdlet sees it.
    $all = Invoke-Cmdlet 'Get-AppxPackage -AllUsers' { @(Get-AppxPackage -AllUsers) }
    $script:inventory = @($all.value)
    Log "  Get-AppxPackage -AllUsers: $($inventory.Count) package(s)"
    foreach ($package in $inventory | Sort-Object Name) {
        $users = (@($package.PackageUserInformation) | ForEach-Object { "$($_.UserSecurityId.Username):$($_.InstallState)" }) -join ','
        Log ("    {0} {1} sig={2} nonremovable={3} framework={4} resource={5} bundle={6} users=[{7}]" -f $package.PackageFullName,
             $package.Architecture, $package.SignatureKind, $package.NonRemovable, $package.IsFramework,
             $package.IsResourcePackage, $package.IsBundle, $users)
    }
    $provisioned = Invoke-Cmdlet 'Get-AppxProvisionedPackage -Online' { @(Get-AppxProvisionedPackage -Online) }
    $script:provisionedInbox = @($provisioned.value)
    Log "  Get-AppxProvisionedPackage -Online: $($provisionedInbox.Count) package(s)"
    foreach ($package in $provisionedInbox) { Log "    provisioned $($package.PackageName) ($($package.DisplayName) $($package.Version))" }

    $list = Invoke-Probe @('list', '--all-users') 'list-all-users'
    if ($list) {
        $types = @{}
        foreach ($package in $list.packages) {
            $kind = if ($package.is_framework -eq $true) { 'framework' } elseif ($package.is_resource -eq $true) { 'resource' } elseif ($package.is_bundle -eq $true) { 'bundle' } else { 'main' }
            $types[$kind] = 1 + [int]$types[$kind]
        }
        Log "  probe FindPackagesWithPackageTypes(all): $($list.count) ($(($types.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', '))"
        $signatures = @{}
        foreach ($package in $list.packages) { $signatures["$($package.signature_kind)"] = 1 + [int]$signatures["$($package.signature_kind)"] }
        Log "  signature kinds: $(($signatures.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', ')"
    }
    $prov = Invoke-Probe @('provisioned') 'provisioned'
    if ($prov) { foreach ($package in $prov.packages) { Log "    FindProvisionedPackages: $($package.full_name)" } }

    $script:storeLines = Get-StoreLines
    Log "  AppxAllUserStore: $($storeLines.Count) line(s); top-level keys:"
    foreach ($key in Get-ChildItem $storeRoot) {
        Log "    $($key.PSChildName) ($(@(Get-ChildItem $key.PSPath -Recurse -ErrorAction SilentlyContinue).Count) subkeys below)"
    }

    # The StateRepository, from a copy.
    try {
        $tables = Invoke-Srd "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name" 500
        if ($tables.Count -and $tables[0] -notlike 'error*') {
            $script:srdWorks = $true
            $names = @($tables | ForEach-Object { $_ -replace '^name=', '' })
            Log "  StateRepository-Machine.srd copy readable: $($names.Count) tables"
            Log "    tables: $($names -join ', ')"
            $script:srdExtraTables = @($names | Where-Object { $_ -match 'Provision|EndOfLife|Delet|Pending|Stag|Remov|Uninstall' })
            Log "    watched extra tables: $($srdExtraTables -join ', ')"
            foreach ($table in @('Package', 'PackageUser', 'User', 'PackageFamily', 'PackageFamilyUser', 'PackageIdentity', 'PackageUserStatus', 'PackageMachineStatus') + $srdExtraTables) {
                $schema = Invoke-Srd "SELECT sql FROM sqlite_master WHERE type='table' AND name='$table'" 1
                Log "    schema $($table): $(($schema -join ' ') -replace '\s+', ' ')"
            }
            foreach ($row in Invoke-Srd 'SELECT * FROM User' 20) { Log "    User: $row" }
        } else {
            Log "  StateRepository copy not readable: $($tables -join ' ')"
        }
    } catch {
        Log "  StateRepository copy not readable: $($_.Exception.Message)"
    }
}

# Can SYSTEM (which holds the TCB privilege an administrator lacks) register an S4U task for
# another user? A SYSTEM task registers one and starts it; the inner task runs the probe's whoami.
function Test-S4UFromSystem {
    $inner = "$outDir\s4u-inner"
    [IO.File]::WriteAllText("$inner.cmd", "@echo off`r`n`"$probe`" whoami > $inner.json 2>&1`r`necho %ERRORLEVEL%> $inner.done`r`n",
                            [Text.Encoding]::ASCII)
    $script = @"
try {
    `$action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument '/d /c $inner.cmd'
    `$principal = New-ScheduledTaskPrincipal -UserId '$env:COMPUTERNAME\$otherUser' -LogonType S4U -RunLevel Limited
    Register-ScheduledTask -TaskPath '\AvelSpike\' -TaskName 's4u-inner' -Action `$action -Principal `$principal -Force -ErrorAction Stop | Out-Null
    Start-ScheduledTask -TaskPath '\AvelSpike\' -TaskName 's4u-inner' -ErrorAction Stop
    `$result = 'registered and started by ' + [Security.Principal.WindowsIdentity]::GetCurrent().Name
} catch { `$result = 'failed: 0x{0:X8} {1}' -f `$_.Exception.HResult, `$_.Exception.Message }
Set-Content '$outDir\s4u-outer.txt' `$result -Encoding ascii
"@
    [IO.File]::WriteAllText("$outDir\s4u-outer.ps1", $script, [Text.Encoding]::ASCII)
    try {
        $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -File $outDir\s4u-outer.ps1"
        $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
        Register-ScheduledTask -TaskPath '\AvelSpike\' -TaskName 's4u-outer' -Action $action -Principal $principal -Force | Out-Null
        Start-ScheduledTask -TaskPath '\AvelSpike\' -TaskName 's4u-outer'
    } catch {
        Log "  S4U from SYSTEM: the SYSTEM task failed: $($_.Exception.Message)"
        return
    }
    $deadline = (Get-Date).AddSeconds(90)
    while (-not (Test-Path "$inner.done") -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        if ((Test-Path "$outDir\s4u-outer.txt") -and "$(Get-Content "$outDir\s4u-outer.txt" -Raw)" -like 'failed*') { break }
    }
    $outer = if (Test-Path "$outDir\s4u-outer.txt") { "$(Get-Content "$outDir\s4u-outer.txt" -Raw)".Trim() } else { 'no output' }
    $whoami = if (Test-Path "$inner.json") { ((Get-Content "$inner.json" -Raw) -replace '\s+', ' ').Trim() } else { 'no output' }
    Log "  S4U from SYSTEM: outer: $outer; inner whoami: $whoami"
    $raw['s4u-from-system'] = @{ outer = $outer; inner = $whoami }
    Note 's4u-from-system' $(if ($whoami -like '*"ok": true*') { "SYSTEM registered an S4U task for $otherUser, which ran as them: $whoami" } else { "SYSTEM could not run an S4U task as $($otherUser): $outer" })
    foreach ($name in 's4u-outer', 's4u-inner') {
        Unregister-ScheduledTask -TaskPath '\AvelSpike\' -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
    }
    Wait-OtherSignedOut | Out-Null
}

function Invoke-OtherUserRound {

    New-LocalUser -Name $otherUser -Password (ConvertTo-SecureString $otherPassword -AsPlainText -Force) `
        -AccountNeverExpires -PasswordNeverExpires | Out-Null
    $script:otherSid = (New-Object Security.Principal.NTAccount($otherUser)).Translate(
        [Security.Principal.SecurityIdentifier]).Value
    # Scheduled tasks log on as a batch job: Performance Log Users hold that right on client SKUs.
    try {
        Add-LocalGroupMember -SID 'S-1-5-32-559' -Member $otherUser
        Log "  $otherUser added to Performance Log Users (S-1-5-32-559) for the batch logon right"
    } catch { Log "  adding $otherUser to Performance Log Users failed: $($_.Exception.Message)" }
    $path = New-Object Text.StringBuilder 260
    $hr = [AvelSpike.UserEnv]::CreateProfile($otherSid, $otherUser, $path, 260)
    if ($hr -ne 0) { throw ('CreateProfile for {0} returned 0x{1:X8}' -f $otherUser, $hr) }
    $script:otherProfile = $path.ToString()
    Log "  user $otherUser ($otherSid), profile $otherProfile (CreateProfile, never signed in)"
    Wait-OtherSignedOut 60 | Out-Null

    $me = Invoke-Probe @('whoami') 'whoami'
    foreach ($logon in 'S4U', 'Password') {
        $run = Invoke-ProbeAsOther @('whoami') "whoami-$logon" $logon
        if ($logon -eq 'S4U') { $script:s4uWorks = [bool]($run.json -and $run.json.ok) }
        if ($run.json) {
            Log "    ran as $($run.json.user) $($run.json.sid) elevated=$($run.json.elevated) session=$($run.json.session) profile=$($run.json.profile)"
            Log "    $otherUser's hive loaded right after the task: $(Test-Path "Registry::HKEY_USERS\$otherSid")"
        }
        Wait-OtherSignedOut $(if ($logon -eq 'Password') { 60 } else { 20 }) | Out-Null
    }
    if (-not $s4uWorks) { Test-S4UFromSystem }
    $mine = Invoke-Probe @('list', '--user', $otherSid) 'list-other-user'
    if ($mine) { Log "    packages registered for $otherUser after the task logons: $($mine.count)" }
    Note 'tasks-as-other' ("S4U task as {0}: {1}; password task: see the whoami lines" -f $otherUser,
        $(if ($s4uWorks) { 'works' } else { 'could not be registered' }))
}

function Invoke-TestPackageRound {
    if (-not (Test-Path $testMsix)) { throw "no $testMsix; run Get-SampleInstallers.ps1" }
    $add = Invoke-Probe @('add', $testMsix, '--allow-unsigned') 'add-test-msix'
    if (-not $add -or -not $add.ok) {
        Invoke-Cmdlet 'Add-AppxPackage -AllowUnsigned' { Add-AppxPackage -Path $testMsix -AllowUnsigned } | Out-Null
    }
    $mine = Invoke-Probe @('list', '--filter', 'Avel.TestPackage', '--users') 'list-test'
    $full = @($mine.packages | Where-Object { $_.name -eq 'Avel.TestPackage' })[0].full_name
    if (-not $full) { throw 'the test package is not installed' }
    $script:test = Get-PackageRecord $full
    Log "  test package $full (family $($test.family)), signature $(@($mine.packages)[0].signature_kind)"
    Show-PackageState $test 'added for the current user'
    Set-Marker $test 'current'

    # Registering it for the signed-out user: the ways Avel could, then the cmdlet.
    $attempts = @(
        @{ logon = 'S4U'; unsigned = $false }, @{ logon = 'S4U'; unsigned = $true },
        @{ logon = 'Password'; unsigned = $false }, @{ logon = 'Password'; unsigned = $true })
    foreach ($attempt in $attempts) {
        if ($attempt.logon -eq 'S4U' -and -not $s4uWorks) { continue }
        $arguments = @('register-full', $full)
        if ($attempt.unsigned) { $arguments += '--allow-unsigned' }
        $run = Invoke-ProbeAsOther $arguments "register-other-$($attempt.logon)$(if ($attempt.unsigned) { '-unsigned' })" $attempt.logon
        if ($run.json -and $run.json.ok) { $script:otherRegister = $attempt; break }
    }
    if (-not $otherRegister) {
        $manifest = "C:\Program Files\WindowsApps\$full\AppxManifest.xml"
        $command = "powershell.exe -NoProfile -ExecutionPolicy Bypass -Command `"Add-AppxPackage -Register '$manifest' -DisableDevelopmentMode; 'cmdlet ok'`""
        $run = Invoke-AsOther $command 'register-other-cmdlet' 'Password'
    }
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'registered for the other user'
    if ($otherRegister) {
        Note 'register-other' ("RegisterPackageByFullNameAsync{0} run as {1} in a {2} task registers a staged package for a signed-out user" -f
            $(if ($otherRegister.unsigned) { ' (AllowUnsigned)' } else { '' }), $otherUser, $otherRegister.logon)
    } else {
        Note 'register-other' 'no probe call registered the package for the other user'
    }
    Set-Marker $test 'other'
    Show-PackageState $test 'markers written'
}

function Invoke-OtherRemovalRound {
    Wait-OtherSignedOut | Out-Null

    Log '  3a: Remove-AppxPackage -Package <full> -User <sid> (cmdlet), the other user signed out'
    Invoke-Cmdlet 'Remove-AppxPackage -User' { Remove-AppxPackage -Package $test.full -User $otherSid } | Out-Null
    Show-PackageState $test 'after Remove-AppxPackage -User'
    Log '  3a: does a (batch) logon of the other user carry out the pending removal?'
    Invoke-ProbeAsOther @('whoami') 'logon-after-3a' 'Password' | Out-Null
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'after a logon of the other user'
    $undo = Register-ForOther $test 'undo-3a'
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'after registering again as the other user'

    Log '  3b: RemovePackageAsync run as the other user (public API, in a task)'
    Set-Marker $test 'other'
    $remove = Invoke-ProbeAsOther @('remove', $test.full) 'remove-as-other' $(if ($otherRegister) { $otherRegister.logon } else { 'S4U' })
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'after RemovePackageAsync as the other user'
    $undo = Register-ForOther $test 'undo-3b'
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'after registering again'

    Log '  3c: RemovePackageAsync(PreserveApplicationData) as the other user'
    Set-Marker $test 'other'
    $remove = Invoke-ProbeAsOther @('remove', $test.full, '--preserve-data') 'remove-as-other-preserve' $(if ($otherRegister) { $otherRegister.logon } else { 'S4U' })
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'after RemovePackageAsync(PreserveApplicationData) as the other user'
    $undo = Register-ForOther $test 'undo-3c'
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'after registering again'

    Log '  3d: CleanupPackageForUserAsync(full name, sid) as the administrator, the profile still there'
    Set-Marker $test 'other'
    $cleanup = Invoke-Probe @('cleanup-user', $test.full, $otherSid) 'cleanup-user'
    Show-PackageState $test 'after CleanupPackageForUserAsync'
    if ($cleanup -and -not $cleanup.ok) {
        $policy = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Appx'
        $set = Invoke-Cmdlet 'set AllowDeploymentInSpecialProfiles=1' {
            if (-not (Test-Path $policy)) { New-Item -Force $policy | Out-Null }
            Set-ItemProperty $policy AllowDeploymentInSpecialProfiles 1 -Type DWord }
        if ($set.ok) {
            $cleanup = Invoke-Probe @('cleanup-user', $test.full, $otherSid) 'cleanup-user-policy'
            Remove-ItemProperty $policy AllowDeploymentInSpecialProfiles -ErrorAction SilentlyContinue
            Show-PackageState $test 'after CleanupPackageForUserAsync with AllowDeploymentInSpecialProfiles'
        }
    }
    $undo = Register-ForOther $test 'undo-3d'
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'after registering again'

    Log '  3e: Remove-AppxPackage -User for a user whose hive is unloaded (reg unload, as at a real sign-out)'
    Set-Marker $test 'other'
    if (Wait-OtherSignedOut -Unload) {
        $profile = Get-CimInstance Win32_UserProfile -Filter "SID='$otherSid'" -ErrorAction SilentlyContinue
        Log "    profile Loaded=$($profile.Loaded)"
        Invoke-Cmdlet 'Remove-AppxPackage -User (hive unloaded)' { Remove-AppxPackage -Package $test.full -User $otherSid } | Out-Null
        Show-PackageState $test 'after Remove-AppxPackage -User, hive unloaded'
        Log "    hive loaded by the removal: $(Test-Path "Registry::HKEY_USERS\$otherSid")"
        $logon = Invoke-ProbeAsOther @('whoami') 'logon-after-3e' 'Password'
        if ($logon.json) { Log "    the next logon got profile $($logon.json.profile)" }
        Wait-OtherSignedOut | Out-Null
        Show-PackageState $test 'after the next logon of the other user'
        $undo = Register-ForOther $test 'undo-3e'
        Wait-OtherSignedOut | Out-Null
        Show-PackageState $test 'after registering again'
    }
}

function Invoke-CurrentRemovalRound {
    Log '  4a: RemovePackageAsync for the current user while the other user keeps it'
    Set-Marker $test 'current'
    Invoke-Probe @('remove', $test.full) 'remove-current' | Out-Null
    Show-PackageState $test 'after RemovePackageAsync (current user)'
    $register = Invoke-Probe @('register-full', $test.full) 'register-current'
    if (-not $register -or -not $register.ok) { $register = Invoke-Probe @('register-full', $test.full, '--allow-unsigned') 'register-current-unsigned' }
    Show-PackageState $test 'after RegisterPackageByFullNameAsync (current user)'
    $family = Invoke-Probe @('remove', $test.full) 'remove-current-2'
    $family = Invoke-Probe @('register-family', $test.family) 'register-family-current'
    Show-PackageState $test 'after remove + RegisterPackageByFamilyNameAsync (current user)'

    Log '  4b: RemovePackageAsync(PreserveApplicationData) for the current user'
    Set-Marker $test 'current'
    Invoke-Probe @('remove', $test.full, '--preserve-data') 'remove-current-preserve' | Out-Null
    Show-PackageState $test 'after RemovePackageAsync(PreserveApplicationData)'
    $register = Invoke-Probe @('register-full', $test.full) 'register-current-after-preserve'
    if (-not $register -or -not $register.ok) { Invoke-Probe @('register-full', $test.full, '--allow-unsigned') 'register-current-after-preserve-unsigned' | Out-Null }
    Show-PackageState $test 'after registering again'

    Log '  4c: removed for its last user (the other user first, then the current user), never provisioned'
    $remove = Invoke-ProbeAsOther @('remove', $test.full) 'remove-other-4c' $(if ($otherRegister) { $otherRegister.logon } else { 'S4U' })
    Wait-OtherSignedOut | Out-Null
    $removed = Get-Date
    Invoke-Probe @('remove', $test.full) 'remove-current-last' | Out-Null
    Show-PackageState $test 'right after the last user''s removal'
    foreach ($wait in 10, 50) {
        Start-Sleep -Seconds $wait
        Log ('    {0:N0} s later: staged-folder={1}' -f ((Get-Date) - $removed).TotalSeconds,
             (Test-Path "C:\Program Files\WindowsApps\$($test.full)"))
    }
    Show-PackageState $test 'a minute after the last user''s removal'
    $register = Invoke-Probe @('register-full', $test.full, '--allow-unsigned') 'register-after-last'
    $family = Invoke-Probe @('register-family', $test.family) 'register-family-after-last'
    if ($register -and $register.ok -and $register.is_registered -ne $false) {
        Note 'gc' 'a package removed for its last user (not provisioned) could still be registered again'
    } else {
        Note 'gc' ("a package removed for its last user (not provisioned) is gone at once: register-full {0}" -f (Format-Probe $register))
    }

    Log '  4d: RemoveForAllUsers on a package of two users (not provisioned)'
    Invoke-Probe @('add', $testMsix, '--allow-unsigned') 'add-again' | Out-Null
    Register-ForOther $test 'register-other-4d' | Out-Null
    Wait-OtherSignedOut | Out-Null
    Set-Marker $test 'current'
    Set-Marker $test 'other'
    Show-PackageState $test 'installed for both again'
    Invoke-Probe @('remove', $test.full, '--all-users') 'remove-all-users' | Out-Null
    Show-PackageState $test 'after RemovePackageAsync(RemoveForAllUsers)'
    Invoke-Probe @('register-full', $test.full, '--allow-unsigned') 'register-after-all-users' | Out-Null
}

function Invoke-ProvisionRound {
    Log '  5a: staged only (StagePackageByUriAsync, AllowUnsigned), no user'
    $stage = Invoke-Probe @('add', $testMsix, '--allow-unsigned', '--stage-only') 'stage'
    Show-PackageState $test 'staged, no user'
    Start-Sleep -Seconds 30
    Show-PackageState $test 'staged, no user, 30 s later'

    Log '  5b: ProvisionPackageForAllUsersAsync(family) on the staged unsigned package'
    $provision = Invoke-Probe @('provision', $test.family) 'provision-test'
    if (-not $provision -or -not $provision.ok) {
        Invoke-Cmdlet 'Add-AppxProvisionedPackage -Online -SkipLicense' {
            Add-AppxProvisionedPackage -Online -PackagePath $testMsix -SkipLicense } | Out-Null
    }
    Show-PackageState $test 'after provisioning'
    $register = Invoke-Probe @('register-family', $test.family) 'register-family-provisioned'
    if (-not $register -or -not $register.ok) { Invoke-Probe @('register-full', $test.full, '--allow-unsigned') 'register-full-provisioned' | Out-Null }
    Show-PackageState $test 'provisioned and registered for the current user'

    Log '  5c: soft removal of a provisioned package (current user), and its undo'
    Set-Marker $test 'current'
    Invoke-Probe @('remove', $test.full) 'remove-provisioned-current' | Out-Null
    Show-PackageState $test 'provisioned, removed for the current user'
    $register = Invoke-Probe @('register-family', $test.family) 'register-family-after-soft'
    if (-not $register -or -not $register.ok) { Invoke-Probe @('register-full', $test.full, '--allow-unsigned') 'register-full-after-soft' | Out-Null }
    Show-PackageState $test 'registered again'

    Log '  5c2: the provisioned package and the other user: does their next logon register it?'
    Invoke-ProbeAsOther @('whoami') 'logon-provisioned' 'Password' | Out-Null
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'after a logon of the other user'
    Register-ForOther $test 'register-provisioned-other' -Family | Out-Null
    Wait-OtherSignedOut | Out-Null
    Set-Marker $test 'other'
    Show-PackageState $test 'registered for the other user by family'
    Log '  5c3: soft removal of the provisioned package for the other user (RemovePackageAsync as them)'
    Invoke-ProbeAsOther @('remove', $test.full) 'remove-provisioned-other' 'Password' | Out-Null
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'provisioned, removed for the other user'
    Invoke-ProbeAsOther @('whoami') 'logon-after-provisioned-removal' 'Password' | Out-Null
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'after the other user''s next logon (re-provisioned for them?)'
    Register-ForOther $test 'undo-provisioned-other' -Family | Out-Null
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'registered again for the other user'
    Log '  5c4: Remove-AppxPackage -User on the provisioned package for the other user'
    Invoke-Cmdlet 'Remove-AppxPackage -User (provisioned)' { Remove-AppxPackage -Package $test.full -User $otherSid } | Out-Null
    Show-PackageState $test 'provisioned, removed for the other user by the cmdlet'
    Register-ForOther $test 'undo-provisioned-other-cmdlet' -Family | Out-Null
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $test 'registered again for the other user'

    Log '  5d: DeprovisionPackageForAllUsersAsync, then ProvisionPackageForAllUsersAsync again'
    Invoke-Probe @('deprovision', $test.family) 'deprovision-test' | Out-Null
    Show-PackageState $test 'after DeprovisionPackageForAllUsersAsync'
    Invoke-Probe @('provision', $test.family) 'reprovision-test' | Out-Null
    Show-PackageState $test 'after provisioning again'

    Log '  5e: RemovePackageAsync(RemoveForAllUsers) on the provisioned package'
    Invoke-Probe @('remove', $test.full, '--all-users') 'remove-all-users-provisioned' | Out-Null
    Show-PackageState $test 'after RemoveForAllUsers'
    Invoke-Probe @('provision', $test.family) 'provision-after-all-users' | Out-Null
    Invoke-Probe @('register-family', $test.family) 'register-family-after-all-users' | Out-Null
    Show-PackageState $test 'after trying to provision and register again'
}

# The inbox package to work on: provisioned, installed for the current user, not NonRemovable, not a
# framework; one an "often removed" list would hold.
function Select-InboxPackage {
    $preferred = 'Microsoft.WindowsCalculator', 'Microsoft.WindowsNotepad', 'Microsoft.Paint', 'Microsoft.ScreenSketch',
                 'Microsoft.MicrosoftStickyNotes', 'Microsoft.WindowsAlarms', 'Microsoft.WindowsTerminal', 'Microsoft.WindowsCamera',
                 'Microsoft.Windows.Photos', 'Microsoft.ZuneMusic', 'Microsoft.GetHelp', 'Clipchamp.Clipchamp'
    $provisionedNames = @($provisionedInbox | ForEach-Object { $_.DisplayName })
    $candidates = @(Get-AppxPackage | Where-Object {
        -not $_.NonRemovable -and -not $_.IsFramework -and -not $_.IsResourcePackage -and
        $provisionedNames -contains $_.Name -and $_.Name -ne 'Avel.TestPackage' })
    Log "  provisioned, removable inbox candidates: $(($candidates | ForEach-Object { $_.Name }) -join ', ')"
    foreach ($name in $preferred) {
        $hit = @($candidates | Where-Object { $_.Name -eq $name })
        if ($hit.Count) { return $hit[0] }
    }
    if ($candidates.Count) { return $candidates[0] }
    # Nothing provisioned: any removable Store package of the current user.
    $fallback = @(Get-AppxPackage | Where-Object { -not $_.NonRemovable -and -not $_.IsFramework -and
        -not $_.IsResourcePackage })
    Log "  no provisioned candidate; removable Store packages: $(($fallback | ForEach-Object { $_.Name }) -join ', ')"
    if ($fallback.Count) { return $fallback[0] }
    return $null
}

function Invoke-InboxRound {
    $package = Select-InboxPackage
    if (-not $package) { Note 'inbox' 'the image has no removable inbox package'; return }
    $script:inbox = Get-PackageRecord $package.PackageFullName
    $bundle = @($provisionedInbox | Where-Object { $_.DisplayName -eq $package.Name })
    Log "  inbox package $($inbox.full); provisioned as $(($bundle | ForEach-Object { $_.PackageName }) -join ', ')"
    Show-PackageState $inbox 'before'

    Log '  6a: registering the provisioned package for the signed-out user (RegisterPackageByFamilyNameAsync as them)'
    Register-ForOther $inbox 'register-inbox-other' -Family | Out-Null
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $inbox 'after registering for the other user'
    Set-Marker $inbox 'current'
    Set-Marker $inbox 'other'

    Log '  6b: soft removal for the current user, undone by RegisterPackageByFamilyNameAsync'
    Invoke-Probe @('remove', $inbox.full) 'remove-inbox-current' | Out-Null
    Show-PackageState $inbox 'removed for the current user'
    Invoke-Probe @('register-family', $inbox.family) 'register-inbox-current' | Out-Null
    Show-PackageState $inbox 'registered again for the current user'

    Log '  6c: soft removal for the signed-out user: the cmdlet -User, then RemovePackageAsync as them'
    Invoke-Cmdlet 'Remove-AppxPackage -User (inbox)' { Remove-AppxPackage -Package $inbox.full -User $otherSid } | Out-Null
    Show-PackageState $inbox 'removed for the other user by the cmdlet'
    Register-ForOther $inbox 'register-inbox-other-2' -Family | Out-Null
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $inbox 'registered again for the other user'
    Invoke-ProbeAsOther @('remove', $inbox.full) 'remove-inbox-as-other' $(if ($otherRegister) { $otherRegister.logon } else { 'S4U' }) | Out-Null
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $inbox 'removed for the other user by RemovePackageAsync as them'
    Register-ForOther $inbox 'register-inbox-other-3' -Family | Out-Null
    Wait-OtherSignedOut | Out-Null
    Show-PackageState $inbox 'registered again for the other user'

    Log '  6d: deprovision (public API), then provision again offline'
    Invoke-Probe @('deprovision', $inbox.family) 'deprovision-inbox' | Out-Null
    Show-PackageState $inbox 'deprovisioned by DeprovisionPackageForAllUsersAsync'
    Invoke-Probe @('provision', $inbox.family) 'reprovision-inbox' | Out-Null
    Show-PackageState $inbox 'provisioned again by ProvisionPackageForAllUsersAsync'

    Log '  6e: deprovision (Remove-AppxProvisionedPackage -Online), then provision again offline'
    foreach ($entry in $bundle) {
        Invoke-Cmdlet "Remove-AppxProvisionedPackage $($entry.PackageName)" {
            Remove-AppxProvisionedPackage -Online -PackageName $entry.PackageName } | Out-Null
    }
    Show-PackageState $inbox 'deprovisioned by the cmdlet'
    Invoke-Probe @('provision', $inbox.family) 'reprovision-inbox-2' | Out-Null
    Show-PackageState $inbox 'provisioned again'

    Log '  6f: deep removal: deprovision + RemovePackageAsync(RemoveForAllUsers)'
    Invoke-Probe @('deprovision', $inbox.family) 'deprovision-inbox-deep' | Out-Null
    Invoke-Probe @('remove', $inbox.full, '--all-users') 'remove-inbox-all-users' | Out-Null
    Show-PackageState $inbox 'after deep removal'
    Start-Sleep -Seconds 20
    Show-PackageState $inbox 'deep removal, 20 s later'
    $provision = Invoke-Probe @('provision', $inbox.family) 'provision-after-deep'
    $register = Invoke-Probe @('register-family', $inbox.family) 'register-after-deep'
    Show-PackageState $inbox 'after trying to bring it back'
    Note 'deep-undo' ("after deprovision + RemoveForAllUsers: provision {0}; register-family {1}" -f (Format-Probe $provision), (Format-Probe $register))
}

function Find-Item($items, [string[]]$preferred, [string]$state, [string]$deny) {
    foreach ($name in $preferred) {
        $hit = @($items | Where-Object { $_.name -like $name -and $_.state -eq $state })
        if ($hit.Count) { return $hit[0].name }
    }
    $any = @($items | Where-Object { $_.state -eq $state -and $_.name -notmatch $deny })
    if ($any.Count) { return $any[0].name }
    return $null
}

function Invoke-DismRound {
    $log = "$work\dismapi.log"
    Log "  pending restart before: $(Get-PendingRestart)"
    $sessions = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\SessionsPending'
    if (Test-Path $sessions) {
        $values = Get-ItemProperty $sessions
        Log "    SessionsPending: $(($values.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ', ')"
    }
    $features = Invoke-Probe @('dism-features', '--log', $log) 'dism-features'
    $capabilities = Invoke-Probe @('dism-capabilities', '--log', $log) 'dism-capabilities'
    if (-not $features -or -not $features.ok) {
        # Diagnosis: the same through DISM's own tools, then once more after servicing had time.
        foreach ($command in '/online /get-features /format:table', '/online /get-capabilities /format:table',
                             '/online /cleanup-image /checkhealth') {
            $started = Get-Date
            $output = & cmd.exe /d /c "dism.exe $command /english 2>&1"
            $text = @($output | Where-Object { $_ -match '\S' })
            Log ("    dism.exe {0}: exit {1} ({2:N1} s): {3}" -f $command, $LASTEXITCODE, ((Get-Date) - $started).TotalSeconds,
                 (($text | Select-Object -Last 4) -join ' | '))
        }
        Invoke-Cmdlet 'Get-WindowsOptionalFeature -Online' { @(Get-WindowsOptionalFeature -Online).Count } | Out-Null
        Invoke-Cmdlet 'Get-WindowsCapability -Online' { @(Get-WindowsCapability -Online).Count } | Out-Null
        Log "    TrustedInstaller: $((Get-Service TrustedInstaller).Status); waiting 90 s, then DismGetFeatures again"
        Start-Sleep -Seconds 90
        $features = Invoke-Probe @('dism-features', '--log', $log) 'dism-features-retry'
        if (-not $capabilities -or -not $capabilities.ok) {
            $capabilities = Invoke-Probe @('dism-capabilities', '--log', $log) 'dism-capabilities-retry'
        }
    }
    if (-not $features -or -not $features.ok) {
        Note 'dism' ("DismGetFeatures failed: {0}; DismGetCapabilities: {1}" -f (Format-Probe $features), (Format-Probe $capabilities))
        # Enumeration fails; single features and capabilities may still work by name. CBS lists the
        # optional features in the registry too.
        $notifications = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\Notifications\OptionalFeatures'
        $registered = @(Get-ChildItem $notifications -ErrorAction SilentlyContinue)
        Log "    CBS Notifications\OptionalFeatures: $($registered.Count) feature(s)"
        $sample = @($registered | Select-Object -First 3 | ForEach-Object {
            $key = $_; "$($key.PSChildName){" + (($key.GetValueNames() | ForEach-Object { "$_=$($key.GetValue($_))" }) -join ',') + '}' })
        Log "    e.g. $($sample -join ' ')"
        $names = @('TelnetClient', 'TFTP', 'SimpleTCP', 'Client-ProjFS', 'TIFFIFilter', 'Printing-XPSServices-Features',
                   'WorkFolders-Client', 'MSRDC-Infrastructure', 'SearchEngine-Client-Package', 'Printing-PrintToPDFServices-Features',
                   'SmbDirect', 'MediaPlayback', 'WindowsMediaPlayer', 'NetFx3', 'Recall', 'Containers-DisposableClientVM')
        $items = @()
        foreach ($name in $names) {
            $info = Invoke-Probe @('dism-feature-info', $name, '--log', $log) "feature-info-$name"
            if ($info -and $info.ok) {
                $items += [pscustomobject]@{ name = $name; state = $info.info.state }
                Log "    $name state=$($info.info.state) restart=$($info.info.restart)"
            }
        }
        $features = [pscustomobject]@{ features = $items }
    }
    if (-not $capabilities -or -not $capabilities.ok) {
        $names = @('Microsoft.Windows.Notepad.System~~~~0.0.1.0', 'Microsoft.Windows.WordPad~~~~0.0.1.0', 'MathRecognizer~~~~0.0.1.0',
                   'Microsoft.Windows.PowerShell.ISE~~~~0.0.1.0', 'Print.Management.Console~~~~0.0.1.0', 'OpenSSH.Client~~~~0.0.1.0',
                   'OpenSSH.Server~~~~0.0.1.0', 'Media.WindowsMediaPlayer~~~~0.0.12.0', 'Microsoft.Wallpapers.Extended~~~~0.0.1.0',
                   'XPS.Viewer~~~~0.0.1.0', 'App.StepsRecorder~~~~0.0.1.0', 'Microsoft.Windows.MSPaint~~~~0.0.1.0')
        $items = @()
        foreach ($name in $names) {
            $info = Invoke-Probe @('dism-capability-info', $name, '--log', $log) "capability-info-$($name.Split('~')[0])"
            if ($info -and $info.ok) {
                $items += [pscustomobject]@{ name = $name; state = $info.info.state }
                Log "    $name state=$($info.info.state) download=$($info.info.download_size) install=$($info.info.install_size)"
            }
        }
        $capabilities = [pscustomobject]@{ ok = [bool]$items.Count; capabilities = $items }
    }
    Log "    DismInitialize $($features.dism_initialize_ms) ms, DismOpenSession(online) $($features.dism_open_session_ms) ms"
    foreach ($group in $features.features | Group-Object state) {
        Log "    features $($group.Name) ($($group.Count)): $(($group.Group | ForEach-Object { $_.name }) -join ', ')"
    }
    if ($capabilities -and $capabilities.ok) {
        foreach ($group in $capabilities.capabilities | Group-Object state) {
            $names = @($group.Group | ForEach-Object { $_.name })
            $shown = if ($group.Name -eq 'not-present') { ($names | Select-Object -First 40) -join ', ' } else { $names -join ', ' }
            Log "    capabilities $($group.Name) ($($group.Count)): $shown"
        }
    }

    $deny = 'Recall|Hyper-V|Containers|VirtualMachine|HypervisorPlatform|Defender|NetFx|IIS|Microsoft-Windows-Subsystem-Linux|DirectPlay|SMB1|Guarded|Printing-Foundation|Windows-Identity'
    $disabled = Find-Item $features.features @('TelnetClient', 'TFTP', 'SimpleTCP', 'Client-ProjFS', 'MSMQ-Container', 'TIFFIFilter') 'staged' $deny
    $enabled = Find-Item $features.features @('Printing-XPSServices-Features', 'WorkFolders-Client', 'MSRDC-Infrastructure',
        'SearchEngine-Client-Package', 'Printing-PrintToPDFServices-Features', 'SmbDirect', 'MediaPlayback') 'installed' $deny
    $removed = Find-Item $features.features @('TelnetClient', 'TFTP', 'SimpleTCP', 'NetFx3', 'DirectPlay') 'removed' 'Recall|Hyper-V|Containers'

    if ($disabled) {
        Log "  7a: a disabled feature: $disabled, enable then disable"
        $enable = Invoke-Probe @('dism-enable', $disabled, '--log', $log) "enable-$disabled"
        Log "    $disabled before=$($enable.before.state) after=$($enable.after.state) restart=$($enable.after.restart) pending=$(Get-PendingRestart)"
        $disable = Invoke-Probe @('dism-disable', $disabled, '--log', $log) "disable-$disabled"
        Log "    $disabled before=$($disable.before.state) after=$($disable.after.state) restart=$($disable.after.restart) pending=$(Get-PendingRestart)"
        Note 'feature-enable' ("{0}: enable {1}; disable {2}" -f $disabled, (Format-Probe $enable), (Format-Probe $disable))
    } else { Log '  7a: no disabled feature with its payload' }
    if ($enabled) {
        Log "  7b: an enabled feature: $enabled, disable (payload kept) then enable"
        $disable = Invoke-Probe @('dism-disable', $enabled, '--log', $log) "disable-$enabled"
        Log "    $enabled before=$($disable.before.state) after=$($disable.after.state) restart=$($disable.after.restart) pending=$(Get-PendingRestart)"
        $enable = Invoke-Probe @('dism-enable', $enabled, '--log', $log) "enable-$enabled"
        Log "    $enabled before=$($enable.before.state) after=$($enable.after.state) restart=$($enable.after.restart) pending=$(Get-PendingRestart)"
        Note 'feature-disable' ("{0}: disable {1}; enable {2}" -f $enabled, (Format-Probe $disable), (Format-Probe $enable))
    } else { Log '  7b: no enabled feature to try' }
    if ($removed) {
        Log "  7c: a feature whose payload is removed: $removed, enable offline"
        $enable = Invoke-Probe @('dism-enable', $removed, '--log', $log) "enable-removed-$removed"
        Log "    $removed before=$($enable.before.state) after=$($enable.after.state)"
        Note 'feature-payload-removed' ("{0}: enable {1}" -f $removed, (Format-Probe $enable))
    } else { Log '  7c: no feature with its payload removed' }

    if ($capabilities -and $capabilities.ok) {
        $capDeny = 'Language\.|ShellComponents|DirectX|Ethernet|Wifi|Kernel|Client\.|DesktopAppInstaller|WMIC|VBSCRIPT'
        $installed = Find-Item $capabilities.capabilities @('Microsoft.Windows.WordPad*', 'Print.Fax.Scan*', 'MathRecognizer*',
            'Microsoft.Windows.PowerShell.ISE*', 'App.StepsRecorder*', 'OneCoreUAP.OneSync*', 'Browser.InternetExplorer*',
            'Microsoft.Windows.Notepad.System*', 'Media.WindowsMediaPlayer*', 'Hello.Face*') 'installed' $capDeny
        $absent = Find-Item $capabilities.capabilities @('OpenSSH.Server*', 'XPS.Viewer*', 'Microsoft.Windows.MSPaint*',
            'Tools.Graphics.DirectX*', 'Rsat.DNS*') 'not-present' $capDeny
        if ($installed) {
            Log "  7d: an installed capability: $installed, remove then add back offline"
            $remove = Invoke-Probe @('dism-remove-capability', $installed, '--log', $log) "remove-cap"
            Log "    $installed before=$($remove.before.state) after=$($remove.after.state) pending=$(Get-PendingRestart)"
            $add = Invoke-Probe @('dism-add-capability', $installed, '--log', $log) "add-cap-back"
            Log "    $installed before=$($add.before.state) after=$($add.after.state) pending=$(Get-PendingRestart)"
            Note 'capability-roundtrip' ("{0}: remove {1}; add back {2}" -f $installed, (Format-Probe $remove), (Format-Probe $add))
        } else { Log '  7d: no installed capability to try' }
        if ($absent) {
            Log "  7e: a capability never present: $absent, add offline"
            $add = Invoke-Probe @('dism-add-capability', $absent, '--log', $log) "add-cap-absent"
            Log "    $absent before=$($add.before.state) after=$($add.after.state)"
            Note 'capability-absent' ("{0}: add {1}" -f $absent, (Format-Probe $add))
        }
    }
    Log "  pending restart after: $(Get-PendingRestart)"
}

# Tails of the DismAPI log, DISM's own log and CBS's log, into the results.
function Save-ServicingLogs {
    $logs = @(@("$work\dismapi.log", 'spike_6b-dismapi.log'), @("$env:windir\Logs\DISM\dism.log", 'spike_6b-dism.log'),
              @("$env:windir\Logs\CBS\CBS.log", 'spike_6b-cbs.log'))
    foreach ($pair in $logs) {
        if (-not (Test-Path $pair[0])) { continue }
        try {
            Copy-Item $pair[0] "$work\copy.log" -Force
            Get-Content "$work\copy.log" -Tail 800 | Set-Content (Join-Path $results $pair[1]) -Encoding utf8
        } catch { Log "  could not copy $($pair[0]): $($_.Exception.Message)" }
    }
}

function Invoke-NonRemovableRound {
    $package = @(Get-AppxPackage | Where-Object { $_.NonRemovable -and -not $_.IsFramework } | Select-Object -First 1)
    if (-not $package.Count) { Log '  no NonRemovable package for the current user'; return }
    $record = Get-PackageRecord $package[0].PackageFullName
    Log "  NonRemovable package $($record.full): RemovePackageAsync for the current user"
    $remove = Invoke-Probe @('remove', $record.full) 'remove-nonremovable'
    Show-PackageState $record 'after the attempt'
    Note 'nonremovable' ("{0}: {1}" -f $record.name, (Format-Probe $remove))
}

try {
    $begun = Get-Date
    Log "Avel Phase 6b spike, $(Get-Date -Format s)"
    Log "OS: $([Environment]::OSVersion.VersionString) build $((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').DisplayVersion) $((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuild).$((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').UBR), user: $env:USERNAME ($currentSid)"
    New-Item -ItemType Directory -Force $work, $probeDir, $outDir | Out-Null
    Copy-Item "$bin\avel_package_probe.exe" $probeDir -Force
    Get-ChildItem $bin -Filter '*.dll' | Where-Object { $_.Name -match '^(msvcp|vcruntime|concrt)' } |
        ForEach-Object { Copy-Item $_.FullName $probeDir -Force }
    & icacls.exe $work /grant '*S-1-5-32-545:(OI)(CI)M' /T /Q | Out-Null
    if (-not (Test-Path "$bin\avel_package_probe.exe")) { throw 'avel_package_probe.exe is not in the build output' }
    Invoke-Round 'Round 0: inventory' { Invoke-InventoryRound }
    Invoke-Round 'Round 1: a signed-out second user, and running as them' { Invoke-OtherUserRound }
    if ($otherSid) {
        Invoke-Round 'Round 2: the test package for both users' { Invoke-TestPackageRound }
        if ($test) {
            Invoke-Round 'Round 3: removal for the signed-out user alone, and its undo' { Invoke-OtherRemovalRound }
            Invoke-Round 'Round 4: removal for the current user, for the last user, for all users' { Invoke-CurrentRemovalRound }
            Invoke-Round 'Round 5: provisioning the test package' { Invoke-ProvisionRound }
        }
        Invoke-Round 'Round 6: an inbox package' { Invoke-InboxRound }
    }
    Invoke-Round 'Round 7: DismAPI, features and capabilities' { Invoke-DismRound }
    Invoke-Round 'Round 8: a NonRemovable package' { Invoke-NonRemovableRound }
    Log ''
    Log 'Findings'
    foreach ($entry in $findings.GetEnumerator()) { Log "  $($entry.Key): $($entry.Value)" }
    Log ('Total {0:N0} s' -f ((Get-Date) - $begun).TotalSeconds)
} catch {
    Fail "scenario error: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
    try { Save-ServicingLogs } catch { }
    Write-Report
}
