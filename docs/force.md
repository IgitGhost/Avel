# Avel Force: removing programs that can't uninstall themselves

Force removes programs whose uninstaller is missing or broken, and folders that no Uninstall
entry describes. It lives in `src/core/force`, with helpers in `src/core/platform`, and like the
rest of `avel_core` it has no Qt dependency and returns codes rather than text.

Force **never runs the program's uninstaller**: a program whose uninstaller works goes through
the normal Uninstall path. It does three things:
1. it ends the program's running processes, once the user agrees;
2. it scans for everything the program left on the system, as if its uninstaller had removed
   nothing (the same scan as [leftovers.md](leftovers.md), plus Windows Installer's
   registration);
3. it removes what the user approves as a ChangeSet (see [changes.md](changes.md)), so undo
   can put it back.

```
Program / --folder / --exe ──▶ ScanTarget ──▶ processes_in() ──▶ stop_process()  (if confirmed)
                                   │
                                   └──▶ scan_leftovers() ──▶ make_plan() ──▶ execute()
```

## Broken entries (`inventory`)

The inventory flags entries that can't uninstall themselves, and the uninstall plan refuses them
with a reason that points to Force:

| Flag | Set when | Plan error |
|---|---|---|
| `uninstaller_missing` | The file the UninstallString runs is gone, or the script or DLL its Windows host runs is gone (`powershell -File x.ps1`, `wscript`/`cscript x.vbs`, `rundll32 x.dll,Entry`). MSI products are exempt: msiexec removes them. | `uninstaller-missing` |
| `msi_package_missing` | Windows Installer can't find the product's package: neither its cached copy (`LocalPackage` in `C:\Windows\Installer`) nor the file it was installed from (`InstallSource` + `PackageName`). msiexec would fail with 1612. | `msi-package-missing` |
| `msi_product_unregistered` | (Phase 1) The entry claims an MSI product that Windows Installer doesn't know. | (none) |

`platform::missing_command_file` decides the first flag. Only "not found" on a fixed disk
counts: an unreadable path might still be there, and network shares and removable media aren't
probed at all, because that could take long or need a disc. On the development machine the
package check adds about 20 ms to the Windows Installer source (two more properties per product,
and a file check), and the whole inventory still takes about 120 ms, because the sources run in
parallel. None of its 597 entries has either flag.

`inventory::is_broken` is any of the three flags. `avel-cli list --broken` shows every broken entry,
whether or not Settings lists it, with its reasons; `no-install-folder` is added when the entry's
folder is gone too (a *dead* entry, whose Uninstall key is all that is left). There is no separate
cleanup: `avel-cli force <id>` removes a broken entry like any other program. On the development
machine one entry is broken: ROGFontInstaller, an MSI product Windows Installer doesn't know.

```
ROGFontInstaller
    reg:HKLM64:{605108C1-153E-43D8-8A67-7CE326B00ECA}
    msi-product-unregistered, no-install-folder
```

## Targets (`force.h`)

- **An installed program:** `leftovers::make_scan_target`, as for any leftover scan. Packaged
  apps are refused; the package manager removes them.
- **A folder (`folder_target`):** a pseudo-target whose program id is `folder:<path>`. Its
  name, the folder's own name by default, is what its other leftovers are matched by. The folder
  must exist, must not be a junction or symbolic link, and must pass both protection layers, so
  `C:\Program Files`, the Windows folder or a profile are refused.
- **A program file (`owner_of`):** the installed program whose install folder or uninstaller
  folder holds the file, the deepest folder winning. On a tie, the entry Settings lists beats a
  hidden component. Packaged apps match by their package folder. Protected folders never match,
  so an entry that records `C:\Program Files` as its location can't claim every program there.
  With no owner, the target is the file's folder. If the file's version resource names a
  product, it is the highest folder above the file named after that product, stopping below
  protected folders and never climbing into the publisher's folder. The GUI's Hunter (4c) uses
  this for a window's process.

## Processes (`platform/processes.h`)

- `running_processes()`: one Toolhelp snapshot, each process's image path from
  `QueryFullProcessImageNameW` (limited query rights), and `IsProcessCritical`.
