# Windows Sandbox test harness

Destructive end-to-end tests (installing and uninstalling real programs, leftover removal,
forced removal, install tracing) run in [Windows Sandbox](https://learn.microsoft.com/windows/security/application-security/application-isolation/windows-sandbox/),
a disposable VM that is wiped when it closes.

## One-time setup

Windows Sandbox needs Windows 10/11 Pro or Enterprise. Enable it from an elevated PowerShell, then reboot:

```powershell
Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM
```

## Usage

The sandbox has neither Qt nor the MSVC runtime, so make the build self-contained first:

```powershell
cmake --build --preset release --target deploy
.\tests\sandbox\Start-AvelSandbox.ps1                  # maps build\release\bin
.\tests\sandbox\Start-AvelSandbox.ps1 -WaitForReport   # also waits for and prints the smoke-test report
.\tests\sandbox\Start-AvelSandbox.ps1 -Preset debug
```

Inside the sandbox:

| Path | Contents | Access |
|---|---|---|
| `C:\Avel\bin` | The build output, on `PATH` | Read-only |
| `C:\Avel\sandbox` | This folder | Read-only |
| `C:\Avel\results` | Reports from the scenarios; shows up on the host as `build\sandbox\results` | Writable |

Only one sandbox can run at a time; `-ReplaceRunning` stops a running one first, and
`-CloseWhenDone` (with `-WaitForReport`) closes it after the report arrives.

## Scenarios

`-Scenario <name>` runs `scenarios/<name>.ps1` at logon. Each writes `results\<name>.txt` last.

| Scenario | What it checks |
|---|---|
| `logon` (default) | `avel-cli version` runs and the GUI starts |
| `uninstall` | Phase 2 acceptance: installs one sample per installer technology and removes each interactively. `scenarios/clicker.ps1`, a separate PowerShell process, clicks through the uninstallers' dialogs in English or Russian. Then it reinstalls them, removes all five in one quiet batch with a System Restore point, and removes two through the GUI. Also writes `uninstall.json` (avel-cli's output), `uninstall.log` (progress while it runs) and `clicks.log` (every window the clicker saw, with its buttons and clicks) |
| `leftovers` | Phase 3a acceptance (the leftover scan only reads): installs the five samples and previews their leftovers with `avel-cli scan`, then removes them in one quiet batch and checks the scan that follows. Notepad++'s context-menu sparse package must be reported as a package leftover, and no reported item may be, or contain, a special folder, a profile or a drive root, lie in the Windows folder or be a SYSTEM key (an oracle built from .NET's special folders, independent of Avel's protection rules). Also writes `leftovers.json` and `leftovers.log` |
| `removal` | Phase 3b acceptance: installs three samples and `Contoso Fixture` (`scenarios/fixture.ps1`). The fixture's uninstaller leaves one leftover of every kind: folders, a shortcut, keys, a Run value, a service, a task, a COM class with its shell extension, and a firewall rule. The scenario also installs canaries named after it in `C:\Windows`, `ProgramData\Microsoft`, `AppData\Roaming\Microsoft` and `HKLM\SOFTWARE\Microsoft`, and a program of the same publisher that stays installed. It runs `uninstall --remove-leftovers --allow-irreversible`, then compares `avel_state_dump` dumps: every fixture leftover and Notepad++'s sparse package must be gone, and the rest unchanged. Then `undo --item` and `undo` must give back identical dumps. Then it reinstalls the fixture and kills the removal after 7 journal records (`AVEL_TEST_FAULT_AFTER`); the next run must settle the set, and undo must restore it exactly. Last, the GUI round (Phase 3c) reinstalls the fixture and uninstalls it through `avel.exe` with UI Automation: the leftover dialog must open by itself, Remove must take every fixture leftover away, and the Undo Center's Restore all must give back identical dumps. Also writes `removal.json` and `removal.log` |
| `force` | Phase 4 acceptance (Avel Force, which never runs the program's uninstaller). Round 1 installs `Contoso Fixture` and the canaries, deletes the fixture's uninstall script and starts its app. `uninstall` must refuse with `uninstaller-missing`, `force --exe` must find the program from its file, and `force --yes` without `--end-processes` must end and remove nothing. `force --yes --end-processes` must end the app and remove the program's folder, its Uninstall key and every leftover, leaving the canaries, the other program and the firewall rule alone; undo must give back identical dumps. Round 2 installs 7-Zip's MSI from a copy, then deletes the copy and Windows Installer's cached package. `uninstall` must refuse with `msi-package-missing`, `force` must remove the program with Windows Installer's registration (product keys, install properties, upgrade code, components) until Windows Installer no longer knows the product, and undo must give the registration back exactly. Round 3 forces a folder no Uninstall entry describes, which must go alone, and `force --folder` must refuse Program Files, `System32` and `C:\Users`. Round 4 adds a dead entry (an Uninstall key whose uninstaller and folder are gone): `list --broken` must show it with its reasons, and `force` must remove the key alone, which undo gives back. Round 5 runs the volume reading unit tests elevated, then `C:\Tools\Contoso Fixture` must be found by `scan --deep` only, at questionable. Round 6: `orphans` must list a folder nobody owns, and none of the installed programs' folders, a folder named after an installed program's publisher, or Windows' folders. Round 7 (Phase 4c) starts the fixture's app again and drives `avel.exe` with UI Automation. The uninstall dialog must offer "Force uninstall…". After "Search the whole disk", the forced-removal review must list `C:\Tools\Contoso Fixture`. Remove must ask to end the app and end it, and the program and its leftovers must go while the unchecked deep-search folder stays. The Undo Center's Restore all must then give back identical dumps, and Tools > Find orphaned folders must list the folder nobody owns. Also writes `force.json` and `force.log` |

**Don't keep files in `build\sandbox\results` open on the host while a scenario runs.** A host
process that holds one open, such as `tail -f`, makes the sandbox's appends to it fail ("being
used by another process"). The scenarios cope: the report is assembled in memory and written at
the end. The progress logs lose their lines, though.

**The clicker** finds classic buttons (msiexec, NSIS, Inno's `TNewButton`, WiX Burn, message
boxes and task dialogs) as child windows with `EnumChildWindows`, and presses them by posting
`WM_COMMAND` to their parent. Managed UI Automation, which PowerShell offers, reports these
buttons as panes without an Invoke pattern, so it's only the fallback for buttons that have no
window of their own. To try the clicker on a dev machine, pass `-WindowTitle <regex>`: it then
touches only matching windows.

**The GUI rounds** (`removal` and `force`) drive `avel.exe` in English with the UI Automation
helpers in `scenarios/gui.ps1`. Qt's own widgets do expose their buttons, check boxes, menus and
tree rows. The helpers look elements up afresh each time, because Qt's provider invalidates
references when the UI changes.

The `uninstall`, `leftovers`, `removal` and `force` scenarios need the sample installers
(`scenarios/samples.ps1` lists them). Download them once (and build the test MSIX) with `Get-SampleInstallers.ps1`; they
land in `installers/`, which git ignores:

| Installer technology | Sample |
|---|---|
| MSI | 7-Zip (.msi) |
| NSIS | Notepad++ (latest release) |
| Inno Setup | Inno Setup (latest release) |
| WiX Burn bundle | Visual C++ Redistributable x64 |
| MSIX | Avel Test Package (unsigned, built locally with makeappx) |

```powershell
.\tests\sandbox\Get-SampleInstallers.ps1
.\tests\sandbox\Start-AvelSandbox.ps1 -Scenario uninstall -WaitForReport -TimeoutSeconds 1500 -CloseWhenDone
```
