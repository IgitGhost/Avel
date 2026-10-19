# Competitive analysis: Windows uninstallers (October 2026)

How existing tools work and which techniques Avel should replicate.
BCU's internals were confirmed by reading its source. The commercial tools are described from
vendor pages, release notes and reviews. Items marked *(suggestion)* are our own ideas, not
something a tool is known to do.

## Comparison

| Tool (version) | License / price | Standout techniques | Known weaknesses |
|---|---|---|---|
| **Revo Uninstaller Pro 5.5.4** | $24.95 per year of updates (lifetime use); Portable $29.95 | Real-time installation monitor that writes "trace logs"; Traced Programs with custom uninstall; vendor-made logs database; Forced Uninstall; Hunter Mode (crosshair on a window or tray icon); Windows apps removal for all users; browser extensions; multi-level backups (restore point, registry backup, Recycle Bin, Backup Manager); one restore point per batch; Safe/Moderate/Advanced scans | Advanced mode is slow and gives false positives; monitoring is started by hand for each install; the driver mechanism is undocumented |
| **Bulk Crap Uninstaller 6.3** (Sep 2026) | Free, Apache-2.0, .NET 8, Windows 10+ | Batch quiet uninstall; orphan detection; confidence-rated leftovers; Steam, Store, Windows features, Windows Updates, Chocolatey, Scoop, Oculus sources; console version with JSON output and dry run; certificate column | No install tracing; heuristic scoring needs user review; dense UI; doesn't read other users' registry hives |
| **IObit Uninstaller 16** | Free; Pro about $19.99/yr | Install monitor; database of 4,000+ stubborn programs; bundleware detection; browser plugins; restore point | Bundled offers in its installer; background services; upselling |
| **Geek Uninstaller 1.5.4** | Free portable EXE (Pro = CrystalIDEA Uninstall Tool) | Force removal; Store apps; ARM64. Uninstall Tool adds a kernel-driver install tracker that follows the installer's process tree | Free version has no tracing or batch removal |
| **HiBit Uninstaller 4.0** | Freeware | Forced and batch uninstall; Store apps; install monitor; browser extensions; startup, junk and registry tools | Single developer; registry cleaner adds risk |
| **Wise Program Uninstaller 3.2.9** | Freeware | Forced uninstall; repairs or removes broken Add/Remove Programs entries; Explorer context menu | Basic; no tracing |
| **Total Uninstall 7.6** | €24.95–€99.95 | Before/after snapshot monitoring; "Analyzed" mode for apps installed before tracking; background agent that notices installers starting; program backup and restore | Snapshots are noisy (other software's changes) and slow |
| **Ashampoo UnInstaller 16** | ~€40 | Install logging; snapshot comparison; analysis of apps installed before tracking | Online activation that is re-checked |
| **Settings > Apps / winget** | Built in | `--silent`, `--force`, `--product-code`, `--all-versions`, `--scope` | No leftover scanning; duplicate or mismatched entries |

Sources:
[Revo](https://www.revouninstaller.com/products/revo-uninstaller-pro/),
[BCU releases](https://github.com/Klocman/Bulk-Crap-Uninstaller/releases),
[IObit](https://www.iobit.com/en/advanceduninstaller.php),
[Geek](https://geekuninstaller.com/),
[HiBit](https://www.hibitsoft.ir/Uninstaller.html),
[Total Uninstall](https://www.neowin.net/amp/total-uninstall-762367/),
[Ashampoo](https://betanews.com/article/ashampoo-uninstaller-16),
[winget uninstall](https://learn.microsoft.com/en-us/windows/package-manager/winget/uninstall).

## BCU source layout (reference implementation)

The repo now lives under the `BCUninstaller` organization. Under `source/`:
- `UninstallTools`: the core.
- `BCU-console`.
- Helpers: `StoreAppHelper`, `SteamHelper`, `WinUpdateHelper`, `OculusHelper`, `ScriptHelper`.
- `UninstallerAutomatizer`: UI automation that clicks through uninstallers without a silent mode.

Inside `UninstallTools`:
- `Factory/`: one class per source of installed apps (Registry, StoreApp, Steam, Chocolatey, Scoop, WindowsFeature, WindowsUpdate, Directory, Predefined, Script, Oculus).
- `Factory/InfoAdders/`: about 25 classes that fill in missing details (quiet uninstall string, MSI info, icon, certificate, size, bitness).
- `Junk/Finders/{Drive,Registry,Misc}`, `Junk/Confidence/`, `Startup/{Normal,Service,Task,Browser}`.

## Techniques to replicate

### Enumerating installed programs
- **Uninstall registry keys:** `HKLM` and `HKCU` `...\CurrentVersion\Uninstall`, plus the `WOW6432Node` copies.
  - Read: `DisplayName`, `UninstallString`, `QuietUninstallString`, `InstallLocation`, `SystemComponent`, `WindowsInstaller`, `ParentKeyName`, `ReleaseType`, `BundleProviderKey`, `EstimatedSize`.
  - Treat an entry as an update if `ParentKeyName` is set, `ReleaseType` contains Update or Hotfix, or the key name is `KB####`.
  - Inno Setup is detected from an `Inno Setup:` value; Steam from a `Steam App N` key name.
  - Get the MSI GUID from `BundleProviderKey`, then the key name, then the uninstall string.
  - **Gap in BCU:** other users' hives. Avel reads `HKU\<SID>`, and loads `NTUSER.DAT` for logged-off users.
- **MSI:** `MsiEnumProductsEx(MSIINSTALLCONTEXT_ALL)` catches products hidden from Add/Remove Programs. The component APIs give file lists.
- **AppX/MSIX:** `PackageManager.FindPackagesForUserWithPackageTypes(sid, Main)`.
  - Skip packages whose status is Disabled or NotAvailable.
  - `SignatureKind::System` means protected.
  - Remove with `RemovePackageAsync`; for all users, add RemoveForAllUsers and deprovisioning.
- **Windows features:** use DismAPI (`DismGetFeatures` / `DismDisableFeature`), and include capabilities.
- **Steam** libraries; **Epic** manifests in `ProgramData\Epic\EpicGamesLauncher\Data\Manifests\*.item` *(suggestion)*; **GOG** registry keys *(suggestion)*.
- **Orphans:** folders in Program Files with no registration.
- **Merging duplicates** (BCU `AreEntriesRelated`):
  - Same `InstallLocation` or same `UninstallString` is an immediate match (score 100).
  - Otherwise a weighted sum: DisplayName +8/−5, Publisher ±5, Version +5/−2, InstallDate within 1h +5, bitness +3/−5.
  - Strings count as equal when the Sift4 distance is under length/6, with a minimum length of 5.
- **Quiet uninstall commands:**
  - Inno: `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART`
  - MSI: `msiexec /x {GUID} /qn`
  - NSIS: `/S`

### Leftover detection and avoiding false positives (BCU's confidence model)
**Where BCU looks:**
- **Disk:**
  - Folders: Start Menu (user and common), AppData, LocalAppData, LocalLow, ProgramData, VirtualStore, Program Files.
  - Other: shortcuts, Prefetch, WER reports.
- **Registry:**
  - `SOFTWARE` subkeys (HKLM, HKCU, Wow6432Node, VirtualStore).
  - COM: CLSID `InprocServer32`, TypeLib, ProgID, Interface, ShellEx, PersistentHandler.
  - Firewall rules (the `|App=` field).
  - AppCompatFlags, UserAssist, `Microsoft\Tracing`, EventLog, Installer\Folders, RegisteredApplications.
  - Startup entries: Run keys, services, tasks, browser entries.

**Scoring:** each candidate collects signed records. A sum below 0 is **Bad**, 0–1 **Questionable**, 2–4 **Good**, 5 or more **VeryGood**.

| Record | Δ | Record | Δ |
|---|---|---|---|
| Is the uninstaller registry key | +20 | Another app's install folder is inside | −7 |
| Explicit connection (a value points into the install folder) | +4 | Is a Store app | −10 |
| Company name match / all subfolders matched / empty folder | +4 | Executables present / name or publisher still used by another app | −4 |
| Perfect product-name match / no subfolders | +2 | Generic folder name (`install`, `settings`, `config`, `data`…) | −3 |
| Depth: `2 − 2×level` | +2…−2 | Dodgy match / item name equals company / used by a similarly named app / >100 files | −2 |

**Safeguards:**
- Never match names of 4 characters or fewer.
- A candidate needs at least one positive record.
- Recursion stops at depth 2.
- Folder blacklist: `Microsoft`, `Common Files`, `WindowsApps`, `Installer`, `winsxs`, and others.
- Never scan system-attribute folders, the Windows directory, known folders or Downloads.
- A final filter drops anything that points into a prohibited location.
- Wow and non-Wow registry views are mirrored.
- Duplicates are merged by combining their records.

**Avel extensions:**
- Apply "explicit connection" to service `ImagePath`, scheduled-task `<Exec>`, App Paths, file associations and context-menu handlers.
- Back up in layers the way Revo does: one restore point per batch, registry export before deleting, files to quarantine.

### Install monitoring
- **Snapshot diff** (Total Uninstall, Ashampoo): simple, but slow and noisy.
- **Real-time** (Uninstall Tool, probably Revo): a kernel minifilter, `CmRegisterCallbackEx` for the registry and process-creation callbacks to follow the installer's process tree. It needs driver signing (attestation, or test-signing mode).
- **Pitfall:** msiexec does the actual work in its SYSTEM service process, which is not a child of setup.exe. Services and COM servers the installer starts behave the same way. Process-tree tracing misses all of these.
- **Avel's choice:** see [ADR 0002](../adr/0002-install-tracker-without-driver.md).
