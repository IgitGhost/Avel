# Avel (Ավել): plan and roadmap

## Context
The goal is a professional-grade software removal suite for Windows, at the level of Revo Uninstaller Pro or BCUninstaller. It started as a personal tool and is developed in the open.

**In scope:**
1. A core uninstaller with leftover cleanup.
2. An install tracker (snapshot-diff, no kernel driver).
3. Forced removal of programs whose uninstaller is broken or missing.
4. Extra cleanup tools.

**Name:** Avel (Ավել) means "broom" and also "excess". The executables are `avel.exe` (GUI) and `avel-cli.exe`, and the C++ namespace is `avel`. Modules: Avel Uninstall, Avel Trace, Avel Force, Avel Tools.

## Research summary (Oct 2026)

| Tool | What it does best | Gap Avel can fill |
|---|---|---|
| Revo Pro 5.5 ($25/yr) | Real-time install traces, Hunter mode, multi-level backups, Safe/Moderate/Advanced scans | Advanced mode is slow and gives false positives; tracing must be started by hand; closed |
| BCUninstaller 6.3 (free, Apache-2.0, .NET 8) | Best documented leftover scoring, many sources (Steam, Store, features, Chocolatey/Scoop), batch quiet removal, console with JSON | No install tracing; dense UI; ignores other users' registry hives |
| Total Uninstall / Ashampoo | Before/after snapshot tracking, analysis of already-installed apps | Snapshots are slow and noisy |
| Geek / Wise / HiBit / IObit | Simple forced removal; IObit has a stubborn-app database | Shallow, or bundleware and upsell |
| Settings / winget | Built in, `--silent`/`--force` | No leftover scanning |

**Reference implementation:** BCU source at `github.com/BCUninstaller/Bulk-Crap-Uninstaller`:
- `source/UninstallTools/Factory/` (app sources and their info adders)
- `source/UninstallTools/Junk/` (finders and confidence scoring)

BCU is also Apache-2.0, so we can port its merge and confidence logic to C++ as long as we credit it in `NOTICE`.

**How Avel will stand out:**
- Scans every user's hive, not just the current one.
- A fast tracker built on the USN journal plus registry `LastWriteTime`, with no driver. It also catches changes msiexec makes from its SYSTEM service, which process-tree tracing misses.
- One undo system covering every action.
- Each leftover shows why it matched.
- A CLI with JSON output.
- UI in three languages: English, Russian and Armenian (Հայերեն).

## Localization (EN / RU / HY), built in from Phase 1
- **Qt Linguist workflow:** every GUI string goes through `tr()` from the first widget onward, never added later. `qt_add_translations()` in CMake builds `avel_en.ts`, `avel_ru.ts` and `avel_hy.ts` into `.qm` files that are embedded as resources. English is the source language.
- **The engine returns codes, not text.** `avel_core` stays Qt-free and has no user-facing strings: it returns enums plus parameters (e.g. a leftover reason `ExplicitPathReference{path}`, or error codes). The GUI turns those into translated text, so translators only ever touch the app layer.
- **The CLI stays English-only.** Its output is meant for scripting and JSON.
- **Language switcher:** a setting with "System default" as the first option, which uses `QLocale::system()`. Switching at runtime reinstalls the `QTranslator` and calls `retranslateUi()`, so no restart is needed.
- **Qt's own dialog strings:** Qt ships `qtbase_ru.qm` but has no Armenian translation. Use Qt's `ru` file, and add a small `qtbase_hy.ts` that covers the standard buttons and dialogs we actually use.
- **Armenian specifics:**
  - Fonts: Segoe UI covers Armenian, with Sylfaen as the fallback. Check that the windows11 style renders it in tables and headers.
  - Use `QLocale(QLocale::Armenian, QLocale::Armenia)` for dates, numbers and sizes.
  - Leave room in layouts for longer Russian and Armenian strings: no fixed-width buttons.

## Stack decision: C++20 + Qt 6.12 Widgets (MSVC, CMake, vcpkg)
**Why Qt is a good fit here:**
- **Fastest-running option.** Native code, cold start around 150–300 ms, low RAM. Most of the speed comes from the design, though (see "Performance design").
- **Direct Win32 access with no P/Invoke layer:** registry, `msi.dll`, Restart Manager, Task Scheduler COM, SRSetRestorePoint, USN journal, DismAPI, and C++/WinRT for AppX.
- **Qt Widgets, not QML.** `QTableView`/`QTreeView` with custom models and `QSortFilterProxyModel` are built for data-dense desktop apps.
- **Qt stays out of the engine.** `avel_core` is pure C++20 + Win32, so it can be unit-tested, the CLI stays tiny, and the UI could be replaced.

**Qt facts:**
- **Version:** Qt 6.12 is LTS (2026-09-30). The `windows11` style is the default on Windows 11 with dark mode and high contrast, but it isn't pixel-perfect Fluent; Fusion is the fallback. Per-Monitor DPI v2 is on by default.
- **New in 6.12:** `QT_WINDOWS_APP_PROJECT_EXECUTION_LEVEL requireAdministrator` sets up elevation. (Its `static_compiler_runtime` / Hybrid CRT option only matters when building Qt from source; the prebuilt kit uses the DLL runtime, `/MD`, so Avel does too.)
- **License:** qtbase is LGPLv3. Personal use with no distribution has no obligations, and static linking is fine. Avoid the GPL-only modules (Charts, Graphs).
- **Interop rules from day 1:**
  - The GUI thread is an STA (single-threaded COM apartment). Run C++/WinRT and WMI on MTA worker threads, and never call `.get()` on the GUI thread.
  - Compile with `QT_NO_KEYWORDS`.
  - Use W APIs only.
  - An elevated window can't receive drag-and-drop from Explorer. Use a crosshair "target" tool and file pickers instead.
- **Rejected:**
  - C#/.NET: faster to develop, but slower cold start and P/Invoke everywhere.
  - Rust: weak data grids.
  - Raw Win32: too slow to develop.

