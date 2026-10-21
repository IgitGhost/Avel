# Runs inside Windows Sandbox (Start-AvelSandbox.ps1 -Scenario force). Phase 4 acceptance test for
# Avel Force, which removes programs whose uninstaller is missing or broken without running it:
#   1. "Contoso Fixture" (fixture.ps1) with its uninstall script deleted and its app running:
#      `uninstall` must refuse (uninstaller-missing); `force --exe` must find the program; `force
#      --yes` alone must refuse to end the app; `force --yes --end-processes` must end it and
#      remove the program's folder, its Uninstall key and every leftover, leave the canaries and
#      the other program alone, and undo must give back exactly what was there;
#   2. 7-Zip's MSI installed from a copy that is then deleted, together with Windows Installer's
#      cached package: `uninstall` must refuse (msi-package-missing); `force` must remove the
#      program and Windows Installer's registration, so Windows Installer no longer knows the
#      product, and undo must give back the registration exactly;
#   3. a folder no Uninstall entry describes: `force --folder` must remove it and nothing outside
#      it, and must refuse Program Files and the Windows folder;
#   4. an Uninstall entry whose uninstaller and folder are gone: `list --broken` must show it, and
#      `force` must remove the key alone, which undo gives back;
#   5. C:\Tools\Contoso Fixture: only `scan --deep` may find it, at questionable; the volume reading
#      unit tests, skipped unelevated on the host, must pass here;
#   6. `orphans` must list a folder nobody owns and none of the installed programs' or Windows'.
# Writes results\force.json (avel-cli's output), results\force.log and, last, results\force.txt.

$ErrorActionPreference = 'Stop'
$bin = 'C:\Avel\bin'
$installers = 'C:\Avel\sandbox\installers'
$results = 'C:\Avel\results'
$cli = Join-Path $bin 'avel-cli.exe'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$scenario = 'force'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\samples.ps1"
. "$PSScriptRoot\fixture.ps1"
. "$PSScriptRoot\oracle.ps1"

$installer = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Installer'

# Windows Installer's packed form of a product code (see src/core/inventory/msi_codes.h).
function ConvertTo-PackedGuid([string]$guid) {
    $digits = $guid.Trim('{}').Replace('-', '').ToUpperInvariant()
    $order = 7, 6, 5, 4, 3, 2, 1, 0, 11, 10, 9, 8, 15, 14, 13, 12, 17, 16, 19, 18, 21, 20, 23, 22, 25, 24, 27, 26, 29, 28, 31, 30
    return -join ($order | ForEach-Object { $digits[$_] })
}

# The keys below `parent` that list the packed product code as a value.
function Get-ClientKeys([string]$parent, [string]$packed) {
    if (-not (Test-Path $parent)) { return @() }
    return @(Get-ChildItem $parent | Where-Object { $_.GetValueNames() -contains $packed } |
        ForEach-Object { $_.Name -replace '^HKEY_LOCAL_MACHINE', 'HKLM' })
}

function Get-ProductState([string]$code) {
    return (New-Object -ComObject WindowsInstaller.Installer).ProductState($code)
}

# Every planned item is done (or went with the folder or key that contains it).
function Check-Done($removal, [string]$label) {
    if (-not $removal) { Fail "$($label): nothing was removed"; return }
    Log "  change set $($removal.set_id), $($removal.items.Count) items, $($removal.quarantined_bytes) bytes quarantined"
    foreach ($item in $removal.items) {
        Log ('    {0,-4} {1,-26} {2,-15} {3}' -f $item.index, "$($item.state) ($($item.reason))", $item.location.kind,
             (Where-Text $item.location))
        if ($item.state -ne 'done' -and -not ($item.state -eq 'skipped' -and $item.reason -eq 'contained')) {
            Fail "$($label): $(Where-Text $item.location) is $($item.state) ($($item.reason))"
        }
    }
}

function Expect-Planned($removal, [string]$kind, [string]$match, [string]$label) {
    if (-not @($removal.items | Where-Object { $_.location.kind -eq $kind -and (Where-Text $_.location) -match $match })) {
        Fail "$($label): not planned: $kind $match"
    }
}

