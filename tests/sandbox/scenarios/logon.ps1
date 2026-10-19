# Runs when the Windows Sandbox session starts (see Start-AvelSandbox.ps1).
# Smoke-tests the mapped build, writes a report to C:\Avel\results\logon.txt (visible on the host
# in build\sandbox\results), then leaves the GUI and a PowerShell window open for manual testing.

$bin = 'C:\Avel\bin'
$env:Path = "$bin;$env:Path"
[Environment]::SetEnvironmentVariable('Path', $env:Path, 'Machine')

# avel-cli writes UTF-8 when redirected; decode it as such.
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$report = [System.Collections.Generic.List[string]]::new()
$report.Add("Avel sandbox smoke test, $(Get-Date -Format s)")
$report.Add("OS: $([Environment]::OSVersion.VersionString), user: $env:USERNAME")

$cliOutput = & "$bin\avel-cli.exe" version 2>&1
$report.Add("avel-cli version (exit $LASTEXITCODE):")
$cliOutput | ForEach-Object { $report.Add("  $_") }

$gui = Start-Process "$bin\avel.exe" -PassThru
$idle = $gui.WaitForInputIdle(15000)
Start-Sleep -Seconds 1
$gui.Refresh()
$report.Add("avel.exe: started=$idle running=$(-not $gui.HasExited) window='$($gui.MainWindowTitle)'")

# Written in one go at the end, so the host never reads a half-written report.
Set-Content -Path 'C:\Avel\results\logon.txt' -Value $report -Encoding utf8

Start-Process powershell.exe -ArgumentList '-NoExit', '-NoProfile', '-Command', @'
Write-Host 'Avel sandbox. Build: C:\Avel\bin   Scenarios: C:\Avel\sandbox\scenarios' -ForegroundColor Cyan
Write-Host 'Everything done here is discarded when the sandbox closes.' -ForegroundColor Yellow
'@
