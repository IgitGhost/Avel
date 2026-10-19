<#
.SYNOPSIS
    Starts a disposable Windows Sandbox with the current Avel build mapped in, and runs a scenario.

.DESCRIPTION
    Generates a .wsb configuration under build\sandbox and launches it. Inside the sandbox:
      C:\Avel\bin        the build output (read-only; run the `deploy` target first so it is
                         self-contained, because the sandbox has neither Qt nor the MSVC runtime)
      C:\Avel\sandbox    this folder: scenarios and downloaded test installers (read-only)
      C:\Avel\results    writable; scenario reports land in build\sandbox\results on the host
    Everything else done inside the sandbox is discarded when it closes, which makes it the
    place to exercise Avel's destructive features.

    Scenarios (scenarios\<name>.ps1) run at logon and write results\<name>.txt when done:
      logon      smoke test: avel-cli and the GUI start (default)
      uninstall  installs sample programs and uninstalls them interactively and quietly
                 (run Get-SampleInstallers.ps1 first)

    Windows Sandbox must be enabled once, from an elevated PowerShell (needs a reboot):
        Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM

.PARAMETER Preset
    The CMake preset whose output to map. Defaults to "release".

.PARAMETER Scenario
    The scenario to run at logon. Defaults to "logon".

.PARAMETER WaitForReport
    Wait until the scenario has written its report, then print it.

.PARAMETER TimeoutSeconds
    How long -WaitForReport waits before giving up. Defaults to 180.

.PARAMETER ReplaceRunning
    Stop a sandbox that is already running (only one can run at a time) instead of failing.

.PARAMETER CloseWhenDone
    With -WaitForReport: close the sandbox once the report has arrived.
#>
param(
    [string]$Preset = 'release',
    [string]$Scenario = 'logon',
    [switch]$WaitForReport,
    [int]$TimeoutSeconds = 180,
    [switch]$ReplaceRunning,
    [switch]$CloseWhenDone
)

$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$binDir = Join-Path $repoRoot "build\$Preset\bin"
$sandboxDir = $PSScriptRoot
$outDir = Join-Path $repoRoot 'build\sandbox'
$resultsDir = Join-Path $outDir 'results'
$reportPath = Join-Path $resultsDir "$Scenario.txt"
$wsb = Get-Command wsb -ErrorAction SilentlyContinue  # The sandbox CLI, on newer Windows builds

if (-not (Test-Path "$env:windir\System32\WindowsSandbox.exe") -and -not $wsb) {
    throw 'Windows Sandbox is not enabled. See the description in this script (Get-Help) for how to enable it.'
}
if (-not (Test-Path (Join-Path $sandboxDir "scenarios\$Scenario.ps1"))) {
    throw "Unknown scenario '$Scenario' (no scenarios\$Scenario.ps1)."
}
if (-not (Test-Path $binDir)) {
    throw "Build output not found at '$binDir'. Build the '$Preset' preset first."
}
if (-not (Test-Path (Join-Path $binDir 'vcruntime140.dll'))) {
    Write-Warning "The build output isn't self-contained; run: cmake --build --preset $Preset --target deploy"
}

function Get-RunningSandboxes {
    if ($wsb) { return @(& $wsb.Source list 2>$null | Where-Object { $_ -match '^[0-9a-f-]{36}$' }) }
    return @(Get-Process WindowsSandboxRemoteSession, WindowsSandboxClient -ErrorAction SilentlyContinue)
}

$running = Get-RunningSandboxes
if ($running) {
    if (-not $ReplaceRunning) {
        throw 'A Windows Sandbox is already running (only one can run at a time). Close it, or pass -ReplaceRunning.'
    }
    Write-Host 'Stopping the running Windows Sandbox'
    if ($wsb) {
        foreach ($id in $running) { & $wsb.Source stop --id $id | Out-Null }
    } else {
        $running | Stop-Process -Force
    }
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-RunningSandboxes) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
}

New-Item -ItemType Directory -Force $outDir, $resultsDir | Out-Null
if (Test-Path $reportPath) {
    Remove-Item -LiteralPath $reportPath
}
$wsbPath = Join-Path $outDir "avel-$Preset-$Scenario.wsb"

$config = @"
<Configuration>
  <VGpu>Disable</VGpu>
  <Networking>Enable</Networking>
  <MemoryInMB>4096</MemoryInMB>
  <MappedFolders>
    <MappedFolder>
      <HostFolder>$binDir</HostFolder>
      <SandboxFolder>C:\Avel\bin</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$sandboxDir</HostFolder>
      <SandboxFolder>C:\Avel\sandbox</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$resultsDir</HostFolder>
      <SandboxFolder>C:\Avel\results</SandboxFolder>
      <ReadOnly>false</ReadOnly>
    </MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\Avel\sandbox\scenarios\$Scenario.ps1</Command>
  </LogonCommand>
</Configuration>
"@

Set-Content -Path $wsbPath -Value $config -Encoding utf8
Write-Host "Starting Windows Sandbox with $wsbPath"
Start-Process $wsbPath

if ($WaitForReport) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while (-not (Test-Path $reportPath)) {
        if ((Get-Date) -gt $deadline) {
            throw "No report at '$reportPath' after $TimeoutSeconds s."
        }
        Start-Sleep -Seconds 2
    }
    Get-Content -LiteralPath $reportPath -Encoding utf8
    if ($CloseWhenDone) {
        Write-Host 'Closing the sandbox'
        if ($wsb) {
            foreach ($id in Get-RunningSandboxes) { & $wsb.Source stop --id $id | Out-Null }
        } else {
            Get-RunningSandboxes | Stop-Process -Force
        }
    }
}