**Dependencies (vcpkg manifest):**
- `wil` (RAII for HKEY/HANDLE/COM)
- `sqlite3`
- `gtest`
- `nlohmann-json`
- `spdlog`

C++/WinRT comes from the Windows SDK. Qt comes from the online installer or `aqtinstall` (msvc2022_64 build).

## Open source on GitHub (Apache-2.0)
- **Avel's license:** Apache-2.0. Qt's free edition is fine because we only use qtbase (LGPLv3) and no GPL-only modules.
- **Repo files:**
  - `LICENSE` (Apache-2.0).
  - `NOTICE`, crediting BCUninstaller (Apache-2.0, for the ported scoring and merge logic).
  - `THIRD_PARTY_NOTICES.md`, covering Qt (LGPLv3, with a link to its source), WIL (MIT), SQLite (public domain), nlohmann-json (MIT), spdlog (MIT) and GoogleTest (BSD-3).
  - `README.md` with build instructions and a safety disclaimer.
- **Release binaries:** link Qt dynamically using `windeployqt`, and put the LGPL text in the zip. That satisfies the "user can swap Qt" requirement with no extra work.
- **CI:** a GitHub Actions workflow on `windows-latest`. It installs Qt 6.12 with `jurplel/install-qt-action` (aqtinstall), restores vcpkg from cache, builds with the release preset and runs `ctest` (unit tests only; destructive tests stay in the Sandbox).
- **Publishing:** set up the repo locally first. Creating the public GitHub repo and the first push happen only after you confirm, at the end of Phase 0.

## Architecture
**Process model:** a single elevated process, using a `requireAdministrator` manifest. Avel enumerates all user profiles from `ProfileList`, reading each user's registry under `HKU\<SID>` if their hive is loaded, or loading their `NTUSER.DAT` otherwise. That way per-user installs are found even when someone elevates as a different account.

```
D:\Avel\
├─ CMakeLists.txt  CMakePresets.json  vcpkg.json  .clang-format  .gitignore
├─ LICENSE  NOTICE  THIRD_PARTY_NOTICES.md  README.md
├─ .github/workflows/ci.yml
├─ src/core/            avel_core (static lib, NO Qt)
│  ├─ platform/         registry wrapper, paths (env expand, 8.3→long, WOW views), user hives, privileges, COM apartments
│  ├─ inventory/        sources: Registry, Msi, Appx, Steam, Epic (later: winget, features); Merger; InfoAdders
│  ├─ uninstall/        UninstallRunner (job objects), Msi/Appx uninstallers, BatchQueue, result verification
│  ├─ leftovers/        scanners + ConfidenceEngine + ProtectedPolicy
│  ├─ changes/          ChangeSet, RegistryBackup, Quarantine, RestorePoint, UndoJournal, RebootOps
│  ├─ tracker/          RegistrySnapshot, UsnJournalReader, TraceSession, NoiseFilter, TraceStore
│  ├─ tools/            StartupManager, AppxBloat, BrowserExtensions, WindowsFeatures, OrphanFinder
│  └─ storage/          SQLite wrapper + schema migrations (%ProgramData%\Avel\avel.db)
├─ src/cli/             avel-cli.exe: list | uninstall | scan | force | trace | undo  (--json, --dry-run)
├─ src/app/             avel.exe (Qt Widgets): models/ views/ dialogs/ widgets/(HunterTarget) i18n/(avel_en.ts, avel_ru.ts, avel_hy.ts)
├─ tests/unit/          GoogleTest     tests/fixtures/ (synthetic hives, folder trees)
├─ tests/sandbox/       .wsb configs + PowerShell scenarios + test-installer list
└─ docs/                ADRs + research notes (save both research reports here)
```

### Key mechanisms
- **Inventory:**
  - **Registry sources:** the Uninstall keys under `HKLM`, `HKLM\...\WOW6432Node` and every user's `HKCU`. Classify each entry using `SystemComponent`, `ParentKeyName`/`ReleaseType` (updates) and `WindowsInstaller`.
  - **MSI:** `MsiEnumProductsEx(MSIINSTALLCONTEXT_ALL)`.
  - **Store apps:** `PackageManager.FindPackagesForUserWithPackageTypes`, run on an MTA thread. Packages with `SignatureKind::System` are protected.
  - **Merging duplicates:** use BCU's related-entry scoring. Same InstallLocation or same UninstallString counts as a match straight away; otherwise a weighted score over name, publisher, version, install date and bitness, with fuzzy name matching via Sift4.
  - **InfoAdders:** detect the installer type (MSI, NSIS, Inno, InstallShield, WiX Burn, Squirrel) from the uninstall string and the uninstaller exe's version info, then derive the quiet command:
    - Inno: `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART`
    - NSIS: `/S`
    - MSI: `msiexec /x {GUID} /qn`

    They also fill in size, icon path and the Authenticode signer.
- **Uninstall runner:**
  - Launch the uninstaller with `CreateProcessW` inside a **Job Object** whose completion port signals when the last process exits. This catches NSIS's trick of copying itself to %TEMP% (as `Au_.exe`) and exiting.
  - Success check: the Uninstall key is gone and the main exe is gone.
  - Run MSI uninstalls one at a time (the `_MSIExecute` mutex).
  - In a batch, create one restore point for the whole batch.
- **Leftover scanning:** port BCU's confidence model. Each record adds or subtracts points: uninstaller key +20, explicit path reference +4, company match +4, executables still present −4, another app uses the folder −7, and so on. Total < 0 = Bad, 0–1 = Questionable, 2–4 = Good, ≥5 = VeryGood. Safeguards:
  - Never match names of 4 characters or fewer.
  - Recurse at most 2 levels deep.
  - Keep a blacklist of shared folders (Microsoft, Common Files, WindowsApps, WinSxS, and so on).
  - Never scan system or known-folder roots.

  Then **extend** the "explicit path reference" record to services (`ImagePath`), scheduled-task `<Exec>`, App Paths, file associations and context-menu handlers.

  The **Safe/Moderate/Advanced** presets map to confidence thresholds and scan depth. Only Good/VeryGood items are pre-checked; Questionable items are shown unchecked; Bad items are hidden.
