# Runs inside Windows Sandbox (Start-AvelSandbox.ps1 -Scenario extensions, which starts it without
# networking). Phase 6c acceptance test for Avel Tools' browser extensions (`avel-cli extensions`,
# Tools > Browser extensions...), on synthetic profiles and sources (extensions_fixture.ps1): no
# browser can install anything offline, so the scenario checks the listing, the registry, the
# profiles' files and undo, not the browsers' own reaction (ADR 0009, "No spike").
#   1. Fixtures: Chrome, Edge, Brave, Opera and Firefox profiles of the current user, a Chrome
#      profile of a signed-out second user (AvelOther, made without a logon; their hive must not be
#      loaded), and every kind of stubborn source in HKLM (both views), HKCU, AvelOther's hive and a
#      fake Firefox install folder's policies.json, beside unrelated policy values.
#   2. List: `extensions list --all-users --json` must show every fixture entry once with its
#      browser, user, profile, name (one from __MSG_), install, store, enabled, permissions,
#      removal, keeps_user_copy and sources (kind, key, value, 32-bit view, ignored_by_browser,
#      owner program); no component extension, theme, built-in add-on or "allowed" policy entry;
#      the profiles with their names, the extensions pages of Edge and Firefox, and exactly the two
#      diagnostics the fixtures plant. Without --all-users AvelOther's entries are left out and the
#      others keep their ids; --removable and --browser filter.
#   3. Refusals change nothing (state dumps): an unknown id, a store-only extension
#      (not-removable), policies.json's add-on (managed-by-policies-json, also by its add-on id), a
#      subkey named for two ids (not-supported), a request mixing a removable entry with a refused
#      one; dry runs plan without changing anything (by entry id, and by extension id: all three
#      entries of the shared machine policy).
#   4. Removals, one change set each: the exact registry afterwards (lists renumbered without a gap,
#      values past a gap left alone or deleted alone, JSON without the extension's key and the rest
#      kept, an emptied JSON value deleted, registration keys and values gone, every other value
#      of the dumped keys unchanged), the profiles' files byte for byte as before (Avel never writes
#      Preferences, Secure Preferences, extensions.json or policies.json), the listing afterwards
#      (entries gone, or still in their profile with nothing left to remove), then `undo` must give
#      back identical dumps. Includes the shared machine policy (`affected`: three profiles of two
#      users), two extensions from one list in one pass, the keeps-user-copy note, and AvelOther's
#      registration (their hive free again after the removal and the undo).
#   5. Crash recovery: AVEL_TEST_FAULT_AFTER stops a removal after its first edit (the list holds a
#      value twice); the next start settles the set (done, then skipped as interrupted), and undo
#      gives back identical dumps.
#   6. GUI (extensions_gui_round.ps1): Tools > Browser extensions..., the shared policy removed after
#      a confirmation that names the other profiles, undone; a value changed behind the window's
#      back between its listing and Remove is skipped, not overwritten (compare-and-set); Open
#      extensions page starts Edge through Explorer, not elevated, on edge://extensions; avel.exe
#      exits with 0.
# After every avel-cli run the signed-out user's hive must be free (not under HKU, NTUSER.DAT opens
# exclusively). Writes results\extensions.json (avel-cli's output), results\extensions.log and,
# last, results\extensions.txt.

$ErrorActionPreference = 'Stop'
$bin = 'C:\Avel\bin'
$results = 'C:\Avel\results'
$cli = Join-Path $bin 'avel-cli.exe'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$scenario = 'extensions'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\extensions_fixture.ps1"

$callNumber = 0
$fixtures = @()
$registrySpecs = $null
$fileSpecs = $null
$allSpecs = @()
$start = $null  # The dump right after the fixtures; every undo must give it back

# Where an error record comes from: file:line.
function Get-ErrorPlace($record) {
    $file = $record.InvocationInfo.ScriptName
    $where = if ($file) { Split-Path -Leaf $file } else { 'line' }
    return "$($where):$($record.InvocationInfo.ScriptLineNumber)"
}

function Invoke-Round([string]$name, [scriptblock]$body) {
    Log ''
    Log $name
    $started = Get-Date
    try {
        & $body
    } catch {
        Fail "$($name): $($_.Exception.Message) at $(Get-ErrorPlace $_)"
        try { if (Test-Path "Registry::HKEY_USERS\$offlineMount") { Dismount-OtherHive } } catch { }
    }
    Log ('  ({0:N0} s)' -f ((Get-Date) - $started).TotalSeconds)
}

# Two lists hold the same strings, in any order.
function Test-SameSet($left, $right) {
    return (@($left | ForEach-Object { "$_" } | Sort-Object) -join "`n") -eq (@($right | ForEach-Object { "$_" } | Sort-Object) -join "`n")
}

# avel-cli extensions <arguments>; then the signed-out user's hive must be free again.
function Invoke-Extensions([string[]]$arguments, [string]$label, [int]$expectExit = 0) {
    $script:callNumber++
    $run = Invoke-Cli (@('extensions') + $arguments) ('{0:00}-{1}' -f $script:callNumber, $label)
    Test-OtherHiveFree $label
    if ($run.exit -ne $expectExit) { Fail "$($label): exit $($run.exit), not $expectExit" }
    return $run
}

function Get-ExtensionsList([string]$label, [string[]]$options = @('--all-users')) {
    $run = Invoke-Extensions (@('list') + $options + @('--json')) $label
    if (-not $run.json) { throw "$($label): extensions list gave no JSON" }
    return $run.json
}

