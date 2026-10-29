# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Avel is a Windows-only uninstaller suite (C++20, MSVC, Qt 6.12 Widgets, CMake + Ninja, vcpkg). The
roadmap, the architecture notes and the current status are in `docs/plan.md` ("Status" section).
Each module has a design doc in `docs/` (`inventory.md`, `uninstall.md`, `leftovers.md`,
`changes.md`, `force.md`, `trace.md`), and decisions are recorded in `docs/adr/`.

## Build and test

Always build from a shell prepared by `scripts/dev-env.ps1`. It enters the VS x64 dev shell, sets
`VCPKG_ROOT` and `QT_ROOT_DIR`, and puts the VS-bundled CMake first. An older standalone CMake on
`PATH` would otherwise win and fail on the 3.28 minimum.

```powershell
. .\scripts\dev-env.ps1                    # dot-source; -QtDir <kit> if Qt isn't at the default
cmake --preset release                     # or debug; -DAVEL_BUILD_APP=OFF builds without Qt
cmake --build --preset release
ctest --preset release                     # all tests
ctest --preset release -R FileChangesTest  # tests are discovered as Suite.Test
build\release\bin\avel_core_tests.exe --gtest_filter=FileChangesTest.*
cmake --build --preset release --target deploy               # Qt DLLs + MSVC runtime into bin\
cmake --build --preset release --target update_translations  # refresh src\app\i18n\*.ts
```

- Every executable, the tests included, lands in `build\<preset>\bin`.
- Warnings are errors (`/W4 /WX /permissive-`). Formatting is `.clang-format` (Google-based, 4
  spaces, 110 columns).
- Source and test lists are explicit. Add a new file to `src/core/CMakeLists.txt` (or the
  cli/app one) and new tests to `tests/CMakeLists.txt`.
- Judge test runs on the release preset. The Debug build's leftover smoke test exceeds its time
  budget.
- A few tests need administrator rights (MFT and USN volume reads, registry links). They call
  `GTEST_SKIP` when unelevated and run inside the Sandbox scenarios instead.
- Unit tests must never change the system. Registry tests work in a volatile key under
  `HKCU\Software\Avel.Tests`, and file tests in temporary folders.
- CI (`.github/workflows/ci.yml`) configures, builds and tests the release preset.

## Windows Sandbox (destructive end-to-end tests)

Real installs, uninstalls, removals and traces run only in Windows Sandbox (`tests/sandbox/README.md`).

```powershell
.\tests\sandbox\Get-SampleInstallers.ps1   # once: 7-Zip MSI, Notepad++, Inno Setup, VC++ Redist, test MSIX
cmake --build --preset release --target deploy
.\tests\sandbox\Start-AvelSandbox.ps1 -Scenario trace -WaitForReport -CloseWhenDone -TimeoutSeconds 3600
```

- Scenarios are `tests/sandbox/scenarios/<name>.ps1`: `logon`, `uninstall`, `leftovers`,
  `removal`, `force`, `trace`. Each phase's acceptance is one of them, and they end with
  `RESULT: PASS` or `RESULT: FAIL (n problem(s))` in `build\sandbox\results\<name>.txt` (also
  `<name>.json`, `<name>.log`).
- The Sandbox maps `build\release\bin` live. Don't rebuild that preset while a scenario runs.
- Only one Sandbox runs at a time.
- Don't keep files in `build\sandbox\results` open (e.g. with `tail -f`) during a run. The guest's
  appends to them fail.
- The Sandbox's Windows UI is in the host OS's base language. Here that may be Russian, so UI
  Automation helpers match localized labels.
- The Sandbox's system files are Windows Container Isolation placeholders, which NTFS creates on
  first access. `docs/trace.md` ("Windows containers") explains how the tracker tells them from
  real changes.
- `trace` checks Avel Trace against an independent oracle, `avel_capture`
  (`tests/helpers/capture.cpp`). It must stay free of avel_core code.
- `removal` and `force` compare `avel_state_dump` dumps before a removal and after its undo.

## Architecture

Three targets share one engine:

- **`avel_core`** (`src/core`, static library) holds all logic.
  - It must stay Qt-free.
  - It returns codes, enums and data, never user-facing text.
- **`avel-cli`** (`src/cli`) is English-only, with `--json` output for scripts and the Sandbox scenarios.
- **`avel.exe`** (`src/app`, Qt Widgets) owns all UI text.
  - Every string goes through `tr()`.
  - Core codes become text in the `*_text.cpp` files.
  - English, Russian and Armenian must all ship (`src/app/i18n`).

`avel.exe` requires elevation (`requireAdministrator`). `avel-cli` runs as invoker: `list` works
unelevated, and the commands that change the system need an elevated shell. The engine reads every
user's registry: the loaded hives under `HKU\<SID>`, and `NTUSER.DAT` of signed-out users.

Modules under `src/core`, in pipeline order:

- `inventory/`: Uninstall keys of all hives, MSI, and AppX sources, run in parallel and merged with
  BCUninstaller's related-entry scoring. There is no cache (ADR 0003).
- `uninstall/`: builds a plan (installer detection, quiet switches) and runs the uninstaller in a
  job object so its whole process tree is watched. MSI runs one at a time, and a batch gets one
  restore point. Afterwards it checks that the program is really gone.
- `leftovers/`: scanners plus the confidence engine ported from BCUninstaller. The port is credited
  in `NOTICE`, so keep that credit.
  - Protection has two layers: `HardFloor`, built from the live system and not replaceable, and
    `ProtectedPolicy`, the known folders of the machine and every user.
  - Both are checked when scanning, and again by the executor right before each item changes.
- `changes/`: the safety core. Every destructive action, from any module, becomes a ChangeSet:
  plan, then backup, then execute, with an append-only journal.
  - Each set is one folder of JSON (ADR 0004, not SQLite).
  - Registry trees are backed up exactly, with their security.
  - Files go to a 30-day quarantine.
  - Services and tasks are kept with their configuration.
  - Interrupted sets are settled at the next start.
  - The env var `AVEL_TEST_FAULT_AFTER=<n>` makes the CLI crash after n journal records, for
    recovery tests.
- `force/`: builds pseudo-targets from an entry, a folder or an exe, plus Windows Installer
  registration cleanup. It never runs the program's uninstaller, and it ends processes only after
  the user confirms.
- `tracker/`: Avel Trace, with no driver (ADR 0002, 0006).
  - Files come from each NTFS volume's USN journal (`platform/usn_journal`), with net effects
    computed in `file_changes.cpp`.
  - The registry comes from a hashed snapshot that is diffed by key LastWriteTime.
  - Known values (PATH and other environment variables, SharedDLLs) are saved exactly.
  - Noise rules are data (`noise_filter`).
  - Traces are stored one folder per trace under `%ProgramData%\Avel\Traces`.
- `platform/`: Win32 wrappers shared by all of the above:
  - `nt_registry`: native `Nt*` registry calls, used for exact value backup and restore;
  - `record_folders`: the folder-per-record storage behind both ChangeSets and traces;
  - `user_profiles` / `user_hives`, `known_folders`, `paths`, `processes`, `usn_journal`.

## Conventions

- Comments, docs and commit messages are in English. Match the surrounding code's naming and
  comment density.
- Naming:
  - in `src/core`: `snake_case` functions and variables, `PascalCase` types, `avel::<module>`
    namespaces;
  - in `src/app`: Qt style (`camelCase` methods) in `avel::app`;
  - everywhere: members end with `_`.
- After changing UI strings, run `update_translations` and translate the new entries in
  `avel_ru.ts` and `avel_hy.ts`.
- When behavior changes, update the module's doc in `docs/`. Record new design decisions as an
  ADR.
- Phases land on `main` as one commit each (`Phase 4c: ...`). Work in progress lives on a phase
  branch (e.g. `phase5a`) and is squashed in once the phase's Sandbox acceptance passes.
- The public repo is `github.com/IgitGhost/Avel`; only `main` is pushed, and phase branches stay
  local. Pushing needs the maintainer's explicit go-ahead.
- Commits use this clone's own git identity (`.git/config`), not the global one. Don't change
  git config.

## Pitfalls already hit in this codebase

- WIL is built with exceptions off. Use `try_query`, `com_ptr_nothrow` and `unique_mutex_nothrow`.
- Don't hold a pointer into a temporary container. `if (auto* v = find_value(key.values(), ...))`
  dangled and emptied every profile path. `find_value` now rejects rvalues; keep it that way.
- `wil::out_param_ptr` fills its target only at the end of the full expression. Split a call that
  fills it from any use of the value.
- For exact registry backups, use `NtSetValueKey` / `NtQueryValueKey` / `NtEnumerateValueKey`.
  `RegSetValueExW` and `RegEnumValueW` can add a terminator to unterminated `REG_SZ` data.
- `RegSetKeySecurity` drops a DACL's auto-inherited mark unless the descriptor has
  `SE_DACL_AUTO_INHERIT_REQ`.
- Qt and UI Automation:
  - Open dialogs with `open()`, not `exec()`. A UIA `Invoke` that opens an `exec()` dialog blocks
    until it closes.
  - Popup menus get copies of their QActions (`addCopy` in `main_window.cpp`). A QAction shared
    with a short-lived popup disappears from UIA in the menu bar.
- PowerShell scripts in the repo must stay ASCII with CRLF line endings. Windows PowerShell 5.1
  reads a BOM-less file as ANSI. Build non-ASCII strings from code points (`[char]0x414`).
- CI installs Qt with aqtinstall from its git master, pinned by commit (`aqtsource` in `ci.yml`).
  Since Qt 6.11 Qt's online repository has a folder per architecture, which aqtinstall 3.3.0, its
  latest release, can't read. Move back to a released version once one supports it.