- **ChangeSet (safety core):** every destructive action, from any module, goes through plan → preview → backup → execute → journal.
  - **Registry:** keys are serialized to SQLite and also exported as `.reg`.
  - **Files:** moved on the same volume to `%ProgramData%\Avel\Quarantine` (keeps permissions, so restores are exact). Old quarantine items are purged automatically after N days.
  - **Restore point:** `SRSetRestorePointW`. Respect the 24h frequency limit and check System Protection is on.
  - **Locked files:** Restart Manager (`RmGetList`) shows the locking process, with `MoveFileExW(MOVEFILE_DELAY_UNTIL_REBOOT)` as the fallback.
  - **Undo Center:** restores any ChangeSet.
- **Forced removal and Hunter mode:**
  - Build a pseudo-entry from a name, folder, exe, or a crosshair-picked window (`WindowFromPoint` → PID → `QueryFullProcessImageNameW` → match against install locations).
  - Run a deep scan: wider depth, plus a whole-volume name search via MFT enumeration (`FSCTL_ENUM_USN_DATA`).
  - Stop processes and services, and remove scheduled tasks.
  - Clean up broken MSI registrations (the packed-GUID keys under `Installer\Products`, `UpgradeCodes` and `UserData`).
  - Repair or remove dead Add/Remove Programs entries.
- **Install tracker (snapshot-diff, no driver):**
  - **Start:** record each NTFS fixed volume's USN journal ID and next position. Take a registry snapshot of HKLM\SOFTWARE, the Services key and every user's HKCU\Software: for each key, its LastWriteTime plus a hash of each value. Save the known spots' values (environment variables, SharedDLLs) exactly.
  - **Stop:**
    - Read the USN journal from the recorded position to get files created, modified or renamed. Check that the journal ID is unchanged and hasn't wrapped.
    - Re-walk the registry, reading values only for keys whose LastWriteTime changed.
    - Apply noise filters (Temp, Prefetch, logs, browser caches, Avel's own files).
  - Save the trace as a folder of JSON files under `%ProgramData%\Avel\Traces` (not SQLite; see [ADR 0006](adr/0006-trace-folders-registry-snapshot.md)) and link it automatically to the inventory entry through the new Uninstall key that appears in the diff.
  - Traced items get top confidence at uninstall time. Trace logs can be exported and imported.
- **Avel Tools:**
  - **Startup manager:** Run keys, Startup folders, scheduled tasks and services. "Disable" works the way Task Manager does it, via the `StartupApproved` keys.
  - **Windows apps:** remove per user or for all users, plus deprovisioning, with a protected list.
  - **Windows features and capabilities:** via DismAPI.
  - **Browser extensions:**
    - List Chromium and Firefox extensions.
    - Remove policy-forced and externally registered ones, the stubborn kind. Their keys live under `Policies\...\ExtensionInstallForcelist` and `Software\Google\Chrome\Extensions`.
    - For the rest, open the browser's own extensions page.
  - **Orphan finder:** Program Files folders with no registration.

### Performance design (answer to "how do I make it run fast")
- **Instant startup:** ~~show the cached inventory from SQLite right away, then refresh in the background and reconcile~~. Superseded: a full scan takes about 0.1 s, so there is no cache (see [ADR 0003](adr/0003-no-inventory-cache.md)).
- **Parallel sources:** all inventory sources run in parallel on worker threads, and results reach the UI in batches about every 50 ms. Target: under 1 s cold for about 300 programs.
- **Lazy icons:** extract icons only for visible rows, on a background thread, with a disk cache.
- **Sizes:** use `EstimatedSize` first, then compute real folder sizes at low priority.
- **File enumeration:** `FindFirstFileExW(FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH)` with parallel roots. Whole-disk searches go through MFT enumeration, which takes seconds, like Everything.
- **Registry:** size buffers up front with `RegQueryInfoKeyW`. The tracker skips keys whose LastWriteTime is unchanged.
- **GUI:** `QAbstractTableModel` + proxy model, uniform row heights, never `QTableWidget`, and no per-row widgets.
- **Build:** Release with `/O2` and whole-program optimization (`/GL` plus LTCG), and dependencies linked statically through the `x64-windows-static-md` vcpkg triplet. Do a profiling pass with Windows Performance Recorder in Phase 7.

## Roadmap (each phase ends with a working, testable build)
| Phase | Deliverable | Done when |
|---|---|---|
| **0. Foundation** | Toolchain (VS 2022/2026 C++ workload with Windows 11 SDK, CMake 3.28+, Ninja, vcpkg, Qt 6.12), `git init`, CMake presets, empty `avel_core`/`avel-cli`/`avel` (elevated, windows11 style), GoogleTest, Windows Sandbox harness, research notes saved in `docs/`, LICENSE/NOTICE/README, GitHub Actions CI | `cmake --preset release` builds; `ctest` passes; avel.exe starts elevated; CI green once pushed (push only after you confirm) |
| **1. Inventory** | Registry (all hives), MSI, AppX sources; Merger; InfoAdders; `avel-cli list --json`; GUI table with search, filters (Win32/Store/updates/system), details pane, cached startup | List matches Settings > Apps with any differences explained; cold start under 1 s |
| **2. Uninstall** | Job-object runner, MSI and AppX removal, success check, batch quiet queue, restore point per batch, progress UI | In Sandbox: MSI, NSIS, Inno and MSIX samples removed both interactively and quietly; a batch of 5 works |
| **3. Leftovers + safety** | Scanners, ConfidenceEngine, ProtectedPolicy, ChangeSet (backup/quarantine/undo), Restart Manager and reboot-pending ops; leftover review tree with confidence colors and "why it matched" | Samples' leftovers removed; undo restores keys and files exactly; zero hits in protected areas across the sample suite |
| **4. Avel Force** | Pseudo-entries, Hunter crosshair, deep MFT scan, service/task/process handling, broken-MSI and dead-entry cleanup, orphan finder | An app with its uninstaller deleted is fully removed in Sandbox |
| **5. Avel Trace** | TraceSession (USN + registry snapshot), noise filter, TraceStore, automatic linking, trace-based uninstall, export/import | 3 traced installs, then trace-based uninstall leaves **zero diff** against a fresh pre-install snapshot (the tracker checks itself) |
| **6. Avel Tools** | Startup manager, Windows apps/bloatware, features and capabilities, browser extensions, Steam/Epic sources, all going through ChangeSet | Each tool verified in Sandbox, and every change can be undone |
| **7. Polish** | Complete and review the EN/RU/HY translations (`tr()` has been in place since Phase 1), plus `qtbase_hy`, dark/light themes, shortcuts, optional Explorer "Uninstall with Avel" verb, settings, log viewer, portable static build, WPR profiling | Daily-driver ready |

## Verification strategy
- **Unit tests (GoogleTest)** for the pure logic: confidence scoring, Sift4 and the merge logic, parsing of `UninstallString` (unquoted paths with spaces, `rundll32`, `msiexec` variants), packed-GUID conversion, path normalization, noise filters.
- **Registry fixture tests:** load synthetic hives with `RegLoadAppKeyW` and run the scanners against them, without ever touching the real registry. Folder fixtures go under temp directories.
- **Integration tests in Windows Sandbox** (needs Windows 10/11 Pro; see `tests/sandbox/README.md`):
  - `tests/sandbox/Start-AvelSandbox.ps1` generates a `.wsb` config that maps the build output and `tests/sandbox/` (including the downloaded `installers/`) read-only into the Sandbox.
  - PowerShell scenarios install one sample of each installer type: an MSI (7-Zip), NSIS (Notepad++), Inno (e.g. Git for Windows), WiX Burn, Squirrel and MSIX.
  - They then run `avel-cli uninstall --quiet`, `avel-cli scan --json` and `avel-cli undo`, and assert on the JSON.
- **Hyper-V VM with checkpoints** for longer manual sessions: the tracker, all-users AppX removal, and forced removal.
- **Rule:** destructive features run only in Sandbox or the VM until Phase 3's backup and undo are proven. `--dry-run` and GUI "simulate" modes exist from Phase 2 on.

## Status
- **Phase 0 (foundation): built and verified locally.**
  - In place: CMake presets with vcpkg, `avel_core`, `avel-cli` and `avel` (elevated manifest), and the EN/RU/HY translation infrastructure with live switching (checked through UI Automation).
  - Also in place: GoogleTest (7 tests passing), the `deploy` target (a self-contained `bin`, about 30 MB), the Sandbox harness, the CI workflow, LICENSE/NOTICE and the docs.
  - The Sandbox smoke test passes: `Start-AvelSandbox.ps1 -WaitForReport` reports that `avel-cli` and the GUI run in a clean sandbox.
  - **GitHub:** the first push was on 2026-10-29 (`main` only, to `github.com/IgitGhost/Avel`). Its CI run failed at the "Install Qt" step, before anything was built. Since Qt 6.11 Qt's online repository has a folder per architecture, which aqtinstall 3.3.0 (its latest release, and `install-qt-action`'s default) can't read. CI now installs aqtinstall from its master, pinned by commit, which supports it. With that, "Install Qt" passes (run of `33a6313`, pushed the same day with Phase 5b). Still open: a green run of the whole job.