function Get-Fixture([string]$label) {
    $found = @($fixtures | Where-Object { $_.label -eq $label })
    if ($found.Count -ne 1) { throw "no fixture '$label'" }
    return $found[0]
}

# The listed entries a fixture stands for.
function Find-Listed($list, $fixture) {
    @($list.extensions | Where-Object {
        $_.browser -eq $fixture.browser -and "$($_.user_sid)" -eq $fixture.sid -and
        [bool]$_.in_profile -eq ($null -ne $fixture.profile) -and
        ($null -eq $fixture.profile -or "$($_.profile_directory)" -eq $fixture.profile) -and
        $(if ($fixture.ext) { "$($_.extension_id)" -eq $fixture.ext } else { -not $_.extension_id -and "$($_.name)" -eq $fixture.name }) })
}

function Get-Listed($list, $fixture, [string]$label) {
    $found = @(Find-Listed $list $fixture)
    if ($found.Count -ne 1) { Fail "$($label): $($fixture.label) listed $($found.Count) time(s)"; return $null }
    return $found[0]
}

function Format-Strings($list) { return (@($list) | ForEach-Object { "$_" }) -join ', ' }

# Checks one listed entry against its fixture; returns the problems.
function Test-ListedEntry($entry, $fixture) {
    $problems = [System.Collections.Generic.List[string]]::new()
    $check = { param($what, $actual, $wanted) if ("$actual" -ne "$wanted") { $problems.Add("$what '$actual', not '$wanted'") } }
    if ($entry.id -notmatch '^[0-9a-f]{10}$') { $problems.Add("id '$($entry.id)'") }
    if ($fixture.name -or $fixture.ext) { & $check 'name' $entry.name $fixture.name }
    & $check 'install' $entry.install $fixture.install
    & $check 'store' $entry.store $fixture.store
    & $check 'from_store' ([bool]$entry.from_store) ($fixture.store -ne 'none')
    & $check 'removal' $entry.removal $fixture.removal
    & $check 'removable' ([bool]$entry.removable) ($fixture.removal -eq 'removable')
    & $check 'keeps_user_copy' ([bool]$entry.keeps_user_copy) $fixture.keepsCopy
    if ($null -ne $fixture.profile) { & $check 'enabled' ([bool]$entry.enabled) $fixture.enabled }
    if ($fixture.owner -eq 'other' -and "$($entry.user_name)" -notmatch "\\$otherUser$") { $problems.Add("user $($entry.user_name)") }
    $sources = @($entry.sources | ForEach-Object { Format-ListedSource $_ })
    if (-not (Test-SameSet $sources $fixture.sources)) {
        $problems.Add("sources [$(Format-Strings $sources)], not [$(Format-Strings $fixture.sources)]")
    }
    if ($null -ne $fixture.permissions) { & $check 'permissions' (Format-Strings $entry.permissions) (Format-Strings $fixture.permissions) }
    if ($null -ne $fixture.hosts) { & $check 'host_permissions' (Format-Strings $entry.host_permissions) (Format-Strings $fixture.hosts) }
    if ($null -ne $fixture.optional) { & $check 'optional_permissions' (Format-Strings $entry.optional_permissions) (Format-Strings $fixture.optional) }
    return $problems.ToArray()
}

# --- Rounds -------------------------------------------------------------------------------------

function Invoke-FixtureRound {
    $edgeRunning = @(Get-Process msedge -ErrorAction SilentlyContinue).Count
    $lua = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System' -ErrorAction SilentlyContinue).EnableLUA
    Log "  Edge: $edgeRunning msedge process(es) running; User Data there: $(Test-Path $edgeData); EnableLUA=$lua"
    New-OtherUser
    Test-OtherHiveFree 'profile created'
    Install-ExtensionFixtures
    Test-OtherHiveFree 'fixtures installed'
    $script:fixtures = Get-ExpectedEntries
    $script:registrySpecs = Get-RegistrySpecs
    $script:fileSpecs = Get-FileSpecs
    $script:allSpecs = @($registrySpecs.Values) + @($fileSpecs.Values)
    $script:start = Get-ExtensionsDump $allSpecs
    foreach ($spec in $registrySpecs.Values) {
        if ($start[$spec] -match '^<missing>') { Fail "fixture key missing: $spec" }
    }
    Log "  $($fixtures.Count) expected entries, $($registrySpecs.Count) registry keys and $($fileSpecs.Count) folders dumped"
}