try {
    Log "Avel Phase 4 force scenario, $(Get-Date -Format s)"
    Log "OS: $([Environment]::OSVersion.VersionString), user: $env:USERNAME"
    Log "avel-cli: $((& $cli version) -join ' ')"

    # 1. A program whose uninstaller is gone, with its app running.
    Log ''
    Log 'Round 1: Contoso Fixture without its uninstaller, its app running'
    Install-ContosoFixture
    Install-Untouchables
    Remove-Item "$fixtureRoot\uninstall.ps1"
    $app = Start-Process "$fixtureRoot\app.exe" '/k' -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 1
    $fixture = @(Get-Inventory | Where-Object { $_.name -eq 'Contoso Fixture' })
    if ($fixture.Count -ne 1) { throw "Contoso Fixture not found in the inventory ($($fixture.Count))" }
    $id = "`"$($fixture[0].id)`""
    if (-not $fixture[0].uninstaller_missing) { Fail 'the inventory does not flag the missing uninstaller' }
    $programSpecs = @("file:$fixtureRoot", 'key:HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ContosoFixture')
    $specs = @(Get-FixtureLeftoverSpecs) + $programSpecs + @(Get-UntouchableSpecs)
    $before = Split-StateDump (Get-StateDump $specs)

    $plan = Invoke-Cli @('uninstall', $id, '--dry-run', '--json') 'uninstall-refused'
    $error1 = $plan.json.results[0].plan.error
    if ($error1 -ne 'uninstaller-missing') { Fail "uninstall: plan error '$error1', not uninstaller-missing" }
    else { Log '  OK: uninstall refuses: uninstaller-missing' }

    $byExe = Invoke-Cli @('force', '--exe', "`"$fixtureRoot\app.exe`"", '--dry-run', '--json') 'force-exe'
    if ($byExe.json.programs[0].id -ne $fixture[0].id) { Fail "force --exe found '$($byExe.json.programs[0].id)'" }
    else { Log '  OK: force --exe finds the program its file belongs to' }
    if (-not @($byExe.json.processes | Where-Object { $_.pid -eq $app.Id })) { Fail 'force --exe: the running app is not listed' }

    $refused = Invoke-Cli @('force', $id, '--yes', '--json') 'force-no-end'
    if ($refused.exit -ne 1 -or $refused.json.leftover_removal) { Fail "force --yes without --end-processes: exit $($refused.exit)" }
    elseif ($app.HasExited -or -not (Test-Path "$fixtureRoot\app.exe")) { Fail 'force --yes ended or removed something' }
    else { Log '  OK: --yes alone ends nothing and removes nothing' }

    $run = Invoke-Cli @('force', $id, '--yes', '--end-processes', '--json') 'force-fixture' 600
    $ended = @($run.json.processes | Where-Object { $_.pid -eq $app.Id })
    if (-not $ended -or $ended[0].outcome -notin 'closed', 'terminated') { Fail "the app wasn't ended: $($ended.outcome)" }
    else { Log "  OK: the app was ended ($($ended[0].outcome))" }
    $removal = $run.json.leftover_removal
    Check-Done $removal 'round 1'
    if ($removal) {
        Expect-Planned $removal 'folder' '^C:\\Program Files\\Contoso Fixture$' 'round 1'
        Expect-Planned $removal 'registry-key' 'Uninstall\\ContosoFixture$' 'round 1'
        foreach ($item in $removal.items) {
            $where = Where-Text $item.location
            if ($where -match 'Microsoft\\Contoso Fixture|Windows\\Contoso Fixture|Contoso Keeper' -or
                $item.location.kind -eq 'firewall-rule') {
                Fail "round 1: planned what must stay: $where"
            }
        }
        Check-Safe $run.json.leftover_scan.leftovers 'round 1 scan'
        Check-Safe $removal.items 'round 1 removal'
    }
    $after = Split-StateDump (Get-StateDump $specs)
    foreach ($spec in @(Get-FixtureLeftoverSpecs) + $programSpecs) {
        if ($after[$spec] -notmatch '^<missing>') { Fail "round 1: not removed: $spec" }
    }
    foreach ($spec in Get-UntouchableSpecs) {
        if ($after[$spec] -ne $before[$spec]) { Fail "round 1: changed what must stay: $spec" }
    }
    if (@(Get-Inventory | Where-Object { $_.name -eq 'Contoso Fixture' })) { Fail 'Contoso Fixture is still listed' }
    if ($removal) {
        $undo = Invoke-Cli @('undo', $removal.set_id, '--json') 'undo-fixture'
        if ($undo.json.state -ne 'restored') { Fail "undo: state $($undo.json.state)" }
        Compare-State $before (Split-StateDump (Get-StateDump $specs)) 'undo gave back the program and its leftovers exactly'
    }

    # 2. An MSI product whose package Windows Installer can't find.
    Log ''
    Log 'Round 2: 7-Zip (MSI) with its package and Windows Installer''s cached copy deleted'
    New-Item -ItemType Directory -Force 'C:\Temp' | Out-Null
    Copy-Item "$installers\7z-x64.msi" 'C:\Temp\7z-x64.msi' -Force
    $p = Start-Process msiexec.exe '/i "C:\Temp\7z-x64.msi" /qn /norestart' -Wait -PassThru
    if ($p.ExitCode -notin 0, 3010) { throw "installing 7-Zip returned $($p.ExitCode)" }
    Remove-Item 'C:\Temp\7z-x64.msi'
    $sevenZip = @(Get-Inventory | Where-Object { $_.name -match '^7-Zip ' })
    if ($sevenZip.Count -ne 1) { throw "7-Zip not found in the inventory ($($sevenZip.Count))" }
    $code = $sevenZip[0].msi_product_code
    $packed = ConvertTo-PackedGuid $code
    $cached = (Get-ItemProperty "$installer\UserData\S-1-5-18\Products\$packed\InstallProperties").LocalPackage
    Log "  product $code ($packed), cached package $cached"
    Remove-Item $cached -Force
    $sevenZip = @(Get-Inventory | Where-Object { $_.name -match '^7-Zip ' })
    if (-not $sevenZip[0].msi_package_missing) { Fail 'the inventory does not flag the missing package' }

    # @() keeps a single key an array: PowerShell unrolls what a function returns.
    $upgradeCodes = @(Get-ClientKeys 'HKLM:\SOFTWARE\Classes\Installer\UpgradeCodes' $packed)
    $components = @(Get-ClientKeys "$installer\UserData\S-1-5-18\Components" $packed)
    Log "  $($upgradeCodes.Count) upgrade code(s), $($components.Count) component(s)"
    $registration = @(
        "key:HKLM\SOFTWARE\Classes\Installer\Products\$packed",
        "key:HKLM\SOFTWARE\Classes\Installer\Features\$packed",
        "key:HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData\S-1-5-18\Products\$packed"
    ) + @(@($upgradeCodes) + @($components) | ForEach-Object { "key:$_" })
    $msiSpecs = $registration + @("key:$($sevenZip[0].registry.path)", 'file:C:\Program Files\7-Zip',
                                  'key:HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\Folders')
    $before = Split-StateDump (Get-StateDump $msiSpecs)
    foreach ($spec in $registration) {
        if ($before[$spec] -match '^<missing>') { Fail "7-Zip not registered: $spec" }
    }

    $id = "`"$($sevenZip[0].id)`""
    $plan = Invoke-Cli @('uninstall', $id, '--dry-run', '--json') 'uninstall-msi-refused'
    $error2 = $plan.json.results[0].plan.error
    if ($error2 -ne 'msi-package-missing') { Fail "uninstall: plan error '$error2', not msi-package-missing" }
    else { Log '  OK: uninstall refuses: msi-package-missing' }

    $run = Invoke-Cli @('force', $id, '--yes', '--json') 'force-msi' 600
    $removal = $run.json.leftover_removal
    Check-Done $removal 'round 2'
    if ($removal) {
        Expect-Planned $removal 'folder' '^C:\\Program Files\\7-Zip$' 'round 2'
        Expect-Planned $removal 'registry-key' "Classes\\Installer\\Products\\$packed$" 'round 2'
        Expect-Planned $removal 'registry-key' "UserData\\S-1-5-18\\Products\\$packed$" 'round 2'
        Check-Safe $run.json.leftover_scan.leftovers 'round 2 scan'
        Check-Safe $removal.items 'round 2 removal'
    }
    $after = Split-StateDump (Get-StateDump $msiSpecs)
    foreach ($spec in $registration) {
        $left = Get-Item ($spec.Substring(4) -replace '^HKLM', 'HKLM:') -ErrorAction SilentlyContinue
        if ($left -and $left.GetValueNames() -contains $packed) { Fail "round 2: still lists the product: $spec" }
        elseif ($left -and $spec -match "\\$packed$") { Fail "round 2: not removed: $spec" }
    }
    if ($after['file:C:\Program Files\7-Zip'] -notmatch '^<missing>') { Fail 'round 2: 7-Zip''s folder is still there' }
    $state = Get-ProductState $code
    if ($state -ne -1) { Fail "Windows Installer still knows 7-Zip (state $state)" }
    else { Log '  OK: Windows Installer no longer knows the product' }
    if ($removal) {
        $undo = Invoke-Cli @('undo', $removal.set_id, '--json') 'undo-msi'
        if ($undo.json.state -ne 'restored') { Fail "undo: state $($undo.json.state)" }
        Compare-State $before (Split-StateDump (Get-StateDump $msiSpecs)) 'undo gave back the registration exactly'
        $state = Get-ProductState $code
        if ($state -ne 5) { Fail "after undo Windows Installer reports state $state, not installed (5)" }
        else { Log '  OK: after undo Windows Installer knows the product again' }
    }

    # 3. A folder no Uninstall entry describes.
    Log ''
    Log 'Round 3: a folder without an Uninstall entry'
    $orphan = 'C:\Program Files\Contoso Orphan'
    New-Item -ItemType Directory -Force "$orphan\bin" | Out-Null
    Copy-Item "$env:WINDIR\System32\cmd.exe" "$orphan\bin\orphan.exe" -Force
    Set-Content "$orphan\readme.txt" 'orphaned' -Encoding ascii
    $before = Split-StateDump (Get-StateDump @("file:$orphan"))
    $run = Invoke-Cli @('force', '--folder', "`"$orphan`"", '--yes', '--json') 'force-folder'
    $removal = $run.json.leftover_removal
    Check-Done $removal 'round 3'
    if ($removal) {
        Expect-Planned $removal 'folder' '^C:\\Program Files\\Contoso Orphan$' 'round 3'
        foreach ($item in $removal.items) {
            if ((Where-Text $item.location) -notmatch 'Contoso Orphan') { Fail "round 3: planned outside the folder: $(Where-Text $item.location)" }
        }
        Check-Safe $removal.items 'round 3 removal'
    }
    if (Test-Path $orphan) { Fail 'round 3: the folder is still there' }
    foreach ($protected in 'C:\Program Files', "$env:WINDIR\System32", 'C:\Users') {
        $no = Invoke-Cli @('force', '--folder', "`"$protected`"", '--dry-run') "force-refused-$(Split-Path $protected -Leaf)"
        if ($no.exit -ne 2 -or $no.errors -notmatch 'protected') { Fail "force --folder $protected was not refused (exit $($no.exit))" }
        else { Log "  OK: refuses $protected" }
    }
    if ($removal) {
        $undo = Invoke-Cli @('undo', $removal.set_id, '--json') 'undo-folder'
        Compare-State $before (Split-StateDump (Get-StateDump @("file:$orphan"))) 'undo gave back the folder exactly'
    }

    # 4. A dead entry: its uninstaller and its folder are gone, only the Uninstall key is left.
    Log ''
    Log 'Round 4: an Uninstall entry whose program is gone'
    $deadKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NorthwindDeadTool'
    New-Item $deadKey -Force | Out-Null
    New-ItemProperty $deadKey -Name DisplayName -Value 'Northwind Dead Tool' | Out-Null
    New-ItemProperty $deadKey -Name Publisher -Value 'Northwind Traders' | Out-Null
    New-ItemProperty $deadKey -Name InstallLocation -Value 'C:\Program Files\Northwind Dead Tool' | Out-Null
    New-ItemProperty $deadKey -Name UninstallString -Value '"C:\Program Files\Northwind Dead Tool\uninst.exe" /S' | Out-Null
    $deadSpecs = @('key:HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NorthwindDeadTool')
    $before = Split-StateDump (Get-StateDump $deadSpecs)
    $broken = Invoke-Cli @('list', '--broken', '--json') 'list-broken'
    $dead = @($broken.json.programs | Where-Object { $_.name -eq 'Northwind Dead Tool' })
    if (-not $dead -or -not $dead[0].uninstaller_missing) { Fail 'list --broken does not show the dead entry' }
    else { Log "  OK: list --broken shows it: $($dead[0].id)" }
    foreach ($entry in $broken.json.programs) {
        Log "    broken: $($entry.name)"
        if (-not $entry.broken) { Fail "list --broken shows an entry that isn't broken: $($entry.name)" }
    }
    $listed = Invoke-Cli @('list', '--broken') 'list-broken-text'
    if ($listed.text -notmatch 'Northwind Dead Tool[\s\S]*uninstaller-missing, no-install-folder') {
        Fail 'list --broken (text) does not give the reasons'
    }
    if ($dead) {
        $run = Invoke-Cli @('force', "`"$($dead[0].id)`"", '--yes', '--json') 'force-dead'
        $removal = $run.json.leftover_removal
        Check-Done $removal 'round 4'
        if ($removal -and @($removal.items).Count -ne 1) { Fail "round 4: $(@($removal.items).Count) items planned, not just the key" }
        elseif ($removal) { Expect-Planned $removal 'registry-key' 'Uninstall\\NorthwindDeadTool$' 'round 4' }
        if (Test-Path $deadKey) { Fail 'round 4: the key is still there' }
        if ($removal) {
            $undo = Invoke-Cli @('undo', $removal.set_id, '--json') 'undo-dead'
            Compare-State $before (Split-StateDump (Get-StateDump $deadSpecs)) 'undo gave back the dead entry exactly'
        }
    }

    # 5. The deep search: a folder named after the program outside the common locations.
    Log ''
    Log 'Round 5: deep search'
    # stdout only: under 'Stop', Windows PowerShell turns a native program's stderr into an error.
    $tests = & "$bin\avel_core_tests.exe" '--gtest_filter=VolumeFoldersTest.*:DeepScanTest.*'
    $tests | Where-Object { $_ -match '^\[  (PASSED|FAILED|SKIPPED) ' } | ForEach-Object { Log "  $_" }
    if ($LASTEXITCODE -ne 0 -or ($tests -match 'SKIPPED')) { Fail 'the deep search unit tests did not all pass elevated' }
    else { Log '  OK: the volume reading tests pass elevated' }
    $tool = 'C:\Tools\Contoso Fixture'
    New-Item -ItemType Directory -Force $tool | Out-Null
    Set-Content "$tool\notes.txt" 'a copy kept elsewhere' -Encoding ascii
    $fixture = @(Get-Inventory | Where-Object { $_.name -eq 'Contoso Fixture' })
    $id = "`"$($fixture[0].id)`""
    $plain = Invoke-Cli @('scan', $id, '--all-levels', '--json') 'scan-plain'
    if (@($plain.json.leftovers | Where-Object { (Where-Text $_.location) -eq $tool })) { Fail "found $tool without --deep" }
    $deep = Invoke-Cli @('scan', $id, '--deep', '--all-levels', '--json') 'scan-deep' 600
    $hit = @($deep.json.leftovers | Where-Object { (Where-Text $_.location) -eq $tool })
    if (-not $hit) { Fail "--deep did not find $tool" }
    elseif ($hit[0].found_by -ne 'deep-search' -or $hit[0].confidence -ne 'questionable') {
        Fail "--deep found $tool by $($hit[0].found_by) at $($hit[0].confidence), not deep-search at questionable"
    } else { Log "  OK: only --deep finds $tool, at questionable" }
    foreach ($item in @($deep.json.leftovers | Where-Object { $_.found_by -eq 'deep-search' })) {
        Log "    deep-search: $($item.confidence) $(Where-Text $item.location)"
    }
    foreach ($diagnostic in $deep.json.diagnostics) {
        if ($diagnostic.code -eq 'deep-search-unavailable') { Fail "deep search: $($diagnostic.subject) unreadable ($($diagnostic.error))" }
    }
    $stage = @($deep.json.timings | Where-Object { $_.stage -eq 'deep' })
    if ($stage) { Log "  deep search read the volumes in $($stage[0].elapsed_ms) ms; the scan took $($deep.json.elapsed_ms) ms" }
    Check-Safe $deep.json.leftovers 'round 5'

    # 6. Orphaned folders. The fixture is installed again (undo in round 1), so its folder and
    # "Contoso Orphan" (named after its publisher, restored in round 3) are owned.
    Log ''
    Log 'Round 6: orphaned folders'
    $northwind = 'C:\Program Files\Northwind Orphan'
    New-Item -ItemType Directory -Force $northwind | Out-Null
    Set-Content "$northwind\readme.txt" 'nobody owns this' -Encoding ascii
    $orphans = Invoke-Cli @('orphans', '--all-levels', '--json') 'orphans'
    $paths = @($orphans.json.leftovers | ForEach-Object { Where-Text $_.location })
    foreach ($path in $paths) { Log "    orphan: $path" }
    if ($paths -notcontains $northwind) { Fail "orphans does not list $northwind" }
    else { Log "  OK: orphans lists $northwind" }
    foreach ($owned in 'C:\Program Files\7-Zip', $fixtureRoot, 'C:\Program Files\Contoso Orphan',
                       'C:\Program Files\Common Files', 'C:\Program Files\WindowsApps', 'C:\Program Files\Windows Defender',
                       'C:\ProgramData\Microsoft', 'C:\ProgramData\Package Cache') {
        if ($paths -contains $owned) { Fail "orphans lists an owned folder: $owned" }
    }
    foreach ($item in $orphans.json.leftovers) {
        if ($item.found_by -ne 'orphans' -or $item.confidence -notin 'questionable', 'bad') {
            Fail "orphans: $(Where-Text $item.location) is $($item.found_by) at $($item.confidence)"
        }
    }
    Check-Safe $orphans.json.leftovers 'round 6'
} catch {
    Fail "scenario error: $($_.Exception.Message) at line $($_.InvocationInfo.ScriptLineNumber)"
} finally {
    Write-Report
}