- **Phase 1 (inventory): done and verified locally.**
  - **Sources:** registry (HKLM in both views, plus every user profile, with signed-out users' hives loaded read-only), Windows Installer and packaged apps. They are merged with Bulk Crap Uninstaller's relatedness scoring, which is ported and checked against reference values from BCU's own C# code. See [inventory.md](inventory.md).
  - **Settings parity:** `scripts/Compare-SettingsApps.ps1` finds all 172 Settings entries identical in Avel (name, version, publisher, date). Avel shows 28 extra packaged apps that Settings hides by an internal policy; they are documented in inventory.md.
  - **Speed:** a full scan takes about 0.1 s, and the GUI shows the list 0.4 to 0.9 s after launch. The startup cache was dropped (ADR 0003).
  - **CLI:** `avel-cli list [--all] [--json] [--timing]`.
  - **GUI:** program table with icons, search, categories, a details pane, F5 refresh and diagnostics in the status bar. All of it is translated to RU and HY (71 strings) and was checked through UI Automation in all three languages.
  - **Tests:** 55 unit tests pass, including the registry fixture tests in a volatile HKCU key and a read-only smoke scan.
- **Phase 2 (uninstall): done and verified in Windows Sandbox.** Details in [uninstall.md](uninstall.md).
  - **Core** (`src/core/uninstall`):
    - per-program plans: MSI via `msiexec /x`; silent switches for Inno, NSIS, WiX Burn, Squirrel and SdbInst; packages via PackageManager;
    - refusal reasons;
    - a job-object runner that waits for the whole process tree;
    - a registration-based success check, plus retries while msiexec reports 1618 (busy);
    - a sequential batch with a System Restore point;
    - NSIS and Inno detection from the uninstaller's manifest, plus the `NullsoftInst` signature.
  - **CLI:** `avel-cli uninstall <id|name>... [--quiet] [--dry-run] [--yes] [--no-restore-point] [--json]`, with Ctrl+C to stop or terminate.
  - **GUI:**
    - Uninstall action (Del, toolbar, context menu);
    - a confirmation dialog showing each plan, with options for silent mode, a restore point and Simulate;
    - a progress dialog with Stop;
    - RU/HY translations: 138 of 138 strings.
  - **Tests:** 77 unit tests pass. They include:
    - the job-object runner, using a helper that spawns a child and exits like NSIS;
    - plans for every technology;
    - a read-only smoke test that plans every installed program (on the dev machine, 161 of 199 get a silent plan).
  - **Host GUI check:** verified on the host in Simulate mode only.
  - **Windows Sandbox** (`tests/sandbox`, scenario `uninstall`, samples from `Get-SampleInstallers.ps1`): **RESULT: PASS** (run of 2026-10-19 20:53; the two runs before it passed too).
    - **Interactive round:** 7-Zip MSI, Notepad++ NSIS, Inno Setup, VC++ Burn and MSIX were removed one at a time without silent switches, in 0.4 to 6 s each. The clicker pressed each uninstaller's buttons; `clicks.log` shows them all:
      - msiexec: "Да";
      - Notepad++: "Удалить", then "Yes" in a message box, then "Закрыть";
      - Inno: "Yes", then "OK";
      - Burn: "Удалить" on the maintenance page, then "Закрыть".
    - **Quiet batch:** all five were removed with a restore point, about 1 s each, and the restore point "Avel: uninstall 5 programs" was created.
    - **GUI:** 7-Zip (silent, with a restore point) and the MSIX were removed.
    - **Known leftover:** Notepad++'s uninstaller, silent or not, removes the app but leaves its Windows 11 context-menu sparse package ("Notepad++") registered. The scenario reports it as `KNOWN` rather than a failure. It's a leftover for Phase 3, and a candidate for offering related packages together with their app.
    - **Clicker fix (2026-10-19):** managed UI Automation reports classic Win32 buttons as panes, so the old clicker found none. It now finds them with `EnumChildWindows` and presses them by posting `WM_COMMAND`. See `tests/sandbox/README.md`.
- **Phase 3 (leftovers, safety and undo): done and verified in Windows Sandbox, in three sub-phases.** Plan approved 2026-10-19.
  - **3a (read-only scan): done.** Details in [leftovers.md](leftovers.md).
    - **Core** (`src/core/leftovers`):
      - BCU's confidence model and name matching (ported, checked against BCU's values);
      - snapshots taken once per scan;
      - 11 scanners: install folder, common folders, shortcuts, software keys, the Uninstall key, path references, related (sparse) packages, services, scheduled tasks, COM classes with their shell extension registrations, and firewall rules;
      - every user's profile and hive are scanned, with `other-user-data` for other users' items;
      - two protection layers (HardFloor from the live system, ProtectedPolicy);
      - merging and containment.
    - **CLI:**
      - `avel-cli scan <id|name>... | --all [--json] [--min-level ...] [--all-levels] [--timing]`;
      - `avel-cli uninstall` scans for leftovers after the batch (`leftover_scan` in its JSON), unless `--no-leftover-scan` is given.
    - **Speed:** all 201 programs on the dev machine in 1.3 s, about 0.5 s for one program.
    - **Tests:** 111 unit tests pass. They include:
      - a fixture test with a fake program and decoys in a temp tree plus volatile registry keys;
      - `LeftoverSmokeTest`: scans every program on the machine and finds zero hits for an independent oracle (`IKnownFolderManager`, profiles, the Windows folder, drive roots, core keys).
    - **Windows Sandbox,** scenario `leftovers`: **RESULT: PASS** (2026-10-19 22:19).
      - The preview found each sample's folder, Uninstall key, shortcuts, App Paths and COM class.
      - After a quiet batch uninstall of all five, the scan reported:
        - Notepad++'s sparse package `NotepadPlusPlus_1.0.0.0_neutral__2247w0b46hfww` (Good);
        - its leftover `HKLM\SOFTWARE\WOW6432Node\Notepad++` key (Good);
        - a Questionable `HKLM\SOFTWARE\Setup` matched to Inno Setup.
      - Zero oracle hits.
  - **3b (removal and undo): done.** Details in [changes.md](changes.md) and [ADR 0004](adr/0004-changeset-folder-journal.md).
    - **Core** (`src/core/changes`):
      - **ChangeSet store:** a folder per set with plan, journal and summary, under a protected `%ProgramData%\Avel\Changes`; 30-day purge.
      - **Registry backup:** exact trees (raw values through the native API, classes, security), merging for partial deletions.
      - **File quarantine:** renames through the handle, never following links. A folder with files in use moves file by file, and Restart Manager names the holders.
      - **Services, tasks and packages:** services and tasks with undo; packages only when explicitly allowed.
      - **Executor:** a safe order, protection checked again before each change, undo that never replaces anything new, and recovery after a crash.
    - **CLI:**
      - `avel-cli uninstall ... --remove-leftovers [--min-level good|very-good] [--allow-irreversible]`;
      - `avel-cli undo --list | --show <id> | <id> [--item <n>]... | --purge`.
    - **Tests:** 137 unit tests pass. They include:
      - registry and quarantine round trips compared by independent dumpers (hard links, streams, long paths, junctions, unterminated strings, unpaired surrogates);
      - store tests (a torn journal line, locks, purge, untrusted roots);
      - an executor test that kills the run after each of its 14 journal records, recovers, and undoes back to the original.
    - **Windows Sandbox,** scenario `removal`: **RESULT: PASS** (2026-10-19 23:30).
      - Removed: all 11 leftovers of `Contoso Fixture` (folders, keys, a Run value, a service, a task, a COM class with its shell extension and `Approved` entry) and Notepad++'s sparse package.
      - Unchanged: the canaries and the other program; the firewall rule was only reported.
      - `undo --item` and then `undo` gave back identical `avel_state_dump` dumps.
      - A run killed after 7 records was settled by the next run and undone exactly.
    - **Decision (2026-10-19):** COM classes and their shell-extension entries are removed with undo, not just reported as first planned.
  - **3c (GUI): done.** Details in the "Front ends" sections of [leftovers.md](leftovers.md) and [changes.md](changes.md).
    - **Uninstall flow:**
      - the confirm dialog gained "Look for leftovers afterwards";
      - the progress dialog scans on its worker thread and opens `LeftoverDialog` by itself;
      - Remove runs the set in `ChangeSetProgressDialog`, whose Undo puts it back at once.
    - **`LeftoverModel`:**
      - a tree of program, group and item, with COM registrations in a group of their own;
      - each item under the item that contains it;
      - tri-state checks, where a checked container takes its contents along;
      - presets Safe, Moderate (the default) and Advanced, with unlikely items behind a toggle;
      - "why it matched" as a column, and as a tooltip with the points.
    - **Undo Center** (Tools menu): restores all or the checked items of a set, or deletes its backup.
    - **At startup** interrupted sets are settled and old ones purged. A prompt appears when any were settled.
    - **Preview leftovers** (File menu) shows the scan for installed programs, read-only. On the dev machine, Java 8 (64-bit) gives a tree of 8,598 items.
    - **Core:** `restore()` reports each item, for the progress bar.
    - **Text:** `LeftoverText` translates the codes. There are 191 new strings, complete in RU and HY with plural forms, and checked on screen in all three languages.
    - **Tests:** 146 pass. The 9 new `LeftoverModelTest` cases cover grouping, containment, checks, presets, hidden items and the plan.
    - **Windows Sandbox,** scenario `removal` with a new GUI round: **RESULT: PASS** (2026-10-21 21:07).
      - Through UI Automation, the GUI uninstalled `Contoso Fixture`. The leftover dialog opened by itself, with 12 of 13 items checked (the firewall rule is shown only).
      - "Remove 12 items" removed all of them.
      - In the Undo Center, Restore all put back the 11 restorable items; the shortcut came back with its folder. The `avel_state_dump` dumps were identical.
    - **Test fix:** `avel_state_dump` printed a missing registry key without a line break, which swallowed the next spec's section. Checks that an item is gone were blind to some keys and values. After the fix, round 1 also checks the removal through the dumps, and passes.
    - **Polish (2026-10-21):**
      - Qt ships no Armenian catalog, so Avel carries its own `src/app/i18n/qtbase_hy.ts`: 51 strings in 6 Qt contexts (standard buttons, the edit and scroll bar menus, message boxes). The source texts are taken from Qt's own catalog.
      - The Undo Center's short columns fit their headers in every language.
      - Retranslating open dialogs isn't needed: every dialog is window-modal, so the language menu can't be reached while one is open.