function Invoke-ListRound {
    $all = Get-ExtensionsList 'list-all-users'
    if (-not $all.elevated) { Fail 'avel-cli does not run elevated' }
    if ($all.policies_managed) { Fail 'policies_managed: the Sandbox counts as domain-joined or enrolled' }
    Log "  $(@($all.extensions).Count) entries in $(@($all.profiles).Count) profiles, listed in $($all.elapsed_ms) ms"

    # Diagnostics: exactly the two the fixtures plant; none about the users.
    $diagnostics = @($all.diagnostics | Where-Object { $_.source -eq 'extensions' } | ForEach-Object { "$($_.code)|$($_.subject)".ToLowerInvariant() })
    $wanted = @("profile-unreadable|$chromeData\Profile 2\Preferences".ToLowerInvariant()
                "policy-unreadable|HKU\$currentSid\Software\Policies\BraveSoftware\Brave\ExtensionSettings".ToLowerInvariant())
    if (-not (Test-SameSet $diagnostics $wanted)) { Fail "extension diagnostics [$(Format-Strings $diagnostics)], not [$(Format-Strings $wanted)]" }
    else { Log '  OK: diagnostics: the unreadable Preferences and the policy that is not JSON' }
    foreach ($diagnostic in @($all.diagnostics | Where-Object { $_.source -ne 'extensions' })) {
        Log "  diagnostic: $($diagnostic.source) $($diagnostic.code) $($diagnostic.subject) $($diagnostic.error)"
        if ($diagnostic.subject -in $currentSid, $otherSid) { Fail "a user's registry couldn't be read: $($diagnostic.code) $($diagnostic.subject)" }
    }

    # Profiles.
    # Edge's own profile may have siblings Edge made; they are logged.
    $expectedProfiles = Get-ExpectedProfiles
    $profiles = @($all.profiles | Where-Object { $_.user_sid -in $currentSid, $otherSid } |
                  ForEach-Object { "$($_.browser)|$($_.user_sid)|$($_.directory)|$($_.name)" })
    $extra = @($profiles | Where-Object { $_ -notin $expectedProfiles })
    $missing = @($expectedProfiles | Where-Object { $_ -notin $profiles })
    foreach ($line in $extra) { if ($line -like 'edge|*') { Log "  also listed: profile $line" } else { Fail "profile listed: $line" } }
    foreach ($line in $missing) { Fail "profile not listed: $line" }
    if (-not $missing.Count) { Log "  OK: $($expectedProfiles.Count) profiles with their names" }

    # Every fixture entry.
    foreach ($fixture in $fixtures) {
        $entry = Get-Listed $all $fixture 'list'
        if (-not $entry) { continue }
        $fixture.id = $entry.id
        $fixture.entry = $entry
        $problems = @(Test-ListedEntry $entry $fixture)
        $where = if ($null -eq $fixture.profile) { 'no profile' } else { "'$($fixture.profile)'" }
        Log ('  {0} {1,-18} {2,-8} {3,-7} {4,-20} {5,-17} {6}' -f $entry.id, $fixture.label, $entry.browser, $fixture.owner, $where,
             $entry.removal, (@($entry.sources | ForEach-Object { "$($_.kind)$(if ($_.ignored_by_browser) { '(ignored)' })" }) -join ' '))
        foreach ($problem in $problems) { Fail "list, $($fixture.label): $problem" }
    }

    # Details: __MSG_ names, merged preferences, times, folders, pages.
    $bad = $failures
    $store = (Get-Fixture 'chrome-store').entry
    if ($store) {
        if ($store.description -ne 'Helps from the store') { Fail "list: the store extension's description '$($store.description)'" }
        if ($store.version -ne '1.0.0') { Fail "list: version $($store.version)" }
        if ($store.install_time -ne 1695526400000) { Fail "list: install_time $($store.install_time), not 1695526400000" }
        if ($store.location -ne '1') { Fail "list: location $($store.location)" }
        if ($store.folder -ine "$chromeData\Default\Extensions\$($ids.store)\1.0.0_0") { Fail "list: folder $($store.folder)" }
        if ($store.page.executable -or $store.page.command_line) { Fail "list: Chrome isn't installed, yet its page runs '$($store.page.command_line)'" }
        if ((Format-Strings $store.page.arguments) -ne '--profile-directory=Default, chrome://extensions') { Fail "list: Chrome's page arguments $(Format-Strings $store.page.arguments)" }
    }
    $disabled = (Get-Fixture 'work-disabled').entry
    if ($disabled -and $disabled.disable_reasons -ne 1) { Fail "list: disable_reasons $($disabled.disable_reasons), not 1" }
    $unpacked = (Get-Fixture 'chrome-unpacked').entry
    if ($unpacked -and ($unpacked.folder -ine $unpackedRoot -or $unpacked.location -ne '4')) { Fail "list: the unpacked extension's folder $($unpacked.folder), location $($unpacked.location)" }
    $edge = (Get-Fixture 'edge-forced').entry
    if ($edge) {
        Log "  Edge's extensions page: $($edge.page.command_line)"
        if (-not $edge.page.executable -or -not (Test-Path -LiteralPath $edge.page.executable)) { Fail "list: Edge's program '$($edge.page.executable)' isn't there" }
        if ((Format-Strings $edge.page.arguments) -ne '--profile-directory=Default, edge://extensions') { Fail "list: Edge's page arguments $(Format-Strings $edge.page.arguments)" }
    }
    $fox = (Get-Fixture 'firefox-store').entry
    if ($fox) {
        if ($fox.page.executable -ine "$firefoxRoot\firefox.exe") { Fail "list: Firefox's program '$($fox.page.executable)'" }
        if ((Format-Strings $fox.page.arguments) -ine "-profile, $firefoxProfile, about:addons") { Fail "list: Firefox's page arguments $(Format-Strings $fox.page.arguments)" }
        if ($fox.folder -ine "$firefoxProfile\extensions\$($firefoxIds.store).xpi") { Fail "list: Firefox add-on file $($fox.folder)" }
    }
    $opera = (Get-Fixture 'opera-store').entry
    if ($opera -and (Format-Strings $opera.page.arguments) -ne 'opera://extensions') { Fail "list: Opera's page arguments $(Format-Strings $opera.page.arguments)" }
    $partner = (Get-Fixture 'machine-partner').entry
    if ($partner -and @($partner.sources)[0].settings_key -ne "$($ids.settings),$($ids.partner)") { Fail "list: settings_key $(@($partner.sources)[0].settings_key)" }
    if ($failures -eq $bad) { Log '  OK: names from _locales, merged preferences, install time, folders, extensions pages' }

    # What must not be listed.
    $hidden = @($ids.component, $ids.theme, $ids.allowed, 'builtin@contoso.example', 'theme@contoso.example')
    $shown = @($all.extensions | Where-Object { $_.extension_id -in $hidden } | ForEach-Object { $_.extension_id })
    if ($shown) { Fail "list shows $(Format-Strings $shown)" } else { Log '  OK: no component extension, theme, built-in add-on or "allowed" entry' }
    $known = @($fixtures | Where-Object { $_.id } | ForEach-Object { $_.id })
    foreach ($other in @($all.extensions | Where-Object { $_.id -notin $known })) {
        Log "  also listed: $($other.id) $($other.browser) $($other.user_name) '$($other.profile_directory)' $($other.extension_id) $($other.name)"
    }

    # Without --all-users: no AvelOther, the same ids for the rest.
    $mine = Get-ExtensionsList 'list-current-user' @()
    $bad = $failures
    foreach ($fixture in $fixtures) {
        $found = @(Find-Listed $mine $fixture)
        if ($fixture.owner -eq 'other') { if ($found.Count) { Fail "list without --all-users shows $($fixture.label)" } }
        elseif ($found.Count -ne 1 -or $found[0].id -ne $fixture.id) { Fail "list without --all-users: $($fixture.label) $($found.Count) time(s), id $($found[0].id)" }
    }
    if (@($mine.profiles | Where-Object { $_.user_sid -eq $otherSid }).Count) { Fail "list without --all-users shows $otherUser's profiles" }
    if ($failures -eq $bad) { Log "  OK: without --all-users: $otherUser's entries left out, the same ids for the others" }

    # --removable and --browser.
    $removable = Get-ExtensionsList 'list-removable' @('--all-users', '--removable')
    $wantedIds = @($all.extensions | Where-Object { $_.removable } | ForEach-Object { $_.id })
    if (-not (Test-SameSet @($removable.extensions | ForEach-Object { $_.id }) $wantedIds)) { Fail '--removable lists other entries than the removable ones' }
    else { Log "  OK: --removable: $($wantedIds.Count) entries" }
    $firefox = Get-ExtensionsList 'list-firefox' @('--all-users', '--browser', 'firefox')
    $wantedIds = @($all.extensions | Where-Object { $_.browser -eq 'firefox' } | ForEach-Object { $_.id })
    if (-not (Test-SameSet @($firefox.extensions | ForEach-Object { $_.id }) $wantedIds) -or
        @($firefox.profiles | Where-Object { $_.browser -ne 'firefox' }).Count) { Fail '--browser firefox lists other entries or profiles' }
    else { Log "  OK: --browser firefox: $($wantedIds.Count) entries" }
}

