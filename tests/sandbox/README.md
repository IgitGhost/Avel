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

The `uninstall` scenario needs the sample installers. Download them once (and build the test
MSIX) with `Get-SampleInstallers.ps1`; they land in `installers/`, which git ignores:

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
