# Avel (Ավել)

**Avel** is a professional software removal suite for Windows 10 and 11.
*Ավել* is Armenian for "broom", and the same word means "excess": a broom for whatever you no longer need.

> **Status: pre-alpha (Phase 3: leftovers and undo).** Avel lists installed programs, runs their
> uninstallers silently or in batches, then finds what they left behind and removes it with a
> backup that can be undone for 30 days. See [docs/plan.md](docs/plan.md) for the roadmap.

## What works today

- **Program inventory.** Avel reads the Uninstall registry keys of the machine and of every user profile, including signed-out users. It also reads Windows Installer products and packaged (Store/MSIX) apps, then merges duplicates.
- **Matches Settings > Apps.** By default Avel shows what Settings shows: on the development machine every Settings entry is identical, including name, version, publisher and date. The remaining differences are explained in [docs/inventory.md](docs/inventory.md).
- **Fast.** A full scan takes about 0.1 s.
- **GUI (`avel.exe`):**
  - a program table with icons, search (Ctrl+F) and categories (Apps, Desktop apps, Store apps, Updates, System components, Everything);
  - a details pane with uninstall commands, registry key, product code and notes;
  - refresh with F5;
  - English, Russian and Armenian UI.
- **Uninstalling.** Select one or more programs and press **Uninstall** (Del). Avel runs each program's own uninstaller, one after another:
  - **silently** when it knows the technology's switch (MSI, Inno Setup, NSIS, WiX Burn, Squirrel, Store packages), otherwise in the uninstaller's window;
  - after a **System Restore point**;
  - watching the uninstaller's whole process tree (job objects), then **checking** that the program is really gone;
  - or as a **simulation** that shows what would run and changes nothing.

  See [docs/uninstall.md](docs/uninstall.md).
- **Leftovers.** After an uninstall, Avel looks for what the uninstaller left behind:
  - folders, files and shortcuts in every user's profile;
  - registry keys and values, COM classes and the shell extensions that name them;
  - services, scheduled tasks, sparse packages and firewall rules.

  Each item gets a confidence level and shows why it matched. A tree groups them by program and
  kind, with Safe, Moderate and Advanced presets. Windows' own folders and keys are never offered.
  See [docs/leftovers.md](docs/leftovers.md).
- **Backup and undo.** Everything Avel removes is backed up first and recorded as it goes:
  - registry trees exactly, with their security;
  - files moved to a quarantine;
  - services and tasks with their configuration.

  **Undo** right after the removal, or **Tools > Undo Center** later (30 days), puts it back
  exactly. A removal that a crash or a power cut interrupted is settled at the next start. See
  [docs/changes.md](docs/changes.md).
- **CLI (`avel-cli.exe`):**

  ```powershell
  avel-cli list            # what Settings > Apps shows
  avel-cli list --all      # plus updates, system components, frameworks and MSI-only products
  avel-cli list --json     # every field, for scripts
  avel-cli list --timing   # per-source scan times and diagnostics

  # Run as administrator. Programs by id (from list --json) or exact name.
  avel-cli uninstall "7-Zip 26.00 (x64)" --dry-run         # show the plan only
  avel-cli uninstall <id> <id> --quiet --yes                # silent batch, no prompt
  avel-cli uninstall <id> --quiet --yes --json              # machine-readable results
  avel-cli uninstall <id> --quiet --yes --remove-leftovers  # then remove what it left behind
  avel-cli scan <id>                                        # preview a program's leftovers
  avel-cli undo --list                                      # what Avel removed, newest first
  avel-cli undo <set id>                                    # put it back
  ```

## Planned modules

| Module | What it does |
|---|---|
| **Avel Uninstall** | Lists every installed program (Win32, MSI, Store/MSIX, every user's per-user installs), runs its uninstaller (also silently or in batches), then finds leftovers and removes them with full backup and undo |
| **Avel Trace** | Records what an installer changes (NTFS USN journal plus a registry snapshot, no kernel driver) so the program can later be removed exactly |
| **Avel Force** | Removes programs whose uninstaller is broken or missing; a crosshair "hunter" picks a program from any window |
| **Avel Tools** | Startup manager, Windows apps (bloatware) removal, Windows features, stubborn browser extensions |

Planned design principles:
- **Safety first.** Every change goes through a single change set: preview, backup, execute, journal. Anything can be undone.
- **Explain why.** Every leftover shows the evidence it was matched on and a confidence level.
- **Fast.** Native C++, background scanning, results streamed to the UI, and MFT/USN-based file enumeration.
- **Three UI languages:** English, Русский, Հայերեն.

## Building

Requirements:
- Visual Studio 2022 or 2026 with the **Desktop development with C++** workload. It includes MSVC, the Windows 11 SDK, CMake, Ninja and vcpkg.
- Qt 6.12, **MSVC 2022 64-bit** kit, from the Qt Online Installer or [aqtinstall](https://github.com/miurahr/aqtinstall).

```powershell
. .\scripts\dev-env.ps1            # VS dev shell + VCPKG_ROOT + QT_ROOT_DIR (use -QtDir if Qt is elsewhere)
cmake --preset release             # vcpkg fetches dependencies on first run
cmake --build --preset release
ctest --preset release
```

The binaries end up in `build\release\bin\`: `avel.exe` (GUI, runs as administrator) and `avel-cli.exe`.
To make that folder self-contained (Qt DLLs and the MSVC runtime, about 30 MB), run `cmake --build --preset release --target deploy`.
To build without Qt (engine, CLI and tests only), add `-DAVEL_BUILD_APP=OFF` to the configure step.

### Translations

UI strings are wrapped in `tr()` and translated with Qt Linguist. After changing UI text, refresh the catalogs:

```powershell
cmake --build --preset release --target update_translations
linguist src\app\i18n\avel_ru.ts   # or avel_hy.ts
```

## Repository layout

```
src/core/     avel_core: Qt-free engine (Win32, C++20); returns codes, never UI text
src/cli/      avel-cli.exe: scripting front end (English, JSON output)
src/app/      avel.exe: Qt Widgets GUI and translations (src/app/i18n)
tests/unit/   GoogleTest unit tests (never modify the system; registry tests use a volatile HKCU key)
tests/sandbox/ Windows Sandbox harness for destructive end-to-end tests
scripts/      Developer environment and verification scripts (e.g. Compare-SettingsApps.ps1)
docs/         Plan, design notes, research notes and architecture decision records
```

## Code style

- Core (`src/core`): `snake_case` functions and variables, `PascalCase` types, `avel::<module>` namespaces.
- GUI (`src/app`): Qt conventions (`camelCase` methods) in `avel::app`.
- Everywhere: members end with `_`, comments and docs are in English, and `clang-format` is configured in `.clang-format`.

## Safety

Avel deletes software, files and registry keys.
- Uninstalling runs each program's own uninstaller, after a System Restore point.
- Leftovers are removed only after a backup, and only outside protected places: Windows, Program
  Files, the user profiles and the known folders, and the core registry keys. Every removal can be
  undone for 30 days.
- Development tests of destructive features run only inside Windows Sandbox (`tests/sandbox`).

## License

[Apache License 2.0](LICENSE). See [NOTICE](NOTICE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Qt is used under the LGPLv3.