# extensions remove <ids> --yes (or --dry-run) --json.
function Invoke-Remove([string[]]$ids, [string]$label, [int]$expectExit = 0, [switch]$DryRun) {
    $mode = if ($DryRun) { '--dry-run' } else { '--yes' }
    $run = Invoke-Extensions (@('remove') + $ids + @($mode, '--json')) $label $expectExit
    if (-not $run.json) { throw "$($label): no JSON output" }
    return $run.json
}

function Test-Refused($json, [hashtable]$expected, [string]$label) {
    $issues = @($json.issues | ForEach-Object { "$($_.id)=$($_.code)" })
    $wanted = @($expected.Keys | ForEach-Object { "$_=$($expected[$_])" })
    $bad = $failures
    if (-not (Test-SameSet $issues $wanted)) { Fail "$($label): issues [$(Format-Strings $issues)], not [$(Format-Strings $wanted)]" }
    if ($json.change) { Fail "$($label): a change set was made ($($json.change.set_id))" }
    if (@($json.plan).Count) { Fail "$($label): a plan of $(@($json.plan).Count) item(s)" }
    if ($failures -eq $bad) { Log "  OK: $label ($(Format-Strings $issues))" }
}

function Format-PlanItem($item) { return "$($item.kind)|$($item.key)|$($item.value)" }

