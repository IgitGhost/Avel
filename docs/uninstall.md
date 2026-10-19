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
| `package` | Packaged (Store/MSIX) apps | `PackageManager.RemovePackageAsync` for the current user |

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
|---|---|
| `removal-blocked` | `NoRemove=1` (Microsoft Edge, for example) or a system package |
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

## Front ends

- **GUI:**
  - Select programs, then press **Uninstall** (Del), or use the context menu.
  - The confirmation dialog shows how each program will be removed. Options: silent mode (the
    default for several programs), a restore point, and **Simulate**, which runs the whole flow
    without changing anything.
  - The progress dialog lists each program's outcome. The program list refreshes afterwards.
- **CLI:**

  ```powershell
  avel-cli uninstall <id or exact name>... [--quiet] [--dry-run] [--yes] [--no-restore-point] [--json]
  ```

  - The exit code is 0 when every program was removed (or would be, with `--dry-run`).
  - `--json` reports the plan and outcome of each program.

## Testing

**Unit tests** cover the logic:
- planning for every technology and every refusal reason;
- exit-code classification and registration lookups;
- dry-run batches and cancellation;
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

**Status (2026-10-18):**

| Round | Status |
|---|---|
| Round 2 (quiet batch) | Passed: all five removed, about 1 s each, and the restore point was created. Notepad++ leaves its context-menu sparse package behind, a leftover for Phase 3. |
| Round 3 (GUI) | Passed. |
| Round 1 (interactive) | Open for MSI, NSIS, Inno and Burn: the clicker sees the uninstallers' windows but none of their buttons. The packaged app passed. |