- `force::processes_in()`: processes whose image lies in a target's install or uninstaller
  folder, unless that folder is protected. Left out: PID 0 and 4, Avel itself, critical
  processes, and service hosts (`service_process_ids()`). Services are stopped through the
  Service Control Manager when the executor removes them.
- `stop_process()`: posts `WM_CLOSE` to the process's visible, unowned top-level windows and
  waits up to 5 s. Then it calls `TerminateProcess` and waits again. It reports `closed`,
  `terminated`, `gone`, `access-denied` or `failed`.

Processes are ended only after the user agrees. In the CLI that means answering the prompt, or
passing `--end-processes`; `--yes` alone refuses to go on while any of them runs.

## Windows Installer's registration (`leftovers/msi_scanner.cpp`)

A product whose msiexec failed, or whose package is gone, stays registered: Windows Installer
still considers it installed, and a reinstall or an upgrade may refuse to run. For a target
with an MSI product code `{P}`, the scanner `msi-registration` finds, with the evidence
`installer-registration` (+20, like the Uninstall key):

| Where | Per machine | Per user |
|---|---|---|
| Product keys | `HKLM\SOFTWARE\Classes\Installer\{Products,Features}\<packed>` | `HKU\<sid>\Software\Microsoft\Installer\{Products,Features}\<packed>`, and `...\Installer\Managed\<sid>\Installer\{Products,Features}\<packed>` for a managed install |
| Install properties | `...\CurrentVersion\Installer\UserData\S-1-5-18\Products\<packed>` | `...\UserData\<sid>\Products\<packed>` |
| Upgrade codes | the product's value under `Classes\Installer\UpgradeCodes\<upgrade code>` | the same under the user's `Software\Microsoft\Installer\UpgradeCodes` and `Managed\<sid>\Installer\UpgradeCodes` |
| Components | the product's value under `UserData\S-1-5-18\Components\<component>` | the same under `UserData\<sid>\Components` |
| WiX Burn | `HKLM\SOFTWARE\Classes\Installer\Dependencies\{P}` | `HKU\<sid>\Software\Classes\Installer\Dependencies\{P}` |

- **Packed codes.** `<packed>` is Windows Installer's packed form of the code
  (`inventory/msi_codes.h`): `{23170F69-40C1-2702-2600-000001000000}` is
  `96F071321C0420726200000010000000`.
- **Shared keys.** An upgrade code or a component key whose only value is the product goes as
  a whole. One that other products share loses only the product's value. Patches are shared and
  left alone.
- **Scope.** A per-user product's registration is looked for in its owner's places only, so
  another user's registration of the same product is never touched.
- **The install folder.** Many MSI products record no `InstallLocation`; 7-Zip's MSI is one.
  Without a folder, the scanners that follow references into it (COM classes and their shell
  extensions, shortcuts, services, tasks, firewall rules) find nothing. The first Sandbox run
  left 7-Zip's context-menu handler behind this way. So when a target has no folder, the scan
  derives one from its components' key paths (`msi_install_folder`):
  1. each file's folder is grouped by its program folder, the highest one below a protected
     folder (`C:\Program Files\7-Zip` for `...\7-Zip\Lang\de.txt`);
  2. the largest group wins, and its deepest common folder is the candidate;
  3. the candidate must not be protected, must be named after the product, and must not be
     named after the publisher (whose folder may hold other programs).

  Registry key paths (`02:\SOFTWARE\...`), files run from the source (`C?\...`) and files in the
  Windows folder are ignored. `ScanResult::targets` returns the targets as scanned, so the CLI
  looks for running processes in the derived folder too.
- **Cost.** `UserData\S-1-5-18\Components` holds about 112,000 keys on the development machine.
  - They are read once per scan, and only for products whose registration is still there; after
    a normal uninstall that is none.
  - The read uses up to 8 threads, and asks each key for the products by name when there are
    few. It runs alongside the other snapshots.
  - Scanning one MSI program takes 0.7 s in all (0.5 s before Phase 4, 1.75 s with a serial
    read); the `installer` stage is about 0.55 s of it.
  - `scan --all` (the programs Settings lists) takes 2.6 s. `LeftoverSmokeTest`, which scans all
    597 entries, takes 12 s. The first version queried every product in every key and took 52 s
    for that test.