function Invoke-RefusalRound {
    $before = Get-ExtensionsDump $allSpecs
    $forced = Get-Fixture 'chrome-forced'
    $json = Invoke-Remove @('ffffffffff') 'refuse-unknown' 1
    Test-Refused $json @{ 'ffffffffff' = 'unknown-id' } 'an unknown id'
    $store = Get-Fixture 'chrome-store'
    $json = Invoke-Remove @($store.id) 'refuse-store' 1
    Test-Refused $json @{ $store.id = 'not-removable' } 'a store extension'
    $managed = Get-Fixture 'firefox-managed'
    $json = Invoke-Remove @($managed.id) 'refuse-policies-json' 1
    Test-Refused $json @{ $managed.id = 'managed-by-policies-json' } "policies.json's add-on"
    $json = Invoke-Remove @($managed.ext) 'refuse-policies-json-by-addon-id' 1
    Test-Refused $json @{ $managed.ext = 'managed-by-policies-json' } "policies.json's add-on by its id"
    $comma = Get-Fixture 'brave-comma'
    $json = Invoke-Remove @($comma.id) 'refuse-comma-subkey' 1
    Test-Refused $json @{ $comma.id = 'not-supported' } 'a policy subkey named for two ids'
    $json = Invoke-Remove @($forced.id, $store.id) 'refuse-mixed' 1
    Test-Refused $json @{ $store.id = 'not-removable' } 'a removable entry with a refused one'
    if (@($json.planned).Count -ne 1) { Fail "refuse-mixed: planned $(Format-Strings $json.planned)" }

    $json = Invoke-Remove @($forced.id) 'dry-run' -DryRun
    $plan = @($json.plan | ForEach-Object { Format-PlanItem $_ })
    $chromeList = 'HKLM\SOFTWARE\Policies\Google\Chrome\ExtensionInstallForcelist'
    $bad = $failures
    if (-not (Test-SameSet $plan @("value-edit|$chromeList|2", "value-edit|$chromeList|3"))) { Fail "dry run: plan [$(Format-Strings $plan)]" }
    if ($json.change -or @($json.issues).Count -or -not $json.dry_run) { Fail 'dry run: a change set, issues, or not a dry run' }
    if (@($json.affected).Count -ne 3) { Fail "dry run: $(@($json.affected).Count) affected entries, not 3" }
    if ($failures -eq $bad) { Log "  OK: dry run by entry id: $($plan.Count) value edits planned, 3 entries affected" }
    $json = Invoke-Remove @($forced.ext) 'dry-run-extension-id' -DryRun
    $wanted = @('chrome-forced', 'work-forced', 'other-forced' | ForEach-Object { (Get-Fixture $_).id })
    if (-not (Test-SameSet @($json.planned) $wanted) -or @($json.plan).Count -ne 2) {
        Fail "dry run by extension id: planned [$(Format-Strings $json.planned)], $(@($json.plan).Count) plan item(s)"
    } else { Log '  OK: dry run by extension id: its three entries planned, the two value edits once' }
    Compare-State $before (Get-ExtensionsDump $allSpecs) 'refusals and dry runs changed nothing'
}

# The registry after a change: every dumped key as at the start with `changes` (per registry spec
# name) applied, and nothing else; the profiles' files exactly as at the start.
function Test-StateAfter($after, [hashtable]$changes, [string]$label, [string[]]$skipFiles = @()) {
    $bad = $failures
    foreach ($name in $registrySpecs.Keys) {
        $spec = $registrySpecs[$name]
        $before = ConvertTo-RegistryMap $start[$spec]
        $expected = Get-ExpectedMap $before $(if ($changes -and $changes[$name]) { $changes[$name] } else { @{} })
        $problems = @(Compare-RegistryMap $expected (ConvertTo-RegistryMap $after[$spec]))
        foreach ($problem in $problems | Select-Object -First 6) { Fail "$($label): $spec $problem" }
    }
    foreach ($name in $fileSpecs.Keys) {
        if (@($skipFiles | Where-Object { $name -like "$_*" }).Count) { continue }
        $spec = $fileSpecs[$name]
        if ($after[$spec] -ne $start[$spec]) {
            Fail "$($label): files changed in $spec"
            $left = $start[$spec] -split "`n"
            $right = $after[$spec] -split "`n"
            for ($i = 0; $i -lt [Math]::Max($left.Count, $right.Count); $i++) {
                if ($left[$i] -ne $right[$i]) { Log "      was: $($left[$i])"; Log "      now: $($right[$i])"; break }
            }
        }
    }
    if ($failures -eq $bad) { Log "  OK: $label" }
}

function Invoke-Undo([string]$setId, [string]$label) {
    if (-not $setId) { return }
    $script:callNumber++
    $run = Invoke-Cli @('undo', $setId, '--json') ('{0:00}-{1}' -f $script:callNumber, $label)
    Test-OtherHiveFree $label
    $items = @($run.json.items | ForEach-Object { "$($_.index)=$($_.state)$(if ($_.reason -and $_.reason -ne 'none') { " ($($_.reason))" })" }) -join ', '
    if ($run.json.state -ne 'restored' -or $run.exit -ne 0) { Fail "$($label): set $setId is $($run.json.state) (exit $($run.exit)): $items" }
    else { Log "  OK: $label ($setId restored: $items)" }
}

# The change sets in the store (undo --list, which also settles interrupted ones).
function Get-ChangeSets([string]$label) {
    $script:callNumber++
    $run = Invoke-Cli @('undo', '--list', '--json') ('{0:00}-{1}' -f $script:callNumber, $label)
    if ($run.exit -ne 0) { throw "undo --list returned $($run.exit)" }
    return @(@($run.json) | Where-Object { $_ })
}

# undo --show: the set with its items.
function Get-SetDetails([string]$setId, [string]$label) {
    $script:callNumber++
    $run = Invoke-Cli @('undo', '--show', $setId, '--json') ('{0:00}-{1}' -f $script:callNumber, $label)
    if ($run.exit -ne 0 -or -not $run.json) { throw "undo --show $setId returned $($run.exit)" }
    foreach ($item in @($run.json.items)) {
        Log ('    {0,-4} {1,-10} {2,-12} {3,-14} {4}' -f $item.index, $item.state, $item.reason, $item.location.kind, (Where-Text $item.location))
    }
    return $run.json
}

# A removal of `remove` (fixture labels; "ext:<label>" for its extension id), checked against the
# plan, the registry afterwards (`changes`), the files, the listing afterwards (`after`: a label's
# removal code, or 'gone'; `sources`: a label's sources afterwards), then undone.
function New-Case([string]$tag, [string]$name, [string[]]$remove, [string[]]$affected, [string[]]$plan, [hashtable]$changes,
                  [hashtable]$after, [hashtable]$sources = @{}, [string[]]$notes = @()) {
    [pscustomobject]@{ tag = $tag; name = $name; remove = $remove; affected = $affected; plan = $plan; changes = $changes
                       after = $after; sources = $sources; notes = $notes }
}

