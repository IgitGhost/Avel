# Dot-sourced by the extensions scenario (and its GUI round). Synthetic browser profiles and the
# "stubborn" sources that install extensions from outside them (docs/tools.md, "Browser
# extensions"), offline: no browser installs anything here, and only Edge is really installed.
#   Profiles of the current user: Chrome (Local State with three profiles: Default and Profile 1
#     with extensions, Profile 2 whose Preferences isn't JSON), Edge (the Sandbox's own profile,
#     which Edge made in the image: the fixtures' entries are added to its Default Preferences),
#     Brave (no Local State), Opera (the folder is the profile) and Firefox (profiles.ini,
#     extensions.json), with manifests (one name from __MSG_), store,
#     user, developer, external and policy installs, a disabled one, a component one and a theme.
#   A signed-out second user, AvelOther (made as in startup_fixture.ps1), with a Chrome profile
#     and a registration in their hive.
#   Sources: Chrome's ExtensionInstallForcelist in HKLM and HKCU (3 values each), Edge's in HKLM
#     with a value past a gap, Chrome's ExtensionSettings as JSON (with an "id1,id2" key, "*" and
#     an "allowed" entry), Edge's in HKCU as JSON with one key, Brave's as subkeys (one named for
#     two ids) and an HKCU one that isn't JSON, registrations in HKLM's 32-bit view (with a path
#     in a program's folder), HKLM's 64-bit view (not read by the browser), HKCU and AvelOther's
#     hive, Firefox's Extensions value, Extensions\Install and Locked, and a fake Firefox install
#     folder whose distribution\policies.json installs one add-on. Unrelated policy values sit in
#     the same keys and must never change.
# Expects common.ps1 to be dot-sourced first (Fail, Log, Get-StateDump, Split-StateDump).

$storeUrl = 'https://clients2.google.com/service/update2/crx'
$edgeStoreUrl = 'https://edge.microsoft.com/extensionwebstorebase/v1/crx'
$operaStoreUrl = 'https://extension-updates.opera.com/api/omaha/update/'
$otherUser = 'AvelOther'
# Where the scenario loads the signed-out user's hive while it writes fixtures to it or dumps it.
# Avel never sees it there: the hive is unloaded again before each avel-cli run.
$offlineMount = 'AvelExtensionsOffline'
$currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$otherSid = $null
$otherProfile = $null
$otherHive = $null

$programRoot = 'C:\Program Files\Contoso Extensions'
$firefoxRoot = 'C:\Program Files\Mozilla Firefox'
$policiesJson = "$firefoxRoot\distribution\policies.json"
$unpackedRoot = 'C:\ContosoDev\unpacked'
$chromeData = Join-Path $env:LOCALAPPDATA 'Google\Chrome\User Data'
$edgeData = Join-Path $env:LOCALAPPDATA 'Microsoft\Edge\User Data'
$braveData = Join-Path $env:LOCALAPPDATA 'BraveSoftware\Brave-Browser\User Data'
$operaData = Join-Path $env:APPDATA 'Opera Software\Opera Stable'
$firefoxData = Join-Path $env:APPDATA 'Mozilla\Firefox'
$firefoxProfile = "$firefoxData\Profiles\avel1234.default-release"
$otherChromeData = $null
$edgeProfileName = 'Default'  # Edge's Default profile's display name, from its Local State

# Chromium ids are 32 letters a-p: a pair of them, 16 times.
function New-ExtensionId([string]$pair) { return $pair * 16 }
$ids = [ordered]@{
    store = New-ExtensionId 'ab'; forced = New-ExtensionId 'bc'; registered = New-ExtensionId 'cd'
    keeper = New-ExtensionId 'de'; settings = New-ExtensionId 'ef'; partner = New-ExtensionId 'fg'
    allowed = New-ExtensionId 'gh'; edgeForced = New-ExtensionId 'hi'; edgeIgnored = New-ExtensionId 'ij'
    machine64 = New-ExtensionId 'jk'; userP = New-ExtensionId 'kl'; userQ = New-ExtensionId 'lm'
    userR = New-ExtensionId 'mn'; machineX = New-ExtensionId 'no'; machineY = New-ExtensionId 'op'
    other = New-ExtensionId 'pa'; braveSub = New-ExtensionId 'ac'; braveComma = New-ExtensionId 'bd'
    bravePartner = New-ExtensionId 'ce'; edgeJson = New-ExtensionId 'df'; edgeStore = New-ExtensionId 'eg'
    opera = New-ExtensionId 'fh'; disabled = New-ExtensionId 'gi'; otherStore = New-ExtensionId 'hj'
    unpacked = New-ExtensionId 'ik'; component = New-ExtensionId 'jl'; theme = New-ExtensionId 'km'
}
$firefoxIds = [ordered]@{
    store = 'store@contoso.example'; registered = 'registered@contoso.example'
    policy = 'policy@contoso.example'; policiesJson = 'pj@contoso.example'
}
$firefoxUrls = [ordered]@{
    other = 'https://contoso.example/other.xpi'; policy = 'https://contoso.example/policy.xpi'
    local = 'C:\Contoso\local.xpi'; policiesJson = 'https://contoso.example/pj.xpi'
}
# The original JSON of Chrome's ExtensionSettings policy, spaced as an administrator would write it.
$chromeSettingsJson = (
    '{ "*": { "installation_mode": "blocked", "blocked_install_message": "Ask IT first" },' +
    ' "' + $ids.settings + ',' + $ids.partner + '": { "installation_mode": "force_installed", "update_url": "' + $storeUrl + '" },' +
    ' "' + $ids.allowed + '": { "installation_mode": "allowed" } }')
# What Avel writes once it takes the extension out: compact, the order and the rest kept.
$chromeSettingsAfter = (
    '{"*":{"installation_mode":"blocked","blocked_install_message":"Ask IT first"},' +
    '"' + $ids.partner + '":{"installation_mode":"force_installed","update_url":"' + $storeUrl + '"},' +
    '"' + $ids.allowed + '":{"installation_mode":"allowed"}}')