- **What stays.** The cached package in `C:\Windows\Installer` stays where it is. The Windows
  folder is under the hard floor, and the user chose registry-only cleanup.

**Protection.** Windows Installer's containers are core keys, protected with their ancestors:
`Classes\Installer\*`, `CurrentVersion\Installer\*`, `Installer\UserData\*\*`,
`Installer\Managed\*\Installer\*` and a user's `Software\Microsoft\Installer\*`. A `*` stands
for any one key, so `UserData\S-1-5-18\Products` is protected while one product's key below it
is not. Before 4a, no scanner reached these keys, but neither protection layer guarded them.

## Deep search (`leftovers/deep_scanner.cpp`)

`scan --deep`, `force --deep` and `uninstall --deep` (`ScanOptions::deep`) also look for folders
named after the program anywhere on the fixed NTFS volumes. The folders come from each volume's
master file table, read in one pass (`platform::VolumeFolders`; [ADR 0005](adr/0005-deep-search-from-the-mft.md)
explains why). It needs administrator rights; without them each volume is reported as
`deep-search-unavailable`.

- **Skipped:**
  - reparse points, system folders and the drive scan's blacklisted names;
  - folders at a volume's root that hold Windows' data or copies of other folders: names starting
    with `$` (the recycle bin, `$Avel.Quarantine`), `System Volume Information`, `Recovery`,
    `Windows.old`, `Config.Msi`;
  - the Windows folder, and the drive scan's roots (searched already);
  - the program's own folders (the install location scan's), and other programs' folders.
- **Evidence:** like the drive scan's, without depth, which has no root to count from:
  - `ProductNamePerfectMatch` (+2) or `ProductNameDodgyMatch` (-2);
  - `OutsideCommonLocations` (-2), new;
  - `CompanyNameMatch` (+4) when the parent is named after the publisher;
  - `ItemNameEqualsCompanyName`, `QuestionableDirectoryName`, `DirectoryStillUsed` and
    `OtherUserData` as in the drive scan.

  A folder named exactly like the program scores 0, Questionable, so it is shown but never
  pre-checked: `D:\Backup\Contoso Fixture` may be the user's own. Inside the publisher's folder
  (`D:\Contoso\Contoso Fixture`) it scores 4, Good. A resemblance alone is never reported.
- **Cost:** many folders share a name (`bin`, `en-US`), so names are matched, not folders: each
  distinct name once per program, and paths are rebuilt only for the matches.

## Orphaned folders (`leftovers/orphans.cpp`)

`avel-cli orphans` (`find_orphans`) lists the folders at the top of the places programs are
installed to that no installed program owns:
- both Program Files folders;
- ProgramData;
- each user's `AppData\Local\Programs` (`FOLDERID_UserProgramFiles`).

Users' AppData itself isn't searched. On the development machine it held about 130 unowned folders,
nearly all the data of programs that never register: games' saves, package managers' caches
(pip, npm, uv), portable tools. That's too many to review, and dangerous to offer.

A folder is **owned**, and left out, when:
- an installed program's install or uninstaller folder is it, lies in it or holds it;
- its name is like a program's (`match_product_name`), is a whole word of a program's name or
  publisher ("Code" for Microsoft Visual Studio Code), contains a program's name, or holds a
  publisher's name as a word ("Intel Package Cache" with Intel's drivers installed). Here the
  publisher loses its legal form ("Contoso" for "Contoso Ltd."; Ltd, LLC, GmbH, AG, Inc and the
  like), which BCU's `trim_publisher` doesn't do for "Ltd";
- a service, task, Run value, App Path, Windows Installer folder, COM class, shortcut or
  firewall rule points into it. MuiCache and SharedDLLs don't count: they outlive the programs
  they name;