function Get-RemovalCases {
    $hku = "HKU\$currentSid"
    $chromeList = 'HKLM\SOFTWARE\Policies\Google\Chrome\ExtensionInstallForcelist'
    $userList = "$hku\Software\Policies\Google\Chrome\ExtensionInstallForcelist"
    $edgeList = 'HKLM\SOFTWARE\Policies\Microsoft\Edge\ExtensionInstallForcelist'
    $firefoxPolicy = 'HKLM\SOFTWARE\Policies\Mozilla\Firefox\Extensions'
    $list = 'V Chrome\ExtensionInstallForcelist'
    $forcedAt1 = Format-ExpectedSource 'force-list' $chromeList '1'
    @(
        New-Case 'forced' 'the machine policy three profiles of two users share (forcelist "2" of 3)' @('chrome-forced') `
            @('chrome-forced', 'work-forced', 'other-forced') @("value-edit|$chromeList|2", "value-edit|$chromeList|3") `
            @{ hklmChrome = @{ copy = @{ "$list|2" = "$list|3" }; delete = @("$list|3") } } `
            @{ 'chrome-forced' = 'not-removable'; 'work-forced' = 'not-removable'; 'other-forced' = 'not-removable' } `
            @{ 'machine-y' = @(Format-ExpectedSource 'force-list' $chromeList '2') }
        New-Case 'two-in-one-list' 'two extensions of one list by their extension ids, in one pass' @('ext:machine-x', 'ext:machine-y') `
            @('machine-x', 'machine-y') @("value-edit|$chromeList|1", "value-edit|$chromeList|2", "value-edit|$chromeList|3") `
            @{ hklmChrome = @{ copy = @{ "$list|1" = "$list|2" }; delete = @("$list|2", "$list|3") } } `
            @{ 'machine-x' = 'gone'; 'machine-y' = 'gone' } `
            @{ 'chrome-forced' = @($forcedAt1); 'work-forced' = @($forcedAt1); 'other-forced' = @($forcedAt1) }
        New-Case 'user-list' "the current user's forcelist ('2' of 3)" @('user-q') @('user-q') @("value-edit|$userList|2", "value-edit|$userList|3") `
            @{ hkcuChrome = @{ copy = @{ "$list|2" = "$list|3" }; delete = @("$list|3") } } @{ 'user-q' = 'gone' } `
            @{ 'user-r' = @(Format-ExpectedSource 'force-list' $userList '2') }
        New-Case 'json-settings' 'ExtensionSettings as JSON: the id taken out of its "id1,id2" key, the rest kept' @('work-settings') `
            @('work-settings') @('value-edit|HKLM\SOFTWARE\Policies\Google\Chrome|ExtensionSettings') `
            @{ hklmChrome = @{ set = @{ 'V Chrome|ExtensionSettings' = (Get-StringData $chromeSettingsAfter) } } } `
            @{ 'work-settings' = 'not-removable' }
        New-Case 'json-emptied' "Edge's user ExtensionSettings with one key: the value is deleted" @('edge-json') @('edge-json') `
            @("value-edit|$hku\Software\Policies\Microsoft\Edge|ExtensionSettings") @{ hkcuEdge = @{ delete = @('V Edge|ExtensionSettings') } } `
            @{ 'edge-json' = 'not-removable' }
        New-Case 'registration-32' "a registration in HKLM's 32-bit view, its file in a program's folder" @('chrome-registered') `
            @('chrome-registered') @("registry-key|HKLM\SOFTWARE\WOW6432Node\Google\Chrome\Extensions\$($ids.registered)|") `
            @{ hklmReg32 = @{ deleteKey = @("Extensions\$($ids.registered)") } } @{ 'chrome-registered' = 'not-removable' }
        New-Case 'registration-64' "a registration in HKLM's 64-bit view (not read by the browser)" @('machine-64') @('machine-64') `
            @("registry-key|HKLM\SOFTWARE\Google\Chrome\Extensions\$($ids.machine64)|") `
            @{ hklmReg64 = @{ deleteKey = @("Extensions\$($ids.machine64)") } } @{ 'machine-64' = 'gone' }
        New-Case 'keeps-user-copy' "the user's registration of an extension they installed from the store: the copy stays" @('chrome-keeper') `
            @('chrome-keeper') @("registry-key|$hku\Software\Google\Chrome\Extensions\$($ids.keeper)|") `
            @{ hkcuReg = @{ deleteKey = @("Extensions\$($ids.keeper)") } } @{ 'chrome-keeper' = 'not-removable' } @{} @('chrome-keeper=keeps-user-copy')
        New-Case 'signed-out-user' "$otherUser's registration, in their unloaded hive" @('other-registered') @('other-registered') `
            @("registry-key|HKU\$otherSid\Software\Google\Chrome\Extensions\$($ids.other)|") `
            @{ otherReg = @{ deleteKey = @("Extensions\$($ids.other)") } } @{ 'other-registered' = 'not-removable' }
        New-Case 'firefox-lists' "Firefox's Extensions\Install ('2' of 3) and Locked ('1' of 1)" @('firefox-policy') @('firefox-policy') `
            @("value-edit|$firefoxPolicy\Install|2", "value-edit|$firefoxPolicy\Install|3", "value-edit|$firefoxPolicy\Locked|1") `
            @{ hklmFirefox = @{ copy = @{ 'V Firefox\Extensions\Install|2' = 'V Firefox\Extensions\Install|3' }
                                delete = @('V Firefox\Extensions\Install|3', 'V Firefox\Extensions\Locked|1') } } `
            @{ 'firefox-policy' = 'not-removable' } @{ 'firefox-path' = @(Format-ExpectedSource 'firefox-install' "$firefoxPolicy\Install" '2') }
        New-Case 'firefox-registry' "Firefox's registry value" @('firefox-registered') @('firefox-registered') `
            @("registry-value|HKLM\SOFTWARE\Mozilla\Firefox\Extensions|$($firefoxIds.registered)") `
            @{ hklmFirefoxReg = @{ delete = @("V Extensions|$($firefoxIds.registered)") } } @{ 'firefox-registered' = 'not-removable' }
        New-Case 'settings-subkey' "Brave's ExtensionSettings subkey" @('brave-sub') @('brave-sub') `
            @("registry-key|HKLM\SOFTWARE\Policies\BraveSoftware\Brave\ExtensionSettings\$($ids.braveSub)|") `
            @{ hklmBrave = @{ deleteKey = @("Brave\ExtensionSettings\$($ids.braveSub)") } } @{ 'brave-sub' = 'not-removable' }
        New-Case 'before-gap' "Edge's forcelist '1', with '3' past a gap left alone" @('edge-forced') @('edge-forced') `
            @("value-edit|$edgeList|1") @{ hklmEdge = @{ delete = @('V Edge\ExtensionInstallForcelist|1') } } @{ 'edge-forced' = 'not-removable' }
        New-Case 'past-gap' "Edge's forcelist '3', past the gap (not read by the browser): deleted alone" @('edge-ignored') @('edge-ignored') `
            @("value-edit|$edgeList|3") @{ hklmEdge = @{ delete = @('V Edge\ExtensionInstallForcelist|3') } } @{ 'edge-ignored' = 'gone' }
    )
}

