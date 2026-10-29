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
        { $_ -in 'registry-value', 'value-edit' } { return "$($location.key) : $($location.value)" }
        { $_ -in 'service', 'driver' } { return $location.service }
        { $_ -in 'task', 'task-folder' } { return $location.task }
        'package' { return $location.package_full_name }
        default { return $location.rule_id }
    }
}

# avel_state_dump's canonical text of `specs` (tests/helpers/state_dump.cpp: key:, value:, file:,
# service:, task:), one "== spec" section each.
function Get-StateDump([string[]]$specs) {
    $output = & (Join-Path (Split-Path $cli) 'avel_state_dump.exe') @specs
    if ($LASTEXITCODE -ne 0) { throw "avel_state_dump failed ($LASTEXITCODE)" }
    return ($output -join "`n")
}

# Splits a dump into its "== spec" sections.
function Split-StateDump([string]$dump) {
    $sections = [ordered]@{}
    $current = $null
    foreach ($line in $dump -split "`n") {
        if ($line.StartsWith('== ')) { $current = $line.Substring(3); $sections[$current] = '' }
        elseif ($current) { $sections[$current] += $line + "`n" }
    }
    return $sections
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

# Oracle captures (avel_capture, tests/helpers/capture.cpp): files, folders, registry keys and
# values listed with plain Win32 calls, independently of Avel. The diff of two captures tells what
# really changed in between. See capture.cpp for the line format.

# Captures `specs` into `path`. `skip`: patterns avel_capture leaves out (with everything below a
# matching folder or key). Returns avel_capture's summary lines ("spec ...", "total: ...",
# "excluded <count> <pattern>").
function New-Capture([string]$path, [string[]]$specs, [string[]]$skip = @()) {
    $exe = Join-Path (Split-Path $cli) 'avel_capture.exe'
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    $arguments = @('capture', $path) + $specs
    if ($skip) {
        [IO.File]::WriteAllLines("$path.exclude", [string[]]$skip, [Text.UTF8Encoding]::new($false))
        $arguments += @('--exclude', "$path.exclude")
    }
    # stdout only: under 'Stop', Windows PowerShell turns a native program's stderr into an error.
    $output = & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "avel_capture capture failed ($LASTEXITCODE): $($output -join ' ')" }
    return $output
}

# One line of a capture diff: sign (+, - or ~), kind (D, F, K, V or !), path, attributes, text.
function ConvertFrom-CaptureLine([string]$line) {
    $rest = $line.Substring(4)
    $tab = $rest.IndexOf("`t")
    $path = if ($tab -ge 0) { $rest.Substring(0, $tab) } else { $rest }
    $attributes = if ($tab -ge 0) { $rest.Substring($tab + 1) } else { '' }
    return [pscustomobject]@{ sign = $line.Substring(0, 1); kind = $line.Substring(2, 1); path = $path
                              attributes = $attributes; text = $line }
}

# The differences between two captures, parsed. Wrap calls in @(): one line comes back bare.
function Get-CaptureDiff([string]$before, [string]$after) {
    $exe = Join-Path (Split-Path $cli) 'avel_capture.exe'
    $out = "$after.diff"
    $null = & $exe diff $before $after $out
    if ($LASTEXITCODE -gt 1) { throw "avel_capture diff failed ($LASTEXITCODE)" }
    # ReadAllLines, not Get-Content: its strings carry note properties.
    foreach ($line in [IO.File]::ReadAllLines($out, [Text.Encoding]::UTF8)) {
        if ($line.Length -ge 4) { ConvertFrom-CaptureLine $line }
    }
}

# The reason of the first `known` pattern (an ordered dictionary or hashtable of pattern = reason)
# that `path` matches, or $null.
function Get-KnownReason([string]$path, $known) {
    if (-not $known) { return $null }
    foreach ($entry in $known.GetEnumerator()) {
        if ($path -match $entry.Key) { return $entry.Value }
    }
    return $null
}

# Compares two captures that should be the same. Every difference fails, except those whose path
# matches an -Exclude pattern (counted) or a -Known pattern (logged as KNOWN with its reason).
# Returns the differences left unexplained.
function Compare-Capture([string]$before, [string]$after, [string]$label, $Known = @{}, [string[]]$Exclude = @()) {
    $lines = @(Get-CaptureDiff $before $after)
    $unexplained = [System.Collections.Generic.List[object]]::new()
    $excluded = 0
    foreach ($line in $lines) {
        if (@($Exclude | Where-Object { $line.path -match $_ }).Count) { $excluded++; continue }
        $reason = Get-KnownReason $line.path $Known
        if ($reason) { Log "  KNOWN: $($reason): $($line.text)"; continue }
        $unexplained.Add($line)
    }
    if ($unexplained.Count) {
        Fail "$($label): $($unexplained.Count) unexplained difference(s)"
        foreach ($line in $unexplained) { Log "      $($line.text)" }
    } else {
        Log "  OK: $label ($($lines.Count) difference(s), $excluded excluded)"
    }
    return $unexplained.ToArray()
}

# Writes results\<scenario>.json (avel-cli's raw output) and, last, results\<scenario>.txt.
function Write-Report {
    Log ''
    Log $(if ($failures -eq 0) { 'RESULT: PASS' } else { "RESULT: FAIL ($failures problem(s))" })
    $raw | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $results "$scenario.json") -Encoding utf8
    Set-Content -Path (Join-Path $results "$scenario.txt") -Value $report -Encoding utf8
}
