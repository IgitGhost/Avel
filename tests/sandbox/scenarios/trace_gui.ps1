# Runs inside Windows Sandbox (Start-AvelSandbox.ps1 -Scenario trace_gui). Phase 5c's GUI round of
# Avel Trace on its own, the round the full `trace` scenario runs last (trace_gui_round.ps1 says what
# it does): avel.exe driven through UI Automation in English (Install and trace, Start and Stop
# tracing, the review and an exclusion, the Trace column and the Traces dialog, uninstalling with
# the trace, the Undo Center, Revert by trace and the startup prompt), judged with the same oracle
# and KNOWN rules as the CLI rounds (trace_oracle.ps1). Takes a few minutes.
# Writes results\trace_gui.json (avel-cli's output), results\trace_gui.log,
# results\trace-oracle-gui-<step>.txt (each oracle diff, every line with what accounted for it or
# judged it) and, last, results\trace_gui.txt.

$ErrorActionPreference = 'Stop'
$bin = 'C:\Avel\bin'
$installers = 'C:\Avel\sandbox\installers'
$results = 'C:\Avel\results'
$cli = Join-Path $bin 'avel-cli.exe'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$scenario = 'trace_gui'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\oracle.ps1"
. "$PSScriptRoot\trace_fixture.ps1"
. "$PSScriptRoot\trace_oracle.ps1"
. "$PSScriptRoot\gui.ps1"
. "$PSScriptRoot\trace_gui_round.ps1"

try {
    Log "Avel Phase 5c trace GUI scenario, $(Get-Date -Format s)"
    Log "OS: $([Environment]::OSVersion.VersionString), user: $env:USERNAME ($sid), $([Environment]::ProcessorCount) processors"
    Log "avel-cli: $((& $cli version) -join ' ')"
    if (Test-Path $captures) { Remove-Item $captures -Recurse -Force }
    Invoke-TraceGuiRound
} catch {
    Fail "scenario error: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
    Write-Report
}