- it is Windows' own: `Packages`, `ssh`, `USOShared`, `regid.*`, `Microsoft …`, `Windows …`, and
  the like.

The rest is reported as `Evidence::NoOwner` (0 points, Questionable), never pre-checked, with
`OtherUserData` added for another user's folder. Each item's program id is `folder:<path>`, like
a forced folder's, and the command is read-only: `force --folder <path>` removes one. On the
development machine it lists 22 folders in 0.6 s, such as `C:\Program Files (x86)\Razer`,
`C:\Program Files\EA Games` and `C:\ProgramData\FLEXnet`.

## Drivers

Services whose image lies in the program's folder were leftovers already. Drivers among them
(kernel, file system and recognizer drivers) are now reported too, as `ServiceLocation{driver}`
(kind `driver` in the CLI), but `changes::is_removable` refuses them. Removing a driver can leave
Windows unable to start, so they are shown for the user to deal with. The executor refused
drivers already (`backup_service` gives `protected`).

## The CLI

```
avel-cli force <program>... | --folder <path> [--name <name>] | --exe <path>
               [--dry-run] [--yes] [--end-processes] [--deep] [--min-level good|very-good]
               [--allow-irreversible] [--json]
avel-cli list --broken [--json]
avel-cli orphans [--all-levels] [--json]
avel-cli scan <program>... [--deep]
```

1. It resolves the targets and lists the processes running from their folders.
2. It scans, and prints the plan: what was found and what will be removed (Good and above by
   default).
3. It asks once ("End 1 process and remove 14 items?") and ends the processes.
4. It runs the ChangeSet, described as "Forced removal of X", which `avel-cli undo` and the
   Undo Center list like any other.
5. It exits 0 when every planned item was removed, 1 when something stayed or nothing was done,
   and 2 for a bad target (a protected folder, say).

The JSON output has `programs`, `processes` (`pid`, `name`, `image`, `outcome`),
`leftover_scan` and `leftover_removal`, the last two as in `uninstall --json`.

```
avel-cli force "Contoso Fixture" --dry-run
avel-cli force --exe "C:\Program Files\Contoso\app.exe" --end-processes
avel-cli force --folder "D:\Tools\Old thing" --name "Old Thing"
```

## Testing

- **Unit tests:**
  - `MsiCodesTest`;
  - `CommandLineTest.Missing*`: the executable and the hosts' scripts and DLLs;
  - `UninstallPlanTest.MissingUninstallersAndPackagesNeedForce`;
  - `HardFloorTest.GuardsWindowsInstallersContainersButNotOneProductsKeys`;
  - `MsiScannerTest`: a fake machine-wide and per-user registration in scratch keys, with
    shared and sole upgrade codes and components, and another user's copy that must stay; and
    the install folder derived from a component's key path;
  - `RegistryBackupTest.RoundTripRestoresTheTreeExactly` now has a key with an auto-inherited
    DACL;
  - `ForceTest`: folder targets and refusals, owners, processes, and ending a real helper
    process copied into a temporary program folder;
  - `VolumeFoldersTest`: paths rebuilt from made-up records, a loop, and (elevated only) a
    temporary folder found on its real volume;
  - `DeepScanTest`: a made-up volume with matches in and out of the publisher's folder, and
    every kind of folder the search skips;
  - `OrphansTest`: a temporary tree with a folder of each kind of owner, and orphans
    machine-wide, per user and another user's.
- **Windows Sandbox, scenario `force`** ([tests/sandbox/README.md](../tests/sandbox/README.md)):
  - Contoso Fixture with its uninstall script deleted and its app running;
  - 7-Zip's MSI with its package and Windows Installer's cached copy deleted;
  - a folder without an Uninstall entry, and refusals of protected folders;
  - a dead entry: `list --broken`, then `force` removing its key alone;
  - `C:\Tools\Contoso Fixture`, found by `scan --deep` only, and the elevated volume test;
  - `orphans`: a folder nobody owns is listed, the installed programs' and Windows' folders
    aren't.

  Each round checks the removal against `avel_state_dump` dumps, and that undo gives them back
  exactly.
