# Dot-sourced by the scenarios that drive avel-cli and compare state dumps. Expects $results,
# $cli and $scenario (the scenario's name) to be set. Defines the report with Log and Fail, a
# time-limited avel-cli runner that keeps its raw output for results\<scenario>.json, a one-line
# text for a reported location, and a section-by-section comparison of state dumps.

$report = [System.Collections.Generic.List[string]]::new()
$raw = [ordered]@{}
$failures = 0
$progressLog = Join-Path $results "$scenario.log"
Remove-Item $progressLog -ErrorAction SilentlyContinue

function Log([string]$text) {
    $report.Add($text)
    Write-Host $text
    try { Add-Content $progressLog ('{0:HH:mm:ss} {1}' -f (Get-Date), $text) -Encoding utf8 } catch { }
}

function Fail([string]$text) { $script:failures++; Log "  FAIL: $text" }

# Runs avel-cli with a time limit; returns @{ exit; text; errors; json }.
function Invoke-Cli([string[]]$arguments, [string]$label, [int]$timeoutSeconds = 300) {
    $out = Join-Path $env:TEMP "avel-$label.out"
    $err = Join-Path $env:TEMP "avel-$label.err"
    $process = Start-Process $cli ($arguments -join ' ') -NoNewWindow -PassThru -RedirectStandardOutput $out `
        -RedirectStandardError $err
    $null = $process.Handle  # Without this, ExitCode stays empty once the process has exited
    if (-not $process.WaitForExit($timeoutSeconds * 1000)) {
        & taskkill.exe /T /F /PID $process.Id | Out-Null
        Fail "$label timed out after $timeoutSeconds s"
        return @{ exit = -1; text = ''; errors = ''; json = $null }
    }
    $text = Get-Content $out -Raw -Encoding utf8
    $errors = Get-Content $err -Raw -Encoding utf8
    $raw[$label] = @{ exit = $process.ExitCode; stdout = $text; stderr = $errors }
    if ($errors) { $errors.Trim() -split "`n" | ForEach-Object { Log "  [stderr] $_" } }
    $json = $null
    if ($text) { try { $json = $text | ConvertFrom-Json } catch { } }
    return @{ exit = $process.ExitCode; text = $text; errors = $errors; json = $json }
}

function Where-Text($location) {
    switch ($location.kind) {
        { $_ -in 'folder', 'file' } { return $location.path }
        'registry-key' { return $location.key }
        'registry-value' { return "$($location.key) : $($location.value)" }
        { $_ -in 'service', 'driver' } { return $location.service }
        'task' { return $location.task }
        'package' { return $location.package_full_name }
        default { return $location.rule_id }
    }
}

# Compares two state dumps section by section; logs the first differences.
function Compare-State($expected, $actual, [string]$label) {
    $same = $true
    foreach ($spec in $expected.Keys) {
        if ($expected[$spec] -ne $actual[$spec]) {
            $same = $false
            Fail "$($label): $spec differs"
            $left = $expected[$spec] -split "`n"
            $right = $actual[$spec] -split "`n"
            $shown = 0
            for ($i = 0; $i -lt [Math]::Max($left.Count, $right.Count) -and $shown -lt 3; $i++) {
                if ($left[$i] -ne $right[$i]) {
                    Log "      expected: $($left[$i])"
                    Log "      actual:   $($right[$i])"
                    $shown++
                }
            }
        }
    }
    if ($same) { Log "  OK: $label" }
}

# Writes results\<scenario>.json (avel-cli's raw output) and, last, results\<scenario>.txt.
function Write-Report {
    Log ''
    Log $(if ($failures -eq 0) { 'RESULT: PASS' } else { "RESULT: FAIL ($failures problem(s))" })
    $raw | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $results "$scenario.json") -Encoding utf8
    Set-Content -Path (Join-Path $results "$scenario.txt") -Value $report -Encoding utf8
}