- **Phase 4 (Avel Force): done and verified in Windows Sandbox, in three sub-phases like Phase 3.** Plan approved 2026-10-21. Details in [force.md](force.md).
  - **User decisions (2026-10-21):**
    - running processes are listed and ended only after confirmation;
    - broken MSI products: Windows Installer's registry registration is removed with undo, and the cached package in `C:\Windows\Installer` stays;
    - Hunter picks windows only (no tray icons);
    - drivers are shown, never removed;
    - Force never runs the program's uninstaller.
  - **4a (forced removal, core and CLI): done.**
    - **Inventory:** entries are flagged `uninstaller_missing` (the file, or a host's script or DLL, is gone) and `msi_package_missing` (neither Windows Installer's cached package nor the original is there). The uninstall plan refuses them with `uninstaller-missing` and `msi-package-missing`.
    - **Core** (`src/core/force`, `platform/processes`, `leftovers/msi_scanner.cpp`):
      - targets for a program, a folder (`folder:<path>`) or the program a file belongs to;
      - the processes running from a target's folders, ended with `WM_CLOSE` and then `TerminateProcess`;
      - Windows Installer's registration (packed product codes, product keys, install properties, upgrade codes, components, Burn dependencies), with its containers added to the hard floor;
      - an MSI program's folder derived from its components' key paths when it records none;
      - drivers reported, never removable.
    - **CLI:** `avel-cli force <program>... | --folder <path> [--name] | --exe <path> [--dry-run] [--yes] [--end-processes] [--min-level] [--allow-irreversible] [--json]`.
    - **Fixes found on the way:**
      - undo dropped the auto-inherited mark of a key's DACL (`D:AI`), which Windows Installer's keys have;
      - App Paths, which both registry views share, was reported twice.
    - **Tests:** 164 unit tests pass.
    - **Windows Sandbox,** scenario `force`: **RESULT: PASS** (2026-10-21 22:21).
      - Contoso Fixture without its uninstall script, its app running: `force --yes` alone refused; with `--end-processes` it ended the app and removed 14 items, and undo gave back identical dumps.
      - 7-Zip's MSI with its package and Windows Installer's cached copy deleted: 42 items, including the shell extension and Windows Installer's registration. Windows Installer then no longer knew the product. Undo gave the registration back exactly, and the product counted as installed again.
      - A folder without an Uninstall entry went alone; Program Files, `System32` and `C:\Users` were refused.
  - **4b (broken entries, deep search, orphans; core and CLI): done.**
    - `avel-cli list --broken` shows the entries that can't uninstall themselves, with their reasons (`no-install-folder` marks a dead entry); `force` removes them.
    - **Deep search** (`--deep` on `scan`, `force` and `uninstall`): folders named after the program anywhere on the fixed NTFS volumes, read from each volume's master file table ([ADR 0005](adr/0005-deep-search-from-the-mft.md)). A folder found only by its name is Questionable (new evidence `outside-common-locations`, −2); inside the publisher's folder it is Good.
    - **Orphans** (`avel-cli orphans`): folders in both Program Files, ProgramData and each user's `AppData\Local\Programs` that no installed program owns by folder, name, publisher or reference. They are Questionable (`no-owner`), never pre-checked. Users' AppData was left out after the first host run: about 130 of its folders were games' saves, package caches and portable tools' data.
    - **Tests:** 170 unit tests pass (one, reading a real volume, runs elevated only).
    - **Windows Sandbox,** scenario `force` with three new rounds: **RESULT: PASS** (2026-10-21 23:00). `removal` and `leftovers` still pass.
      - A dead entry is listed by `list --broken`, and `force` removes its key alone; undo gives it back exactly.
      - The volume reading tests pass elevated; `C:\Tools\Contoso Fixture` is found by `scan --deep` only, at Questionable. The Sandbox's volume was read in 16 ms.
      - `orphans` lists a folder nobody owns, and none of the installed programs' or Windows' folders. A folder named after the fixture's publisher ("Contoso Orphan", publisher "Contoso Ltd") is owned. The first run listed it, because BCU's `trim_publisher` keeps "Ltd"; orphans now drop legal forms themselves.
  - **4c (GUI, Hunter, translations): done.** Details in [force.md](force.md), section "The GUI".
    - **Force flow:**
      - **Entry points:** File > Force uninstall… (Shift+Del), the list's context menu, and File > Force uninstall a folder…. The uninstall dialog offers it for a missing uninstaller or MSI package, and the uninstall progress dialog for uninstallers that failed.
      - **Steps:** it scans on a worker, then shows the review ("Forced removal of X"). After Remove it lists the processes running from the program's folders and ends them once the user agrees. Then it runs the ChangeSet.
      - The processes are ended after the review rather than before, so nothing is closed for a removal the user cancels.
    - **Hunter:** a crosshair on the toolbar. Dragged onto a window, it selects the window's program and offers Uninstall, Force uninstall, Preview leftovers, Open install folder and End process. For a program file no entry owns, it offers to force its folder.
    - **Program list:** a Broken category, a warning badge on broken rows, and notes in the details pane.
    - **Leftover dialog:**
      - "Search the whole disk" reruns the scan with the deep search;
      - Windows Installer's registration has its own group;
      - drivers are shown only.
    - **Orphans:** Tools > Find orphaned folders… opens the review with nothing checked.
    - **Fixes found on the way:**
      - Popup menus now show copies of the window's actions. Qt's accessibility lost the File menu's items for UI Automation, and so for screen readers, once a popup had shown them first. The old table context menu had this too.
      - "Why it matched" now lists evidence worth no points, so orphans say why they're listed.
    - **Translations:** 87 new strings each in Russian and Armenian.
    - **Tests:** 173 unit tests pass.
    - **Host check:** unelevated, so nothing could be removed.
      - The Hunter was dragged with synthetic mouse input onto 7-Zip and onto a copy of 7-Zip in a folder no program owns.
      - Screenshots in English, Russian and Armenian.
    - **Windows Sandbox,** scenario `force` with a new GUI round: **RESULT: PASS** (2026-10-23 20:57).
      - The fixture was forced from the uninstall dialog with its app running.
      - The whole-disk search listed `C:\Tools\Contoso Fixture`, unchecked.
      - Remove ended the app and removed 14 items, and the Undo Center gave back identical dumps.
      - The orphan finder listed the folder nobody owns.