# The listing after a removal: each fixture as at the start, but for `after` and `sources`.
function Test-ListAfter($list, $case) {
    $bad = $failures
    foreach ($fixture in $fixtures) {
        $found = @(Find-Listed $list $fixture)
        $state = $case.after[$fixture.label]
        if ($state -eq 'gone') {
            if ($found.Count) { Fail "$($case.tag), listed after: $($fixture.label) is still listed" }
            continue
        }
        if ($found.Count -ne 1) { Fail "$($case.tag), listed after: $($fixture.label) $($found.Count) time(s)"; continue }
        $entry = $found[0]
        if ($entry.id -ne $fixture.id) { Fail "$($case.tag), listed after: $($fixture.label) has id $($entry.id), not $($fixture.id)" }
        $removal = if ($state) { $state } else { $fixture.removal }
        if ($entry.removal -ne $removal) { Fail "$($case.tag), listed after: $($fixture.label) is $($entry.removal), not $removal" }
        $wanted = if ($case.sources.ContainsKey($fixture.label)) { @($case.sources[$fixture.label]) }
                  elseif ($state) { @() } else { $fixture.sources }
        $sources = @($entry.sources | ForEach-Object { Format-ListedSource $_ })
        if (-not (Test-SameSet $sources $wanted)) { Fail "$($case.tag), listed after: $($fixture.label) sources [$(Format-Strings $sources)], not [$(Format-Strings $wanted)]" }
        if ($state -and $entry.keeps_user_copy) { Fail "$($case.tag), listed after: $($fixture.label) still keeps_user_copy" }
    }
    if ($failures -eq $bad) { Log '  OK: the listing afterwards' }
}

function Get-CaseDescription($case) {
    $planned = @($case.remove | ForEach-Object { Get-Fixture ($_ -replace '^ext:', '') })
    $names = @($planned | ForEach-Object { if ($_.ext) { $_.ext } else { $_.name } } | Select-Object -Unique)
    if ($names.Count -ne 1) { return "Remove $($names.Count) browser extensions" }
    $first = $planned[0]
    return 'Remove browser extension ' + $(if ($first.name) { $first.name } else { $first.ext })
}