Add-Type -Namespace AvelExtensions -Name UserEnv -MemberDefinition @'
[DllImport("userenv.dll", CharSet = CharSet.Unicode)]
public static extern int CreateProfile(string sid, string userName, System.Text.StringBuilder profilePath, uint size);
'@

# --- The signed-out user (as startup_fixture.ps1) ------------------------------------------------

# Runs a command line through cmd.exe with its output thrown away; returns the exit code.
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
    $hr = [AvelExtensions.UserEnv]::CreateProfile($otherSid, $otherUser, $path, 260)
    if ($hr -ne 0) { throw ('CreateProfile for {0} returned 0x{1:X8}' -f $otherUser, $hr) }
    $script:otherProfile = $path.ToString()
    $script:otherHive = Join-Path $otherProfile 'NTUSER.DAT'
    $script:otherChromeData = Join-Path $otherProfile 'AppData\Local\Google\Chrome\User Data'
    if (-not (Test-Path $otherHive)) { throw "no NTUSER.DAT in $otherProfile" }
    $deadline = (Get-Date).AddSeconds(60)
    while ((Test-Path "Registry::HKEY_USERS\$otherSid") -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
    Log "  user $otherUser ($otherSid), profile $otherProfile"
}

# Fails when the signed-out user's hive is loaded, by anyone: mounted under HKU, or held open
# (RegLoadAppKey keeps NTUSER.DAT open exclusively).
function Test-OtherHiveFree([string]$label) {
    if (-not $otherHive) { return }
    if (Test-Path "Registry::HKEY_USERS\$otherSid") { Fail "$($label): $otherUser's hive is loaded (HKU\$otherSid)"; return }
    if (Test-Path "Registry::HKEY_USERS\$offlineMount") { Fail "$($label): $otherUser's hive is still mounted by the scenario"; return }
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

# --- Writing fixtures ----------------------------------------------------------------------------

# Sets a value through .NET, whose key handles close on Dispose (the registry provider's may stay
# open and keep a mounted hive from unloading).
function Set-RegistryValue([Microsoft.Win32.RegistryKey]$root, [string]$path, [string]$name, $value,
                           [Microsoft.Win32.RegistryValueKind]$kind = 'String') {
    $key = $root.CreateSubKey($path)
    try { $key.SetValue($name, $value, $kind) } finally { $key.Dispose() }
}

function Get-Hklm { [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64') }
function Get-Hklm32 { [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry32') }
function Get-Hkcu { [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', 'Registry64') }

function Write-Utf8([string]$path, [string]$text) {
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    [IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
}

function Write-JsonFile([string]$path, $object) { Write-Utf8 $path ($object | ConvertTo-Json -Depth 20 -Compress) }

# An extension's folder in a Chromium profile, with its manifest (and messages); returns the path
# relative to the profile's Extensions folder, as extensions.settings records it.
function New-ChromiumExtension([string]$profilePath, [string]$id, [string]$version, $manifest, $messages = $null) {
    $relative = "$id\$($version)_0"
    Write-JsonFile "$profilePath\Extensions\$relative\manifest.json" $manifest
    if ($messages) { Write-JsonFile "$profilePath\Extensions\$relative\_locales\en\messages.json" $messages }
    return $relative
}

# One extensions.settings entry.
function New-Setting([int]$location, [string]$relative, [hashtable]$extra = @{}) {
    $setting = [ordered]@{ location = $location; path = $relative; state = 1; install_time = '13340000000000000'
                           was_installed_by_default = $false }
    foreach ($name in $extra.Keys) { $setting[$name] = $extra[$name] }
    return $setting
}

function New-Manifest([string]$name, [string]$version, [int]$manifestVersion = 3, [string]$updateUrl = '',
                      [string[]]$permissions = @(), [string[]]$hosts = @(), [string[]]$optional = @()) {
    $manifest = [ordered]@{ manifest_version = $manifestVersion; name = $name; version = $version
                            description = "$name, a fixture of Avel's Sandbox" }
    if ($updateUrl) { $manifest.update_url = $updateUrl }
    if ($permissions.Count) { $manifest.permissions = $permissions }
    if ($hosts.Count) { $manifest.host_permissions = $hosts }
    if ($optional.Count) { $manifest.optional_permissions = $optional }
    return $manifest
}

function Write-Preferences([string]$path, $settings, [hashtable]$more = @{}) {
    $root = [ordered]@{ browser = [ordered]@{ has_seen_welcome_page = $true } }
    foreach ($name in $more.Keys) { $root[$name] = $more[$name] }
    $root.extensions = [ordered]@{ settings = $settings }
    Write-JsonFile $path $root
}

function Install-ChromeProfiles {
    Write-JsonFile "$chromeData\Local State" ([ordered]@{ profile = [ordered]@{ info_cache = [ordered]@{
        'Default' = [ordered]@{ name = 'Person 1' }; 'Profile 1' = [ordered]@{ name = 'Work' }
        'Profile 2' = [ordered]@{ name = 'Broken' } } } })

    $default = "$chromeData\Default"
    # The store extension: MV2, its name and description from _locales\en (__MSG_), a host pattern
    # among its permissions; disabled in Preferences, enabled in Secure Preferences, which wins.
    $store = New-ChromiumExtension $default $ids.store '1.0.0' ([ordered]@{
        manifest_version = 2; name = '__MSG_extName__'; description = '__MSG_extDesc__'; default_locale = 'en'
        version = '1.0.0'; update_url = $storeUrl; permissions = @('tabs', 'storage', 'https://*.contoso.example/*')
        optional_permissions = @('downloads') }) ([ordered]@{
        extName = [ordered]@{ message = 'Contoso Store Helper' }; extDesc = [ordered]@{ message = 'Helps from the store' } })
    $forced = New-ChromiumExtension $default $ids.forced '2.0' (New-Manifest 'Contoso Forced' '2.0' 3 $storeUrl @('storage') @('<all_urls>'))
    $registered = New-ChromiumExtension $default $ids.registered '1.2.3' (New-Manifest 'Contoso Registered' '1.2.3' 3 '' @('cookies'))
    $keeper = New-ChromiumExtension $default $ids.keeper '3.0' (New-Manifest 'Contoso Keeper' '3.0' 3 $storeUrl @('alarms'))
    $component = New-ChromiumExtension $default $ids.component '1.0' (New-Manifest 'Contoso Component' '1.0')
    Write-JsonFile "$unpackedRoot\manifest.json" (New-Manifest 'Contoso Unpacked' '0.1' 3 '' @('debugger'))
    Write-Preferences "$default\Preferences" ([ordered]@{
        $ids.store = New-Setting 1 $store @{ from_webstore = $true; state = 0 }
        $ids.forced = New-Setting 9 $forced
        $ids.registered = New-Setting 3 $registered
        $ids.unpacked = New-Setting 4 $unpackedRoot
        $ids.component = New-Setting 5 $component
    }) @{ profile = [ordered]@{ name = 'Person 1' } }
    Write-Preferences "$default\Secure Preferences" ([ordered]@{
        $ids.store = [ordered]@{ state = 1 }
        $ids.keeper = New-Setting 1 $keeper @{ from_webstore = $true }
    })

    $work = "$chromeData\Profile 1"
    $forcedWork = New-ChromiumExtension $work $ids.forced '2.0' (New-Manifest 'Contoso Forced' '2.0' 3 $storeUrl @('storage') @('<all_urls>'))
    $settings = New-ChromiumExtension $work $ids.settings '4.0' (New-Manifest 'Contoso Settings' '4.0' 3 $storeUrl @('storage'))
    $disabled = New-ChromiumExtension $work $ids.disabled '5.0' (New-Manifest 'Contoso Paused' '5.0' 3 $storeUrl)
    $theme = New-ChromiumExtension $work $ids.theme '1.0' ([ordered]@{ manifest_version = 3; name = 'Contoso Theme'; version = '1.0'
                                                                       theme = [ordered]@{ colors = [ordered]@{} } })
    Write-Preferences "$work\Preferences" ([ordered]@{
        $ids.forced = New-Setting 9 $forcedWork
        $ids.settings = New-Setting 9 $settings
        $ids.disabled = New-Setting 1 $disabled @{ from_webstore = $true; state = 0; disable_reasons = @(1) }
        $ids.theme = New-Setting 1 $theme @{ from_webstore = $true }
    })
    Write-Utf8 "$chromeData\Profile 2\Preferences" '{"extensions": {"settings": '
}

Add-Type -AssemblyName System.Web.Extensions

# A JSON file as .NET dictionaries (key order kept), for files of a real browser profile that are
# changed in place; an empty dictionary when the file is missing.
function Read-JsonDictionary([string]$path) {
    $serializer = [System.Web.Script.Serialization.JavaScriptSerializer]::new()
    $serializer.MaxJsonLength = [int]::MaxValue
    $serializer.RecursionLimit = 1000
    if (-not (Test-Path -LiteralPath $path)) { return [System.Collections.Generic.Dictionary[string, object]]::new() }
    return $serializer.DeserializeObject([IO.File]::ReadAllText($path))
}

function Write-JsonDictionary([string]$path, $dictionary) {
    $serializer = [System.Web.Script.Serialization.JavaScriptSerializer]::new()
    $serializer.MaxJsonLength = [int]::MaxValue
    $serializer.RecursionLimit = 1000
    Write-Utf8 $path $serializer.Serialize($dictionary)
}

# `parent[name]` as a dictionary, made when missing.
function Get-JsonChild($parent, [string]$name) {
    if (-not $parent.ContainsKey($name) -or $parent[$name] -isnot [System.Collections.Generic.Dictionary[string, object]]) {
        $parent[$name] = [System.Collections.Generic.Dictionary[string, object]]::new()
    }
    return $parent[$name]
}

# The Sandbox's own Edge profile (Edge has run in its image; it isn't running now): the fixtures'
# extensions are added to Default's Preferences, beside whatever Edge keeps there, and Default is
# made sure to be among Local State's profiles. Returns Default's display name, as Avel reads it.
function Install-EdgeProfile {
    Get-Process msedge -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    $edge = "$edgeData\Default"
    $state = Read-JsonDictionary "$edgeData\Local State"
    $cache = $null
    if ($state.ContainsKey('profile') -and $state['profile'] -is [System.Collections.IDictionary] -and
        $state['profile'].ContainsKey('info_cache')) { $cache = $state['profile']['info_cache'] }
    $listed = if ($cache) { @($cache.Keys) -join ', ' } else { 'none' }
    Log "  Edge's own profile: Local State's profiles: $listed; Default\Preferences there: $(Test-Path "$edge\Preferences"); Secure Preferences: $(Test-Path "$edge\Secure Preferences")"
    $name = 'Default'
    if ($cache -and $cache.Count) {
        if (-not $cache.ContainsKey('Default')) {
            $info = [System.Collections.Generic.Dictionary[string, object]]::new()
            $info['name'] = 'Avel Edge'
            $cache['Default'] = $info
            Write-JsonDictionary "$edgeData\Local State" $state
            Log "  Default added to Edge's Local State as 'Avel Edge'"
        }
        if ($cache['Default'] -is [System.Collections.IDictionary] -and "$($cache['Default']['name'])") { $name = "$($cache['Default']['name'])" }
    }
    $forced = New-ChromiumExtension $edge $ids.edgeForced '1.0' (New-Manifest 'Contoso Edge Forced' '1.0' 3 $edgeStoreUrl @('storage'))
    $store = New-ChromiumExtension $edge $ids.edgeStore '2.5' (New-Manifest 'Contoso Edge Add-on' '2.5' 3 $edgeStoreUrl @('contextMenus'))
    $json = New-ChromiumExtension $edge $ids.edgeJson '1.1' (New-Manifest 'Contoso Edge Json' '1.1' 3 $edgeStoreUrl)
    $ours = [ordered]@{
        $ids.edgeForced = New-Setting 9 $forced
        $ids.edgeStore = New-Setting 1 $store @{ from_webstore = $true }
        $ids.edgeJson = New-Setting 9 $json
    }
    # Through JSON text, so the dictionaries hold plain .NET values only.
    $added = [System.Web.Script.Serialization.JavaScriptSerializer]::new().DeserializeObject(($ours | ConvertTo-Json -Depth 20 -Compress))
    $preferences = Read-JsonDictionary "$edge\Preferences"
    $settings = Get-JsonChild (Get-JsonChild $preferences 'extensions') 'settings'
    $already = @($settings.Keys)
    foreach ($id in $added.Keys) { $settings[$id] = $added[$id] }
    Write-JsonDictionary "$edge\Preferences" $preferences
    Log "  Edge's Default profile ('$name'): $($already.Count) extension setting(s) of its own, the fixtures' 3 added"
    return $name
}

function Install-OtherProfiles {
    $script:edgeProfileName = Install-EdgeProfile

    # Brave: no Local State, so its profiles are the folders holding a Preferences file.
    $brave = "$braveData\Default"
    $sub = New-ChromiumExtension $brave $ids.braveSub '1.0' (New-Manifest 'Contoso Brave Subkey' '1.0' 3 $storeUrl)
    $comma = New-ChromiumExtension $brave $ids.braveComma '1.0' (New-Manifest 'Contoso Brave Shared' '1.0' 3 $storeUrl)
    Write-Preferences "$brave\Preferences" ([ordered]@{ $ids.braveSub = New-Setting 9 $sub; $ids.braveComma = New-Setting 9 $comma })

    # Opera: the folder itself is the one profile.
    $opera = New-ChromiumExtension $operaData $ids.opera '7.0' (New-Manifest 'Contoso Opera Add-on' '7.0' 3 $operaStoreUrl)
    Write-Preferences "$operaData\Preferences" ([ordered]@{ $ids.opera = New-Setting 1 $opera @{ from_webstore = $true } })

    # Firefox: profiles.ini and extensions.json (built-in and theme add-ons aren't listed).
    Write-Utf8 "$firefoxData\profiles.ini" ("[General]`r`nStartWithLastProfile=1`r`n`r`n[Profile0]`r`nName=default-release`r`n" +
                                            "IsRelative=1`r`nPath=Profiles/avel1234.default-release`r`nDefault=1`r`n")
    $addon = {
        param($id, $name, $location, $active, $signed, $source, $origin)
        [ordered]@{ id = $id; type = 'extension'; location = $location; version = '1.5'; active = $active
                    userDisabled = -not $active; signedState = $signed; sourceURI = $source; installDate = 1700000000000
                    path = "$firefoxProfile\extensions\$id.xpi"
                    defaultLocale = [ordered]@{ name = $name; description = "$name, a fixture of Avel's Sandbox" }
                    userPermissions = [ordered]@{ permissions = @('storage', 'tabs'); origins = @('https://*.contoso.example/*') }
                    optionalPermissions = [ordered]@{ permissions = @('downloads'); origins = @() }
                    installTelemetryInfo = [ordered]@{ source = $origin } }
    }
    $addons = @(
        (& $addon $firefoxIds.store 'Contoso Fox Store' 'app-profile' $true 2 'https://addons.mozilla.org/firefox/downloads/file/1/store.xpi' 'amo')
        (& $addon $firefoxIds.registered 'Contoso Fox Registered' 'winreg-app-global' $false 0 '' 'unknown')
        (& $addon $firefoxIds.policy 'Contoso Fox Policy' 'app-profile' $true 0 $firefoxUrls.policy 'enterprise-policy')
        (& $addon $firefoxIds.policiesJson 'Contoso Fox Managed' 'app-profile' $true 0 $firefoxUrls.policiesJson 'enterprise-policy')
        (& $addon 'builtin@contoso.example' 'Contoso Built-in' 'app-builtin' $true 0 '' 'unknown')
    )
    $theme = & $addon 'theme@contoso.example' 'Contoso Fox Theme' 'app-profile' $true 2 '' 'amo'
    $theme.type = 'theme'
    Write-JsonFile "$firefoxProfile\extensions.json" ([ordered]@{ schemaVersion = 36; addons = @($addons + $theme) })

    # The Firefox install folder, found as Program Files\Mozilla Firefox (Firefox isn't installed: its
    # program is a stand-in that is never started).
    New-Item -ItemType Directory -Force $firefoxRoot | Out-Null
    Copy-Item (Join-Path (Split-Path $cli) 'avel_test_spawner.exe') "$firefoxRoot\firefox.exe" -Force
    Write-JsonFile $policiesJson ([ordered]@{ policies = [ordered]@{
        DisableTelemetry = $true
        ExtensionSettings = [ordered]@{ $firefoxIds.policiesJson = [ordered]@{ installation_mode = 'force_installed'
                                                                               install_url = $firefoxUrls.policiesJson } } } })

    # The program whose folder holds the registered .crx and .xpi.
    New-Item -ItemType Directory -Force $programRoot | Out-Null
    Write-Utf8 "$programRoot\contoso.crx" 'Cr24 fixture'
    Write-Utf8 "$programRoot\registered.xpi" 'PK fixture'
    $hklm = Get-Hklm
    $uninstall = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ContosoExtensions'
    Set-RegistryValue $hklm $uninstall 'DisplayName' 'Contoso Extensions'
    Set-RegistryValue $hklm $uninstall 'Publisher' 'Contoso Ltd'
    Set-RegistryValue $hklm $uninstall 'DisplayVersion' '1.0'
    Set-RegistryValue $hklm $uninstall 'InstallLocation' $programRoot
    Set-RegistryValue $hklm $uninstall 'UninstallString' "`"$programRoot\uninstall.exe`""
}

function Install-OtherUserProfile {
    Write-JsonFile "$otherChromeData\Local State" ([ordered]@{ profile = [ordered]@{ info_cache = [ordered]@{
        'Default' = [ordered]@{ name = 'Other Person' } } } })
    $default = "$otherChromeData\Default"
    $forced = New-ChromiumExtension $default $ids.forced '2.0' (New-Manifest 'Contoso Forced' '2.0' 3 $storeUrl @('storage') @('<all_urls>'))
    $other = New-ChromiumExtension $default $ids.other '6.0' (New-Manifest 'Contoso Other Registered' '6.0' 3 $storeUrl @('tabs'))
    $store = New-ChromiumExtension $default $ids.otherStore '1.0' (New-Manifest 'Contoso Other Store' '1.0' 3 $storeUrl)
    Write-Preferences "$default\Preferences" ([ordered]@{
        $ids.forced = New-Setting 9 $forced
        $ids.other = New-Setting 3 $other
        $ids.otherStore = New-Setting 1 $store @{ from_webstore = $true }
    })
}

function Install-Sources {
    $hklm = Get-Hklm
    $chrome = 'SOFTWARE\Policies\Google\Chrome'
    Set-RegistryValue $hklm "$chrome\ExtensionInstallForcelist" '1' "$($ids.machineX);$storeUrl"
    Set-RegistryValue $hklm "$chrome\ExtensionInstallForcelist" '2' "$($ids.forced);$storeUrl"
    Set-RegistryValue $hklm "$chrome\ExtensionInstallForcelist" '3' "$($ids.machineY);$storeUrl"
    Set-RegistryValue $hklm $chrome 'ExtensionSettings' $chromeSettingsJson
    Set-RegistryValue $hklm $chrome 'HomepageLocation' 'https://contoso.example/'
    Set-RegistryValue $hklm "$chrome\ExtensionInstallAllowlist" '1' $ids.allowed
    $edge = 'SOFTWARE\Policies\Microsoft\Edge'
    Set-RegistryValue $hklm "$edge\ExtensionInstallForcelist" '1' "$($ids.edgeForced);$edgeStoreUrl"
    Set-RegistryValue $hklm "$edge\ExtensionInstallForcelist" '3' "$($ids.edgeIgnored);$edgeStoreUrl"
    Set-RegistryValue $hklm $edge 'HideFirstRunExperience' 1 DWord
    $brave = 'SOFTWARE\Policies\BraveSoftware\Brave\ExtensionSettings'
    Set-RegistryValue $hklm "$brave\$($ids.braveSub)" 'installation_mode' 'force_installed'
    Set-RegistryValue $hklm "$brave\$($ids.braveSub)" 'update_url' $storeUrl
    Set-RegistryValue $hklm "$brave\$($ids.braveComma),$($ids.bravePartner)" 'installation_mode' 'normal_installed'
    Set-RegistryValue $hklm "$brave\$($ids.braveComma),$($ids.bravePartner)" 'update_url' $storeUrl
    Set-RegistryValue $hklm "$brave\*" 'installation_mode' 'blocked'
    $firefox = 'SOFTWARE\Policies\Mozilla\Firefox'
    Set-RegistryValue $hklm "$firefox\Extensions\Install" '1' $firefoxUrls.other
    Set-RegistryValue $hklm "$firefox\Extensions\Install" '2' $firefoxUrls.policy
    Set-RegistryValue $hklm "$firefox\Extensions\Install" '3' $firefoxUrls.local
    Set-RegistryValue $hklm "$firefox\Extensions\Locked" '1' $firefoxIds.policy
    Set-RegistryValue $hklm $firefox 'DisableAppUpdate' 1 DWord
    Set-RegistryValue $hklm 'SOFTWARE\Mozilla\Firefox\Extensions' $firefoxIds.registered "$programRoot\registered.xpi"
    # Chromium's registrations: HKLM's 32-bit view (what Chromium reads) and its 64-bit view (what it
    # doesn't).
    $registration = "SOFTWARE\Google\Chrome\Extensions\$($ids.registered)"
    Set-RegistryValue (Get-Hklm32) $registration 'path' "$programRoot\contoso.crx"
    Set-RegistryValue (Get-Hklm32) $registration 'version' '1.2.3'
    Set-RegistryValue $hklm "SOFTWARE\Google\Chrome\Extensions\$($ids.machine64)" 'update_url' $storeUrl

    $hkcu = Get-Hkcu
    $userChrome = 'Software\Policies\Google\Chrome'
    Set-RegistryValue $hkcu "$userChrome\ExtensionInstallForcelist" '1' "$($ids.userP);$storeUrl"
    Set-RegistryValue $hkcu "$userChrome\ExtensionInstallForcelist" '2' "$($ids.userQ);$storeUrl"
    Set-RegistryValue $hkcu "$userChrome\ExtensionInstallForcelist" '3' "$($ids.userR);$storeUrl"
    Set-RegistryValue $hkcu $userChrome 'ShowHomeButton' 1 DWord
    Set-RegistryValue $hkcu 'Software\Policies\Microsoft\Edge' 'ExtensionSettings' (
        '{"' + $ids.edgeJson + '": {"installation_mode": "normal_installed", "update_url": "' + $edgeStoreUrl + '"}}')
    Set-RegistryValue $hkcu 'Software\Policies\BraveSoftware\Brave' 'ExtensionSettings' 'this is not JSON'
    Set-RegistryValue $hkcu "Software\Google\Chrome\Extensions\$($ids.keeper)" 'update_url' $storeUrl

    Mount-OtherHive
    try {
        Set-RegistryValue ([Microsoft.Win32.Registry]::Users) "$offlineMount\Software\Google\Chrome\Extensions\$($ids.other)" `
            'update_url' $storeUrl
    } finally {
        Dismount-OtherHive
    }
}

# --- What the list must show ---------------------------------------------------------------------

# A source as the comparison writes it: kind|key (policies.json: its file)|value|ignored|wow64|program.
function Format-ExpectedSource([string]$kind, [string]$key, [string]$value = '', [switch]$Ignored, [switch]$Wow64,
                               [string]$Program = '') {
    return "$kind|$key|$value|$([int][bool]$Ignored)|$([int][bool]$Wow64)|$Program"
}

function Format-ListedSource($source) {
    $where = if ($source.kind -eq 'policies-json') { $source.file } else { $source.key }
    return "$($source.kind)|$where|$($source.value_name)|$([int][bool]$source.ignored_by_browser)|$([int][bool]$source.wow64)|$($source.program_name)"
}

# Every entry the fixtures make, as the list must show it. owner: machine (a machine source's entry
# in no profile), current or other; profile: the folder ($null: in no profile, '' Opera's).
function Get-ExpectedEntries {
    $hku = "HKU\$currentSid"
    $chromeList = 'HKLM\SOFTWARE\Policies\Google\Chrome\ExtensionInstallForcelist'
    $userList = "$hku\Software\Policies\Google\Chrome\ExtensionInstallForcelist"
    $edgeList = 'HKLM\SOFTWARE\Policies\Microsoft\Edge\ExtensionInstallForcelist'
    $braveSettings = 'HKLM\SOFTWARE\Policies\BraveSoftware\Brave\ExtensionSettings'
    $firefoxPolicy = 'HKLM\SOFTWARE\Policies\Mozilla\Firefox\Extensions'
    $forced = Format-ExpectedSource 'force-list' $chromeList '2'
    $settings = Format-ExpectedSource 'settings' 'HKLM\SOFTWARE\Policies\Google\Chrome' 'ExtensionSettings'
    $braveComma = Format-ExpectedSource 'settings' "$braveSettings\$($ids.braveComma),$($ids.bravePartner)"
    $program = 'Contoso Extensions'
    function E([string]$label, [string]$browser, [string]$owner, $folder, [string]$ext, [string]$name,
               [string]$install, [string]$store, [string]$removal, [string[]]$sources = @(), [bool]$enabled = $true,
               [string[]]$permissions = $null, [string[]]$hosts = $null, [string[]]$optional = $null,
               [bool]$keepsCopy = $false) {
        [pscustomobject]@{ label = $label; browser = $browser; owner = $owner; profile = $folder; ext = $ext; name = $name
                           install = $install; store = $store; removal = $removal; sources = @($sources); enabled = $enabled
                           permissions = $permissions; hosts = $hosts; optional = $optional; keepsCopy = $keepsCopy
                           sid = $(switch ($owner) { 'machine' { '' } 'current' { $currentSid } 'other' { $otherSid } })
                           id = $null; entry = $null }
    }
    @(
        E 'chrome-store' 'chrome' 'current' 'Default' $ids.store 'Contoso Store Helper' 'store' 'chrome-web-store' 'not-removable' `
            -permissions @('tabs', 'storage') -hosts @('https://*.contoso.example/*') -optional @('downloads')
        E 'chrome-forced' 'chrome' 'current' 'Default' $ids.forced 'Contoso Forced' 'policy' 'chrome-web-store' 'removable' @($forced) `
            -permissions @('storage') -hosts @('<all_urls>')
        E 'chrome-registered' 'chrome' 'current' 'Default' $ids.registered 'Contoso Registered' 'external-registry' 'none' 'removable' `
            @(Format-ExpectedSource 'external-registry' "HKLM\SOFTWARE\WOW6432Node\Google\Chrome\Extensions\$($ids.registered)" -Wow64 -Program $program)
        E 'chrome-keeper' 'chrome' 'current' 'Default' $ids.keeper 'Contoso Keeper' 'store' 'chrome-web-store' 'removable' `
            @(Format-ExpectedSource 'external-registry' "$hku\Software\Google\Chrome\Extensions\$($ids.keeper)") -keepsCopy $true
        E 'chrome-unpacked' 'chrome' 'current' 'Default' $ids.unpacked 'Contoso Unpacked' 'developer' 'none' 'not-removable'
        E 'work-forced' 'chrome' 'current' 'Profile 1' $ids.forced 'Contoso Forced' 'policy' 'chrome-web-store' 'removable' @($forced)
        E 'work-settings' 'chrome' 'current' 'Profile 1' $ids.settings 'Contoso Settings' 'policy' 'chrome-web-store' 'removable' @($settings)
        E 'work-disabled' 'chrome' 'current' 'Profile 1' $ids.disabled 'Contoso Paused' 'store' 'chrome-web-store' 'not-removable' -enabled $false
        E 'user-p' 'chrome' 'current' $null $ids.userP '' 'policy' 'chrome-web-store' 'removable' @(Format-ExpectedSource 'force-list' $userList '1')
        E 'user-q' 'chrome' 'current' $null $ids.userQ '' 'policy' 'chrome-web-store' 'removable' @(Format-ExpectedSource 'force-list' $userList '2')
        E 'user-r' 'chrome' 'current' $null $ids.userR '' 'policy' 'chrome-web-store' 'removable' @(Format-ExpectedSource 'force-list' $userList '3')
        E 'machine-x' 'chrome' 'machine' $null $ids.machineX '' 'policy' 'chrome-web-store' 'removable' @(Format-ExpectedSource 'force-list' $chromeList '1')
        E 'machine-y' 'chrome' 'machine' $null $ids.machineY '' 'policy' 'chrome-web-store' 'removable' @(Format-ExpectedSource 'force-list' $chromeList '3')
        E 'machine-partner' 'chrome' 'machine' $null $ids.partner '' 'policy' 'chrome-web-store' 'removable' @($settings)
        E 'machine-64' 'chrome' 'machine' $null $ids.machine64 '' 'external-registry' 'chrome-web-store' 'removable' `
            @(Format-ExpectedSource 'external-registry' "HKLM\SOFTWARE\Google\Chrome\Extensions\$($ids.machine64)" -Ignored)
        E 'other-forced' 'chrome' 'other' 'Default' $ids.forced 'Contoso Forced' 'policy' 'chrome-web-store' 'removable' @($forced)
        E 'other-registered' 'chrome' 'other' 'Default' $ids.other 'Contoso Other Registered' 'external-registry' 'chrome-web-store' 'removable' `
            @(Format-ExpectedSource 'external-registry' "HKU\$otherSid\Software\Google\Chrome\Extensions\$($ids.other)")
        E 'other-store' 'chrome' 'other' 'Default' $ids.otherStore 'Contoso Other Store' 'store' 'chrome-web-store' 'not-removable'
        E 'edge-forced' 'edge' 'current' 'Default' $ids.edgeForced 'Contoso Edge Forced' 'policy' 'edge-add-ons' 'removable' `
            @(Format-ExpectedSource 'force-list' $edgeList '1')
        E 'edge-store' 'edge' 'current' 'Default' $ids.edgeStore 'Contoso Edge Add-on' 'store' 'edge-add-ons' 'not-removable'
        E 'edge-json' 'edge' 'current' 'Default' $ids.edgeJson 'Contoso Edge Json' 'policy' 'edge-add-ons' 'removable' `
            @(Format-ExpectedSource 'settings' "$hku\Software\Policies\Microsoft\Edge" 'ExtensionSettings')
        E 'edge-ignored' 'edge' 'machine' $null $ids.edgeIgnored '' 'policy' 'edge-add-ons' 'removable' `
            @(Format-ExpectedSource 'force-list' $edgeList '3' -Ignored)
        E 'brave-sub' 'brave' 'current' 'Default' $ids.braveSub 'Contoso Brave Subkey' 'policy' 'chrome-web-store' 'removable' `
            @(Format-ExpectedSource 'settings' "$braveSettings\$($ids.braveSub)")
        E 'brave-comma' 'brave' 'current' 'Default' $ids.braveComma 'Contoso Brave Shared' 'policy' 'chrome-web-store' 'not-supported' @($braveComma)
        E 'brave-partner' 'brave' 'machine' $null $ids.bravePartner '' 'policy' 'chrome-web-store' 'not-supported' @($braveComma)
        E 'opera-store' 'opera' 'current' '' $ids.opera 'Contoso Opera Add-on' 'store' 'opera-add-ons' 'not-removable'
        E 'firefox-store' 'firefox' 'current' 'avel1234.default-release' $firefoxIds.store 'Contoso Fox Store' 'store' 'mozilla' 'not-removable' `
            -permissions @('storage', 'tabs') -hosts @('https://*.contoso.example/*') -optional @('downloads')
        E 'firefox-registered' 'firefox' 'current' 'avel1234.default-release' $firefoxIds.registered 'Contoso Fox Registered' 'external-registry' 'none' 'removable' `
            @(Format-ExpectedSource 'firefox-registry' 'HKLM\SOFTWARE\Mozilla\Firefox\Extensions' $firefoxIds.registered -Program $program) -enabled $false
        E 'firefox-policy' 'firefox' 'current' 'avel1234.default-release' $firefoxIds.policy 'Contoso Fox Policy' 'policy' 'none' 'removable' `
            @((Format-ExpectedSource 'firefox-install' "$firefoxPolicy\Install" '2'), (Format-ExpectedSource 'firefox-locked' "$firefoxPolicy\Locked" '1'))
        E 'firefox-managed' 'firefox' 'current' 'avel1234.default-release' $firefoxIds.policiesJson 'Contoso Fox Managed' 'policy' 'none' 'managed-by-policies-json' `
            @(Format-ExpectedSource 'policies-json' $policiesJson)
        E 'firefox-url' 'firefox' 'machine' $null '' $firefoxUrls.other 'policy' 'none' 'removable' @(Format-ExpectedSource 'firefox-install' "$firefoxPolicy\Install" '1')
        E 'firefox-path' 'firefox' 'machine' $null '' $firefoxUrls.local 'policy' 'none' 'removable' @(Format-ExpectedSource 'firefox-install' "$firefoxPolicy\Install" '3')
    )
}

# The profiles the list must show: browser|owner|folder|name.
function Get-ExpectedProfiles {
    @(
        "chrome|$currentSid|Default|Person 1", "chrome|$currentSid|Profile 1|Work", "chrome|$currentSid|Profile 2|Broken"
        "chrome|$otherSid|Default|Other Person", "edge|$currentSid|Default|$edgeProfileName", "brave|$currentSid|Default|Default"
        "opera|$currentSid||", "firefox|$currentSid|avel1234.default-release|default-release"
    )
}

# --- State dumps ---------------------------------------------------------------------------------

# The keys every round compares (whole trees: the fixtures' values and the unrelated ones beside
# them) and the profiles' files, which Avel must never write.
function Get-RegistrySpecs {
    [ordered]@{
        hklmChrome = 'key:HKLM\SOFTWARE\Policies\Google\Chrome'
        hklmEdge = 'key:HKLM\SOFTWARE\Policies\Microsoft\Edge'
        hklmBrave = 'key:HKLM\SOFTWARE\Policies\BraveSoftware\Brave'
        hklmFirefox = 'key:HKLM\SOFTWARE\Policies\Mozilla\Firefox'
        hklmReg32 = 'key:HKLM\SOFTWARE\WOW6432Node\Google\Chrome\Extensions'
        hklmReg64 = 'key:HKLM\SOFTWARE\Google\Chrome\Extensions'
        hklmFirefoxReg = 'key:HKLM\SOFTWARE\Mozilla\Firefox\Extensions'
        hkcuChrome = 'key:HKCU\Software\Policies\Google\Chrome'
        hkcuEdge = 'key:HKCU\Software\Policies\Microsoft\Edge'
        hkcuBrave = 'key:HKCU\Software\Policies\BraveSoftware\Brave'
        hkcuReg = 'key:HKCU\Software\Google\Chrome\Extensions'
        otherReg = "key:HKU\$offlineMount\Software\Google\Chrome\Extensions"
    }
}

function Get-FileSpecs {
    # Edge's: only its profile's records and the fixtures' extensions (its caches aren't Avel's
    # business). Every name starting with "edge" is Edge's own profile.
    [ordered]@{
        chrome = "file:$chromeData"; brave = "file:$braveData"; opera = "file:$operaData"; firefox = "file:$firefoxData"
        distribution = "file:$firefoxRoot\distribution"; unpacked = "file:$unpackedRoot"; program = "file:$programRoot"
        other = "file:$otherChromeData"; edgeState = "file:$edgeData\Local State"
        edgePreferences = "file:$edgeData\Default\Preferences"; edgeSecure = "file:$edgeData\Default\Secure Preferences"
        edgeForced = "file:$edgeData\Default\Extensions\$($ids.edgeForced)"
        edgeStore = "file:$edgeData\Default\Extensions\$($ids.edgeStore)"
        edgeJson = "file:$edgeData\Default\Extensions\$($ids.edgeJson)"
    }
}

# avel_state_dump of `specs`, split into sections; mounts the signed-out user's hive while it runs.
function Get-ExtensionsDump([string[]]$specs) {
    $mount = @($specs | Where-Object { $_ -match "HKU\\$offlineMount\\" }).Count -gt 0
    if ($mount) { Mount-OtherHive }
    try {
        return Split-StateDump (Get-StateDump $specs)
    } finally {
        if ($mount) { Dismount-OtherHive }
    }
}

# A registry name or data from avel_state_dump's hex (UTF-16LE).
function ConvertFrom-HexText([string]$hex) {
    if (-not $hex) { return '' }
    $bytes = [byte[]]::new($hex.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) { $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16) }
    return [Text.Encoding]::Unicode.GetString($bytes)
}

# A key spec's dump as a flat map: "K <path>" (class and security) and "V <path>|<name>"
# ("type=N data=<hex>"), the path starting with the spec's own key name.
function ConvertTo-RegistryMap([string]$section) {
    $map = [ordered]@{}
    $stack = [System.Collections.Generic.List[string]]::new()
    foreach ($line in "$section" -split "`n") {
        if ($line -match '^( *)key ([0-9a-fA-F]*) (.*)$') {
            $depth = [int]($Matches[1].Length / 2)
            while ($stack.Count -gt $depth) { $stack.RemoveAt($stack.Count - 1) }
            $stack.Add((ConvertFrom-HexText $Matches[2]))
            $map["K $($stack -join '\')"] = $Matches[3]
        } elseif ($line -match '^ *value ([0-9a-fA-F]*) (type=\d+ data=[0-9a-fA-F]*)\s*$') {
            $map["V $($stack -join '\')|$(ConvertFrom-HexText $Matches[1])"] = $Matches[2].ToLowerInvariant()
        }
    }
    return $map
}

# "type=1 data=<hex>" of a REG_SZ holding `text`, with its terminator.
function Get-StringData([string]$text) {
    $bytes = [Text.Encoding]::Unicode.GetBytes($text + [char]0)
    return 'type=1 data=' + (-join ($bytes | ForEach-Object { $_.ToString('x2') }))
}

# A value's data for the log: a string's text, else as dumped.
function Show-RegistryData([string]$data) {
    if ($data -match '^type=(1|2) data=([0-9a-f]*)$') { return "'$((ConvertFrom-HexText $Matches[2]).TrimEnd([char]0))'" }
    return $data
}

# The map `before` with `changes` applied: copy (to = from, both entries of `before`), set (entry =
# data), delete (entries), deleteKey (a key path and everything below it).
function Get-ExpectedMap($before, $changes) {
    $map = [ordered]@{}
    foreach ($key in $before.Keys) { $map[$key] = $before[$key] }
    if ($changes.copy) { foreach ($to in $changes.copy.Keys) { $map[$to] = $before[$changes.copy[$to]] } }
    if ($changes.set) { foreach ($entry in $changes.set.Keys) { $map[$entry] = $changes.set[$entry] } }
    foreach ($entry in @($changes.delete)) { if ($entry) { $map.Remove($entry) } }
    foreach ($path in @($changes.deleteKey)) {
        if (-not $path) { continue }
        foreach ($key in @($map.Keys)) {
            if ($key -eq "K $path" -or $key.StartsWith("K $path\") -or $key.StartsWith("V $path|") -or $key.StartsWith("V $path\")) {
                $map.Remove($key)
            }
        }
    }
    return $map
}

# Compares two registry maps entry by entry; returns the differences as text.
function Compare-RegistryMap($expected, $actual) {
    $problems = [System.Collections.Generic.List[string]]::new()
    foreach ($key in $expected.Keys) {
        if (-not $actual.Contains($key)) { $problems.Add("missing: $key = $(Show-RegistryData $expected[$key])") }
        elseif ($actual[$key] -ne $expected[$key]) {
            $problems.Add("$key is $(Show-RegistryData $actual[$key]), not $(Show-RegistryData $expected[$key])")
        }
    }
    foreach ($key in $actual.Keys) {
        if (-not $expected.Contains($key)) { $problems.Add("unexpected: $key = $(Show-RegistryData $actual[$key])") }
    }
    return $problems.ToArray()
}

function Install-ExtensionFixtures {
    Install-ChromeProfiles
    Install-OtherProfiles
    Install-OtherUserProfile
    Install-Sources
}