- **Phase 5 (Avel Trace): in progress, in three sub-phases.** Plan approved 2026-10-23. Details in [trace.md](trace.md) and [ADR 0006](adr/0006-trace-folders-registry-snapshot.md).
  - **User decisions (2026-10-23):**
    - both **Install and trace** (Avel launches the installer and stops by itself) and manual start and stop; a program the installer starts at the end ("Run X now") doesn't hold the trace;
    - uninstall by trace in both modes: the program's uninstaller first, then the traced items join the leftover review; or through Avel Force, without the uninstaller;
    - the known spots (environment variables, SharedDLLs) are saved exactly, and removal takes back only the installer's part; other edits to existing items are shown only;
    - VeryGood only for items in the folders and keys the installer created that are tied to the program, so noise is never pre-checked;
    - a trace survives a restart; each trace is a folder of JSON files, not SQLite;
    - a missing USN journal is created (64 MB); existing ones are never resized;
    - no background monitor; firewall rules stay report-only.
  - **5a (tracker core and CLI) is done.** The Sandbox `trace` scenario passes (run 9, 2026-10-25 12:42; see below).
    - **Refactors first** (behavior unchanged): the native registry helpers moved to `platform/nt_registry`, the one-folder-per-record mechanics to `platform/record_folders` (shared with the ChangeStore), and FNV-1a went into `platform/hash.h`.
    - **Core** (`src/core/tracker`, `platform/usn_journal`):
      - files from each fixed NTFS volume's USN journal: start positions, reading every 2 s while an Avel process drives the trace, checkpoints, net effects per file (replacements, roots only, moved-in folders, hard links, items open across the start) and incomplete volumes;
      - a binary registry snapshot with a hash per value, taken on up to 8 threads, compared by reading values only where a key's LastWriteTime changed; created services, tasks and firewall rules come out as such;
      - the known spots saved exactly;
      - a noise filter whose rules are data;
      - Install and trace: a job object, the installer's helpers, programs it starts, Windows Installer's mutex looked at but never taken, a quiet period and settling;
      - the trace store: one open trace, resumed after a restart or a crash.
    - **CLI:** `avel-cli trace start | stop | run [--timeout <s>] -- <installer> [<args>...] | list | show <id> [--all] | cancel | delete <id>`, with `--json`.
    - **Speed:** on the dev machine (unelevated smoke test), about 500,000 keys and 790,000 values make a 48 MB snapshot, taken in 3.2 to 3.5 s and compared in 1.4 to 1.8 s.
    - **Tests:** 311 unit tests; 306 pass on the host, and the 5 that need administrator rights run in the Sandbox.
    - **Windows Sandbox,** scenario `trace`, checked against an independent oracle (`avel_capture`): an idle round, a container round (a system file deleted, another renamed), `Contoso Trace Fixture` between `trace start` and `trace stop`, 7-Zip, Notepad++ and Inno Setup through `trace run`, an app started at the end, a killed run and a replaced journal.
      - **Tuning (2026-10-24), seven runs:** 20, 82, 84, 2 and 1 problems, then **RESULT: PASS** (run 6, 21:08).
        - The Sandbox's system files are Windows Container Isolation placeholders that NTFS creates on first access; the tracker tells them from real changes by their records (see [trace.md](trace.md), "Windows containers").
        - Other fixes: a console installer no longer writes into `--json` output; Code Integrity's extended attributes and other metadata-only changes are noise; deletions of files open since before the trace; noise rules for what the idle round found.
      - **After the profile fix below (2026-10-24 and 25), runs 7 to 9:** 1 and 2 problems, then **RESULT: PASS** (run 9, 2026-10-25 12:42).
        - Run 7, the oracle's error: Edge's component unpacker extracted files into a temporary folder while the oracle's first capture walked the disk, then moved the whole folder to `LocalState\EBWebView\hyphen-data` before the trace started. The trace was right to report nothing. The files had no records of their own: a folder moved as a whole takes its contents along, and neither the journal nor the oracle's name watch names them. The oracle now dates such lines by the folder's move.
        - Run 8, a tracker bug: WMI renames `WmiApRpl_new.ini` over `WmiApRpl.ini`, and had kept the new file open since before the trace. Its leftover `FILE_CREATE` made it look created during the trace, so its old name was lost. An older file renamed over another is now reported as a replacement, and its old name as deleted.
        - Run 8, the oracle: two `en-US` placeholder folders came and went only in the container's listings, with no journal record of either name. The oracle no longer counts such items against the trace.
        - `leftovers`, `removal` and `force` pass with the profile fix, and again on the final code (2026-10-25, 12:49 to 12:55).
    - **Fix found on the way (affects Phases 1-4):** `find_value(key.values(), ...)` inside an `if` kept a pointer into a temporary list that was already destroyed. As a result, every user's profile folder read as empty.
      - **Effects:**
        - the hard floor guarded only `C:\Users`, not each profile, its AppData, its packages' folders or OneDrive;
        - signed-out users' hives weren't loaded;
        - the trace's per-user noise rules missed.
      - **Fix:** `find_value` no longer accepts a temporary list, so the compiler rejects the pattern. Unit tests now check the live hard floor for the current user's profile.
  - **5b (linking, uninstall by trace, value edits, export and import): done.** The Sandbox `trace` scenario passes (run 4, 2026-10-29 21:13; see below). Details in [trace.md](trace.md) ("Linking", "Tying", "Uninstalling by the trace", "Export and import"), [leftovers.md](leftovers.md) and [changes.md](changes.md).
    - **Value edits:** a ChangeSet can now change one value instead of removing it (`ValueEditLocation`). The executor writes it only while the value still holds what the plan saw, and the hard floor allows exactly these edits in the known spots. Undo takes back the installer's part of `Path` and the SharedDLLs counts.
    - **Linking:** a finished trace is linked to the program it installed by itself, or by hand. It is kept while that program is installed and for 30 days after it is uninstalled. `trace export` and `trace import` move a trace to another machine.
    - **Uninstall by trace:**
      - `uninstall` uses the program's trace by default (`--no-trace` leaves it out). After the uninstaller, the trace's items and value edits join the leftover review.
      - `force` reverts a program by its trace without any uninstaller. `force --trace <id>` reverts a trace whose program's entry is gone or that was imported.
      - Only items tied to the program are VeryGood. What existed before the install is never offered.
    - **Containers that hold more than the trace saw:** a folder or key the installer created whole, holding since then what other programs put there (another program's handler in 7-Zip's `DragDropHandlers` key), is no longer offered whole. The trace offers the items it saw in it instead. A package's own registration (Notepad++'s sparse package) goes only through the package's removal.
    - **Windows Sandbox,** scenario `trace` with new 5b parts:
      - round 1 reverts `Contoso Trace Fixture` by force and undoes it, then exports, imports and reverts by `force --trace`;
      - rounds 2 to 4 uninstall 7-Zip, Notepad++ and Inno Setup with their traces.
      - **Tuning (2026-10-25 to 29), four runs:** 6, 11 and 1 problems, then **RESULT: PASS** (run 4, 2026-10-29 21:13).
        - **Two product bugs** (run 1, Notepad++):
          - During Notepad++'s install, Windows installed its compatibility fix database `{22221111-…}.sdb`, an Uninstall entry Settings lists. The trace linked it as the primary program, so `uninstall` ran `sdbinst -u` on Windows' own database. The database is now noise, and among the entries Settings lists, linking puts first the one whose install folder, uninstaller or icon is in a folder the trace created.
          - `sdbinst` wrote into avel-cli's redirected `--json` output: Windows gives a console child the parent's standard handles even when they aren't inherited. A console uninstaller now gets a console of its own whenever Avel's output is redirected.
        - **Noise found by the rounds:** the user's `MrtCache` (names and logos of packages, filled by reading the inventory), Explorer's `ParseAutoexec` after an environment broadcast, the compatibility appraiser and its telemetry, Storage Sense's free-space reading, two of Explorer's empty keys, and Windows' feature configuration (`CurrentVersion\Fcon`, written by its `Flighting\FeatureConfig` tasks on their own schedule). A created folder or key that holds nothing but noise is noise too.
        - **Scenario fixes:**
          - a script variable shadowed by a local one (`$results`);
          - a deleted key or value dated by the last write of the key that held it (7-Zip's `MuiCache`);
          - the shell's records of a restored shortcut and of the file types it has seen (`UFH\SHC`, `FileExts`) are KNOWN at undo;
          - a package's registration is KNOWN while the package stays registered. The scan offers Notepad++'s sparse package at Good, but removing a package needs `--allow-irreversible`, which the rounds don't pass.
        - `leftovers`, `removal` and `force` pass on the final code (2026-10-29, 21:13 to 21:26).
    - **Tests:** 398 unit tests; 393 pass on the host, and the 5 that need administrator rights run in the Sandbox (28 elevated tests pass there).
    - **Follow-ups:**
      - The scenario's KNOWN rule "left below the removal threshold, untied" is broad. Today only Windows' own activity falls under it, but an installer's item that linking failed to tie would pass too. It could accept only items outside the program's names, folders and keys.
      - `LeftoverSmokeTest` sometimes exceeds its 20 s budget on the development machine (602 programs, 24 to 25 s).
      - A console uninstaller run while avel-cli's output is redirected opens a visible console window.
      - A created task's file in `System32\Tasks` is reported as a created file next to the task. FAT and exFAT volumes aren't listed as `untracked`; only NTFS volumes are looked at. ADR 0004 still names SQLite for trace logs.
  - **Next: 5c** (GUI and RU/HY).
