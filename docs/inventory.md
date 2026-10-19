# Inventory: how Avel finds installed programs

The inventory engine lives in `src/core/inventory` and has no Qt dependency. `scan_inventory()`
runs three sources in parallel, merges their results and returns one list of `Program` records
plus diagnostics. Both `avel-cli list` and the GUI program table show that list.

## Sources

| Source | Where it looks | Notes |
|---|---|---|
| **Registry** (`registry_source.cpp`) | `Uninstall` keys in HKLM (64-bit and 32-bit views) and in every user profile from `ProfileList`, also both views | Hives of signed-out users are opened read-only from their `NTUSER.DAT` with `RegLoadAppKeyW`. `%USERPROFILE%`-style paths are expanded for the owning user, not for whoever runs Avel. |
| **Windows Installer** (`msi_source.cpp`) | `MsiEnumProductsExW` for all users and contexts | Enumerating other users' products needs administrator rights. Without them Avel falls back to the current user and reports the `msi-all-users-denied` diagnostic. |
| **Packaged apps** (`appx_source.cpp`) | `PackageManager.FindPackagesForUserWithPackageTypes("", Main)` | Current user only. Runs on its own MTA thread (the GUI thread is an STA); packages are inspected on up to 8 threads. |

Field parsing for Uninstall keys (`uninstall_entry.cpp`) follows Bulk Crap Uninstaller's
`RegistryFactory`. That includes hidden `UninstallString_*` values, `YYYYDDMM` dates, update
detection from `ParentKeyName`, `ReleaseType` and KB numbers, and the installer type detected from
the uninstall command (MSI, Inno Setup, InstallShield, WiX Burn, Squirrel, PowerShell, SdbInst, Steam).

## Merging

Registry entries are the base list. Each Windows Installer product is merged into a registry entry,
either the one named after its product code or, failing that, the most related entry by Bulk Crap
Uninstaller's relatedness score (`related_entries.cpp`, threshold 1). A merge only fills empty
fields, and never replaces an existing uninstall command. Products that match nothing are added as
hidden entries (`source: msi`). Packaged apps are never merged: Settings lists them separately as
well.

## Matching Settings > Apps

`is_listed_in_settings()` decides whether Settings > Apps > Installed apps would show an entry.
The GUI's default "Apps" category and `avel-cli list` without `--all` show exactly those entries.
Settings' rules, as reproduced here:

- **Hidden entries:**
  - entries without a display name;
  - `SystemComponent=1`;
  - updates;
  - framework packages;
  - products registered only with Windows Installer;
  - Windows Installer entries (`WindowsInstaller=1`) whose product Windows Installer doesn't know for this computer or the current user. These are typically per-user installs of another account, or leftovers.
- **Names:**
  - A resolvable `DisplayName_Localized` (an `@file,-id` resource reference) wins over `DisplayName`.
  - Packaged apps with a single application use that application's display name from the manifest. Otherwise they use the package's display name. `ms-resource:` strings are resolved through the package's resources.
- **Install dates:**
  - `InstallDate` can be `YYYYMMDD`, `YYYYDDMM`, `YYYY-MM-DD` or the asctime form (`Sat Oct 17 22:38:01 2026`).
  - Without an `InstallDate` value, the date the Uninstall key was last written is used.
  - An `InstallDate` that doesn't parse means no date.
  - Packaged apps use their install date. All dates are local calendar dates.

### Verification

`scripts\Compare-SettingsApps.ps1` reads the Settings list through UI Automation and compares
every line ("Name, version, publisher, date") with `avel-cli list --json`. It is read-only, and
should be run from a non-elevated shell as the user whose apps Settings shows.

Result on the development machine (Windows 11 build 26300, 2026-10-18): **all 172 Settings
entries are identical** in Avel: name, version, publisher and date.

### Explained differences

Avel lists **28 packaged apps that Settings hides**. All of them are Microsoft or OEM packages:

| Group | Packages |
|---|---|
| Media codecs and extensions | AV1, AVC Encoder, HEIF, HEVC, MPEG-2, Raw Image, VP9, WebP |
| Store and system tools | App Installer, Microsoft Store, Store Experience Host, Windows Security, Get Help, Windows Advanced Settings, Windows Application Compatibility Enhancements, Windows Sandbox, Windows Subsystem for Linux, Windows Web Experience Pack, WidgetsPlatformRuntime |
| Xbox | Game Bar, Xbox Game bar, Game Speech Window, Xbox Identity Provider |
| Phone | Phone Link, Mobile devices |
| Packages of Win32 apps (sparse or stub packages) | Microsoft Edge, OneDrive, Armoury Crate (`ASUSAmbientHAL64`) |

Settings applies an internal policy to these packages, and no public API exposes it. Properties
checked without finding a rule that separates them from the packages Settings does show:
- `NonRemovable`;
- `SignatureKind`;
- `IsStub`;
- `IsDevelopmentMode`;
- sparse packages with an external location;
- whether a Win32 entry owns the package's folder.

Copilot, for example, is a sparse package inside a Win32 app's folder, and Settings shows it.

Avel deliberately keeps these packages in its "Apps" list: most can be removed for the current
user, and removing preinstalled packages is one of Avel's goals. The Windows apps tool (Phase 6)
will add a protected list for the packages that must not be removed, such as Windows Security,
App Installer and Microsoft Store.

### Known limitations

- Packaged apps of other users are not listed yet: that needs all-users enumeration with
  administrator rights. Planned with the Windows apps tool (Phase 6).
- Without administrator rights, Windows Installer products of other users are not merged, and the
  diagnostic says so. `avel.exe` always runs elevated.

## Performance

| Step | Time (development machine) |
|---|---|
| Registry (466 keys across 4 views and 2 profiles) | ~48 ms |
| Windows Installer (362 products) | ~95–105 ms |
| Packaged apps (126 packages) | ~90–105 ms |
| **Whole scan** (sources in parallel, plus merging and sorting) | **~100–120 ms** (220 ms on the first run after boot) |
| GUI: process start → window visible | ~0.4 s |
| GUI: process start → list shown | 0.42–0.87 s |

The plan's startup cache is therefore not needed. See
[ADR 0003](adr/0003-no-inventory-cache.md).

Icons load after the list appears, on background threads. A DisplayIcon `path,index` is
extracted with `SHDefExtractIconW`; packaged apps use their scale-qualified logo images.