function Invoke-RemovalCase($case) {
    Log "  -- $($case.tag): $($case.name)"
    $arguments = @($case.remove | ForEach-Object {
        if ($_ -like 'ext:*') { (Get-Fixture $_.Substring(4)).ext } else { (Get-Fixture $_).id } })
    $json = Invoke-Remove $arguments "remove-$($case.tag)"
    $bad = $failures
    $planned = @($case.remove | ForEach-Object { (Get-Fixture ($_ -replace '^ext:', '')).id })
    if (-not (Test-SameSet @($json.planned) $planned)) { Fail "$($case.tag): planned [$(Format-Strings $json.planned)]" }
    $affected = @($case.affected | ForEach-Object { (Get-Fixture $_).id })
    if (-not (Test-SameSet @($json.affected | ForEach-Object { $_.id }) $affected)) {
        Fail "$($case.tag): affected [$(Format-Strings @($json.affected | ForEach-Object { "$($_.id) $($_.browser) $($_.user_name) '$($_.profile_name)'" }))]"
    }
    $notes = @($case.notes | ForEach-Object { $parts = $_ -split '='; "$((Get-Fixture $parts[0]).id)=$($parts[1])" })
    if (-not (Test-SameSet @($json.notes | ForEach-Object { "$($_.id)=$($_.code)" }) $notes)) { Fail "$($case.tag): notes [$(Format-Strings @($json.notes | ForEach-Object { "$($_.id)=$($_.code)" }))]" }
    $plan = @($json.plan | ForEach-Object { Format-PlanItem $_ })
    if (-not (Test-SameSet $plan $case.plan)) { Fail "$($case.tag): plan [$(Format-Strings $plan)], not [$(Format-Strings $case.plan)]" }
    $description = Get-CaseDescription $case
    if ($json.description -ne $description) { Fail "$($case.tag): description '$($json.description)', not '$description'" }
    if (@($json.issues).Count) { Fail "$($case.tag): issues $(Format-Strings @($json.issues | ForEach-Object { "$($_.id)=$($_.code)" }))" }
    $setId = $json.change.set_id
    if (-not $setId) { Fail "$($case.tag): no change set" }
    foreach ($item in @($json.change.items)) {
        if ($item.state -ne 'done') { Fail "$($case.tag): $(Where-Text $item.location) is $($item.state) ($($item.reason))" }
    }
    if ($failures -eq $bad) {
        Log "  OK: change set $setId '$($json.description)': $(@($json.change.items).Count) item(s) done, $($affected.Count) entr$(if ($affected.Count -eq 1) { 'y' } else { 'ies' }) affected$(if ($notes) { ", note $($case.notes -join ', ')" })"
    }
    Test-StateAfter (Get-ExtensionsDump $allSpecs) $case.changes "$($case.tag): the registry as planned, nothing else changed, the profiles' files as they were"
    Test-ListAfter (Get-ExtensionsList "list-after-$($case.tag)") $case
    Invoke-Undo $setId "undo-$($case.tag)"
    Compare-State $start (Get-ExtensionsDump $allSpecs) "$($case.tag): undo gave back every key and file exactly"
}

function Invoke-RemovalRound {
    foreach ($case in Get-RemovalCases) {
        try { Invoke-RemovalCase $case }
        catch { Fail "$($case.tag): $($_.Exception.Message) at $(Get-ErrorPlace $_)" }
    }
}

function Invoke-CrashRound {
    $forced = Get-Fixture 'chrome-forced'
    $sets = Get-ChangeSets 'crash-sets-before'
    $script:callNumber++
    $env:AVEL_TEST_FAULT_AFTER = '2'
    try { $crash = Invoke-Cli @('extensions', 'remove', $forced.id, '--yes', '--json') ('{0:00}-crash-after-first-edit' -f $script:callNumber) }
    finally { Remove-Item Env:\AVEL_TEST_FAULT_AFTER }
    Test-OtherHiveFree 'crash'
    if ($crash.exit -ne 99) { Fail "crash: avel-cli exited with $($crash.exit), not 99" }
    else { Log '  avel-cli stopped after 2 journal records (the first edit done; exit 99)' }
    $list = 'V Chrome\ExtensionInstallForcelist'
    Test-StateAfter (Get-ExtensionsDump $allSpecs) @{ hklmChrome = @{ copy = @{ "$list|2" = "$list|3" } } } `
        'crash: the first edit made ("2" now holds "3"''s entry, which is there twice), the second not'
    $known = @($sets | ForEach-Object { $_.id })
    $new = @(Get-ChangeSets 'crash-sets-after' | Where-Object { $_.id -notin $known })
    if ($new.Count -ne 1) { Fail "crash: $($new.Count) new change set(s), not 1"; return }
    $set = $new[0]
    Log "  change set $($set.id) '$($set.description)': $($set.state) after the next start"
    if ($set.state -ne 'complete') { Fail "crash: the interrupted set is $($set.state), not complete" }
    $show = Get-SetDetails $set.id 'crash-show'
    $states = @($show.items | ForEach-Object { "$($_.state)/$($_.reason)" })
    if ((Format-Strings $states) -ne 'done/none, skipped/interrupted') { Fail "crash: settled as [$(Format-Strings $states)], not [done/none, skipped/interrupted]" }
    else { Log '  OK: settled: the first edit done, the second skipped (interrupted)' }
    Invoke-Undo $set.id 'crash-undo'
    Compare-State $start (Get-ExtensionsDump $allSpecs) 'crash: undo gave back every key and file exactly'
}

try {
    Log "Avel Phase 6c extensions scenario, $(Get-Date -Format s)"
    Log "OS: $([Environment]::OSVersion.VersionString), user: $env:USERNAME ($currentSid)"
    Log "avel-cli: $((& $cli version) -join ' ')"
    Invoke-Round 'Round 1: fixtures, a signed-out second user' { Invoke-FixtureRound }
    if ($start) {
        Invoke-Round 'Round 2: list' { Invoke-ListRound }
        if (@($fixtures | Where-Object { -not $_.id }).Count) {
            Fail 'not every fixture is listed; the rounds that change them are skipped'
        } else {
            Invoke-Round 'Round 3: refusals and dry runs change nothing' { Invoke-RefusalRound }
            Invoke-Round 'Round 4: removals, each checked and undone' { Invoke-RemovalRound }
            Invoke-Round 'Round 5: crash recovery' { Invoke-CrashRound }
            . "$PSScriptRoot\extensions_gui_round.ps1"
            Invoke-Round 'Round 6: GUI (Tools > Browser extensions)' { Invoke-ExtensionsGuiRound }
        }
    }
} catch {
    Fail "scenario error: $($_.Exception.Message) at $(Get-ErrorPlace $_)"
} finally {
    Write-Report
}
