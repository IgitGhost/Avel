# Uninstall: how Avel removes programs

The uninstall engine lives in `src/core/uninstall` and, like the rest of `avel_core`, has no Qt
dependency and returns codes rather than text. The GUI and `avel-cli uninstall` are thin layers
on top of it.

```
Program ──make_uninstall_plan()──▶ UninstallPlan ──uninstall()──▶ UninstallResult
                                        │                              ▲
                     run_batch(): restore point, then one by one ──────┘
```

## Planning (`uninstall_plan.cpp`)

`make_uninstall_plan(program, mode, context)` decides how a program will be removed, without
running anything. It returns one of three methods, or a reason why it can't proceed:

| Method | Used for | Runs |
|---|---|---|
| `msi` | Windows Installer products: `WindowsInstaller=1`, MSI-only products, `msiexec` commands | `msiexec.exe /x {ProductCode}`, plus `/qn /norestart` in quiet mode |
| `command` | Every other desktop program | The registered uninstall command, plus the technology's silent switch in quiet mode |
| `package` | Packaged (Store/MSIX) apps | `PackageManager.RemovePackageAsync` for the current user, through a ChangeSet when that can be undone (see "Packaged apps") |

Interactive MSI removal uses `/x`, not the registered `/I`. `/I` would open the repair/remove
maintenance dialog instead of removing the product.

**Silent switches.** The vendor's own `QuietUninstallString` always wins. Otherwise Avel adds the
technology's switch:

| Technology | Silent switch | How it's recognized |
|---|---|---|
| Inno Setup | `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART` | `unins###.exe`, "Inno Setup:" values, or the `JR.Inno.Setup` manifest |
| NSIS | `/S`, placed before `_?=` (which must come last) | The `Nullsoft.NSIS` manifest, the `NullsoftInst` data signature, or a `_?=` argument |
| WiX Burn | `/quiet /norestart` | Package Cache path, `BundleCachePath` / `BundleProviderKey` |
| Squirrel | `-s` | `Update.exe --uninstall` |
| SdbInst | `-q` | `sdbinst … .sdb` |

When the inventory couldn't tell the technology from the registry, `detect_installer_type()`
examines the uninstaller executable. It reads the embedded manifest as a data file, so nothing is
run, and then looks for the NSIS data signature.

If no silent switch is known (InstallShield without a response file, custom uninstallers,
Chromium-based browsers and so on), quiet mode falls back to the uninstaller's own window and the
plan says so.

On the development machine, 161 of the 199 programs listed in Settings get a silent plan:
- 81 packages
- 45 MSI
- 20 WiX Burn
- 7 NSIS
- 6 Inno Setup
- 1 each for SdbInst, Squirrel and a vendor command

Run `InventorySmokeTest.PlansTheRemovalOfEveryListedProgram` to see this breakdown on another machine.

**Reasons not to proceed** (`PlanError`):

| Error | When |
| `removal-blocked` | `NoRemove=1` (Microsoft Edge, for example): the installer forbids it |
| `protected-package` | A packaged app Windows needs or that belongs to a desktop program, as the Windows apps tool protects it (see "Packaged apps") |
| `other-user` | A per-user install of another account: its uninstaller would act on Avel's user profile instead |
| `elevation-required` | A machine-wide program when Avel isn't elevated. Only the CLI can run unelevated. |
| `no-uninstaller` | No uninstall command |
| `unsupported-command` | The command is a URL (such as `steam://`), not a program |

## Running (`process_runner.cpp`, `uninstaller.cpp`)

**The process tree.**
- The uninstaller starts suspended inside a new **job object** and is resumed once assigned.
  This way none of its child processes escape.
- Avel waits until **every process in the job** has exited, not just the first one. NSIS
  uninstallers copy themselves to `%TEMP%\~nsu.tmp\Au_.exe` and exit at once, and Inno Setup hands
  off to a `_iu*.tmp` copy. Waiting for the first process would declare them done far too early.
