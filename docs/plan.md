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
  - **Start:** record each NTFS fixed volume's USN journal ID and next position. Take a registry snapshot of HKLM\SOFTWARE, the Services key and every user's HKCU\Software: for each key, its LastWriteTime plus a hash of its values.
  - **Stop:**
    - Read the USN journal from the recorded position to get files created, modified or renamed. Check that the journal ID is unchanged and hasn't wrapped.
    - Re-walk the registry, reading values only for keys whose LastWriteTime changed.
    - Apply noise filters (Temp, Prefetch, logs, browser caches, Avel's own files).
  - Save the TraceLog to SQLite and link it automatically to the inventory entry through the new Uninstall key that appears in the diff.
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
  - Still open: the first push to GitHub so CI can run.
- **Phase 1 (inventory): done and verified locally.**
  - **Sources:** registry (HKLM in both views, plus every user profile, with signed-out users' hives loaded read-only), Windows Installer and packaged apps. They are merged with Bulk Crap Uninstaller's relatedness scoring, which is ported and checked against reference values from BCU's own C# code. See [inventory.md](inventory.md).
  - **Settings parity:** `scripts/Compare-SettingsApps.ps1` finds all 172 Settings entries identical in Avel (name, version, publisher, date). Avel shows 28 extra packaged apps that Settings hides by an internal policy; they are documented in inventory.md.
  - **Speed:** a full scan takes about 0.1 s, and the GUI shows the list 0.4 to 0.9 s after launch. The startup cache was dropped (ADR 0003).
  - **CLI:** `avel-cli list [--all] [--json] [--timing]`.
  - **GUI:** program table with icons, search, categories, a details pane, F5 refresh and diagnostics in the status bar. All of it is translated to RU and HY (71 strings) and was checked through UI Automation in all three languages.
  - **Tests:** 55 unit tests pass, including the registry fixture tests in a volatile HKCU key and a read-only smoke scan.
  - Still open from Phase 0: the first push to GitHub.
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
  - Still open from Phase 0: the push to GitHub. The first local commit exists.
- **Next:** Phase 3: leftovers, safety and undo.