- Job notifications aren't guaranteed to arrive, so the job's process count is also polled.
- Processes that explicitly ask to break away may do so; blocking them breaks some installers.
- The job doesn't kill anything if Avel exits.
- When Avel's standard output or error goes somewhere other than a console (`avel-cli --json`
  redirected by a script), the uninstaller gets a console of its own. Windows gives a console
  program that shares its parent's console the parent's standard handles, so a console uninstaller
  would write into Avel's output: in the Sandbox, `sdbinst -u` put "Uninstallation of ...
  complete." ahead of the JSON. From a console, and from the GUI, nothing changes.

**Checking the result.**
- The uninstaller's exit code isn't trusted on its own. Avel checks whether the program is still
  *registered*: its Uninstall key, its Windows Installer product (for the machine or the current
  user), or its package.
- It polls for up to three seconds, because some uninstallers drop the key a moment after exiting.
- The outcome is one of:
  - `removed`
  - `reboot-required` (exit code 3010 or 1641)
  - `still-installed` (the user cancelled, or the uninstaller failed)
  - `cancelled`
  - `failed` (couldn't start, or the package manager reported an error)
  - `not-possible`
  - `simulated`
- If the install folder still exists, the result notes it. Phase 3 deals with leftovers.

**Windows Installer** runs one installation at a time. If `msiexec` reports another one in
progress (1618), Avel retries every 5 seconds for up to 5 minutes.

**Cancelling.** "Stop" ends the batch after the running uninstaller. "Terminate" also kills the
running uninstaller's whole job. Ctrl+C in the CLI does the first; pressed twice, it does the second.

## Batches and restore points (`batch.cpp`, `restore_point.cpp`)

**Order.** `run_batch()` uninstalls strictly one program after another. Uninstallers, Windows
Installer ones in particular, can't safely run side by side.

**Restore point.**
- Before the first program, the batch creates one "application uninstall" System Restore point
  with `SRSetRestorePointW`.
- It's completed afterwards, or discarded if nothing ran.
- The process sets COM security at startup (`platform::initialize_com_security()`), as the System
  Restore documentation requires.
- If the restore point can't be created, the GUI asks whether to continue. The usual reason is
  that System Protection is off; Windows also creates at most one such point a day by default. The
  CLI warns and continues.

Phase 3 adds Avel's own backup and undo, which don't depend on System Restore.

## Packaged apps (`batch.cpp`)

**Refused packages.** `make_uninstall_plan` refuses (`protected-package`) the packaged apps the
Windows apps tool protects ([tools.md](tools.md), "Windows apps", "Protection"), so the program
list agrees with it:
- System-signed ones (the inventory's `no_remove`);
- the families on the critical list (`tools::app_category`: the Store, App Installer, Windows
  Security...), most of which aren't System-signed;
- a package with an external location (`Program::external_location`): a sparse package that gives
  a desktop program its identity, such as Notepad++'s context-menu package or PowerToys'. It goes
  when that program is uninstalled, as its leftover (`related-packages`, which is why the
  inventory doesn't mark it `no_remove`: that scan skips `no_remove` packages).

Removing a packaged app for the current user can be undone only while the package stays on this
PC: Windows deletes it the moment its last user's registration goes, unless it is provisioned
([ADR 0008](adr/0008-windows-apps-and-features.md)). So the batch removes a packaged app in one of
two ways (the maintainer's decision of 2026-11-14):

- **Softly, through a ChangeSet**, when it can be undone: the current user has the family, and
  another user has it too or it is provisioned for new users (`soft_removal_plan`, on
  `PackageFacts` read through `platform::family_users` and `platform::provisioned_package`). The
  batch builds a one-item plan, a `PackageUserLocation` from registered to not for the current
  user, exactly what the Windows apps tool's "Remove for me" plans ([tools.md](tools.md), "Windows
  apps"), and hands it to the front end (`BatchOptions::run_change_set`), which runs it in its
  change store under the store's lock, with the plan's description set ("Uninstall Contoso
  News"). The result keeps the set's id (`UninstallResult::change_set`): undo registers the app
  for the user again (its data isn't kept). An item that is `done` makes the outcome `removed`; a
  set that couldn't be written (another Avel process holds the store) `failed`, with nothing
  changed; anything else `failed` or `still-installed` while the package is still registered.
- **As before** otherwise: `RemovePackageAsync` for the current user, which Windows can't undo.
  That is the case when nobody else would keep the package, and whenever the front end has no
  change store to give: a dry run, or `avel-cli` without administrator rights (who else has the
  family and the provisioning can't be read then anyway).

The facts are read right before the app's turn in the batch, and the executor checks again when
the set runs (compare-and-set; `skipped (not-undoable)` if the package would no longer stay).

- **CLI:** `avel-cli uninstall`, elevated, opens the change store when a packaged app is in the
  batch (settling interrupted sets, as every changing command does; exit 1 when it can't be
  trusted). The outcome line ends with "for you only; undo with: avel-cli undo <set>", and
  `--json` reports `results[].change_set` (null for every other uninstall).
- **GUI:** the uninstall progress dialog runs the set on its worker, with exclusive hive access and
  then the store's lock, as every ChangeSet in avel.exe; the program's status reads "Removed for
  you; can be undone in the Undo Center", where the set is listed like any other.

## Front ends

- **GUI:**
  - Select programs, then press **Uninstall** (Del), or use the context menu.
  - The confirmation dialog shows how each program will be removed. Options: silent mode (the
    default for several programs), a restore point, and **Simulate**, which runs the whole flow
    without changing anything. A program installed with Avel Trace adds **Use the install trace
    (n items)**, on by default ([trace.md](trace.md), "Uninstalling by the trace").
  - The progress dialog lists each program's outcome. The program list refreshes afterwards.
- **CLI:**

  ```powershell
  avel-cli uninstall <id or exact name>... [--quiet] [--dry-run] [--yes] [--no-restore-point] [--json]
  ```

  - The exit code is 0 when every program was removed (or would be, with `--dry-run`).
  - `--json` reports the plan and outcome of each program, and the ChangeSet of a packaged app
    removed softly (`change_set`).

## Testing

**Unit tests** cover the logic:
- planning for every technology and every refusal reason;
- exit-code classification and registration lookups;
- dry-run batches and cancellation;
- whether a packaged app is removed softly (on injected facts), and a batch running its ChangeSet
  through a fake runner;
- installer detection (manifest and signature);
- the job-object runner.

The runner tests use a helper program that starts a child and exits at once, as NSIS does. They
check that Avel waits for the child, and that terminating kills the whole tree.

**End to end**, programs are uninstalled only in Windows Sandbox
(`tests/sandbox`, scenario `uninstall`). It runs three rounds, each followed by a check of the
inventory:
1. Samples of MSI, NSIS, Inno Setup, WiX Burn and MSIX are installed and removed interactively,
   one at a time. A UI Automation helper (`scenarios/clicker.ps1`) clicks through the
   uninstallers' dialogs.
2. They are reinstalled and removed in one quiet batch, with a restore point.
3. Two are removed through the GUI.

**Status (2026-10-19): RESULT: PASS.**

| Round | Status |
|---|---|
| Round 1 (interactive) | Passed: all five removed, 0.4 to 6 s each, with the clicker pressing each uninstaller's buttons. |
| Round 2 (quiet batch) | Passed: all five removed, about 1 s each, and the restore point was created. |
| Round 3 (GUI) | Passed. |

Notepad++'s uninstaller, silent or not, leaves its context-menu sparse package behind. The
scenario reports it as a known leftover for Phase 3 rather than a failure.
