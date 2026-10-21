# Leftovers: how Avel finds what uninstallers leave behind

The leftover scan lives in `src/core/leftovers`. Like the rest of `avel_core`, it has no Qt
dependency and returns codes rather than text. In Phase 3a it only reads: it finds and explains
leftovers but removes nothing. Removal with backup and undo is Phase 3b (see `plan.md`).

```
Program ──make_scan_target()──▶ ScanTarget ─┐
                                            ├─▶ scan_leftovers() ──▶ ScanResult
ScanEnvironment ──take_snapshots()──────────┘     scanners → merge → protection → containment
```

Much of the matching is ported from Bulk Crap Uninstaller's `UninstallTools/Junk` (Apache-2.0,
credited in `NOTICE`; each ported file names its C# originals). BCU's weaknesses that Avel
fixes:
- BCU scans only the current user. Avel scans every user a program could have touched.
- BCU's "inside folder" checks are plain string prefixes. Avel's respect path boundaries.
- BCU misses services, scheduled tasks, shell extensions, firewall rules and sparse packages.

## Targets (`scan_target.cpp`)

`make_scan_target(program)` records what the scan needs about a program while it is still
installed: afterwards its Uninstall key is gone, and paths that no longer exist can't be
resolved (8.3 names, for instance). It records:
- the display name and publisher, trimmed as BCU does;
- the scope (machine or one user) and owner;
- the install folder as a normalized long path;
- the uninstaller and its folder, unless it is a Windows tool (msiexec, rundll32);
- the Uninstall key's address, and for packaged apps the package name.

When the entry records no install folder, the uninstaller's folder or the icon's folder stands in,
as long as it is named after the program (BCU's info adders do the same). A recorded folder that
is protected, or holds protected folders (an entry that claims `C:\Program Files`), is ignored:
otherwise everything inside it would look like the program's.

## Snapshots (`snapshots.cpp`, `system_snapshots.cpp`)

Each scan reads the system once, whatever the number of programs:

| Snapshot | What it holds |
|---|---|
| Folders | Levels 0–2 under Program Files (both), ProgramData, the Start Menu, and each user's Roaming, Local, LocalLow, Start Menu and VirtualStore. System, reparse-point and blacklisted folders are skipped |
| Registry keys | Levels 0–2 under `SOFTWARE` (both views), each user's `Software`, and VirtualStore, with the values that hold paths |
| Shortcuts | `.lnk` files in every Start Menu and desktop, resolved with `IShellLinkW` (raw targets, expanded for their owner) |
| References | Run/RunOnce, App Paths, SharedDLLs, `Installer\Folders`, MuiCache. A key that both registry views share (App Paths) is read once: its `WOW6432Node` path has the same kernel name |
| Services | Each service's `ImagePath` and `ServiceDll`, drivers included (they are reported, never removed) |
| Scheduled tasks | Exec actions of every task (hidden ones too), except Windows' own under `\Microsoft` |
| COM classes | `InprocServer32` and `LocalServer32` of every class (machine, 32-bit, each user), plus the shell extension handlers and `Shell Extensions\Approved` entries that name a class |
| Firewall rules | The `App=` path of each rule in the firewall policy |

## Scanners

Every scanner is a pure function of a target and the snapshots. Each finding carries **records**:
the pieces of evidence for and against it.

| Scanner | Finds | Main evidence |
|---|---|---|
| `install-location` | The install folder itself | explicit-connection |
| `common-drive` | Folders named after the program or its publisher | name match, depth, company name |
| `shortcuts` | Shortcuts into the install folder, or to the uninstaller | path-reference + name |
| `software-key` | Registry keys named after the program, or holding its folder | name match, explicit-connection |
| `uninstaller-key` | The Uninstall key, if it survived | uninstaller-key |
| `path-references` | Run values, App Paths, SharedDLLs, Installer folders, MuiCache entries pointing into the folder | startup-matched / path-reference |
| `related-packages` | Packages whose external location is inside the folder (sparse packages such as Notepad++'s context-menu package) | path-reference |
| `services` | Services running a file from the folder | startup-matched |
| `tasks` | Scheduled tasks running a file from the folder | startup-matched |
| `com-classes` | COM classes served from the folder, and the shell extension registrations naming them | path-reference |
| `firewall` | Firewall rules for programs in the folder (reported only, never removed) | path-reference |
| `msi-registration` | Windows Installer's registration of the program's MSI product code: product keys, install properties, upgrade codes and components (see [force.md](force.md)) | installer-registration |

## Confidence (`confidence.cpp`)

The score is the sum of the records' points: BCU's values, plus four of Avel's own (marked *).

| Evidence | Points |
|---|---|
| uninstaller-key | +20 |
| startup-matched * | +6 |
| explicit-connection, path-reference *, company-name-match, all-subdirs-matched | +4 |
| product-name-perfect-match | +2 |
| depth(n) | 2 − 2·n |
| directly-inside-known-folder | −1 |
| product-name-dodgy-match, item-name-equals-company-name, used-by-similar-named-app, other-user-data * | −2 |
| questionable-directory-name | −3 |
| startup-is-run-once * | −5 |
| directory-still-used | −7 |
| store-app | −10 |

| Score | Level |
|---|---|
| below 0 | Bad |
| 0–1 | Questionable |
| 2–4 | Good |
| 5 and up | VeryGood |

Names are matched with BCU's `MatchStringToProductName`: names of four characters or fewer never
match, otherwise the Sift4 distance decides, with a second try without the publisher's name. Its
outputs are checked against reference values from BCU's own code (`product_name_test.cpp`).

**Whose data.** A program installed for one user is only looked for in that user's profile and
hive. A program installed for everyone is looked for in every profile, but other users' items get
`other-user-data` (−2), so they stay unchecked unless there is explicit evidence.

**Still used.** A folder that another installed program lives in gets `directory-still-used` (−7).
After an uninstall, "installed" means a fresh inventory taken after the batch.

## Protection (`protection.cpp`)

Every finding passes two independent layers before it is reported. Both are applied again by the
removal code in 3b, right before it acts.

- **HardFloor**, built from the live system and not replaceable by tests or settings:
  - the Windows folder, drive roots and volume system folders;
  - Program Files, ProgramData, `C:\Users`, every profile and its AppData folders, and every
    ancestor of them;
  - WindowsApps, Package Cache, packaged apps' data (`AppData\Local\Packages`), OneDrive folders
    and Avel's own data;
  - the SYSTEM hive (services and firewall rules are their own item kinds), the top-level keys,
    the core keys (Run, Uninstall, App Paths, CLSID, Windows Installer's containers, ...) with
    their ancestors, and Avel's key;
  - Windows' own scheduled tasks (`\Microsoft\...`).
- **ProtectedPolicy**: the known folders (and their ancestors) of the machine and every scanned
  user, and BCU's blacklists of shared folder and key names (`Common Files`, `Microsoft`,
  `Windows`, ...). A folder name also counts without its extension (`Microsoft.NET`).

`LeftoverSmokeTest` checks every finding for every program on the machine against a third,
independent oracle: every known folder Windows reports (`IKnownFolderManager`), every profile,
the Windows folder and drive roots, and the core registry keys. The Sandbox scenario does the
same with .NET's special folders.

## Merging and containment (`merge.cpp`)

Items found more than once (by several scanners, or for several programs in one batch) are
merged: their records are joined, each piece of evidence counted once, and so are their owners.
The results are then sorted as a tree, and each item points at the nearest other item that
contains it (`Leftover::inside`). Removal will skip items whose container is removed too.

## After an uninstall (`post_uninstall.cpp`)

The CLI and the GUI share one path:
1. Before the batch runs, `capture_targets()` records each program's scan target.
2. After the batch, `scan_after_uninstall()` scans the programs that were removed (or will be
   once Windows restarts), against a fresh inventory.

Nothing is scanned after a simulated run.

## Front ends

**CLI.**
- `avel-cli scan <id|name>... | --all [--json] [--min-level questionable|good|very-good] [--all-levels] [--timing]`
  previews what each program would leave behind if its uninstaller removed nothing. It's useful
  to check the matching on a live system. The GUI's Preview leftovers does the same.
- `avel-cli uninstall` scans after the batch and prints what was left behind. JSON output gains
  a `leftover_scan` object (every level). `--no-leftover-scan` turns it off.

The JSON lists each item with its category, location, confidence, score, evidence (kind,
points, reference, detail), owners, the scanner that found it and the index of its container,
plus the protected matches dropped, the time taken and diagnostics (`not-elevated`,
`user-hive-unavailable`).

**GUI** (`src/app`).
- **The confirm dialog** has "Look for leftovers afterwards", on by default and remembered. It is
  disabled in Simulate mode, which never scans.
- **After the batch,** the progress dialog captures the targets before the uninstallers run and
  scans on its worker thread. When it finds anything at Questionable or above, it opens
  `LeftoverDialog` by itself, on top of the uninstall results. Its **Leftovers…** button opens the
  dialog again until something is removed.
- **`LeftoverModel`** is a tree: program, then group, then item.
  - The groups are the scan's categories, with COM classes and shell extensions (the
    `com-classes` scanner) apart from the rest of the registry. Java 8 alone registers thousands
    of them.
  - An item sits under the nearest shown item that contains it, whatever its group. A Start Menu
    folder holds its shortcuts, a key its values. A nested item shows only the part of its path
    below its container's.
  - Groups and containers with up to 50 items start expanded. Rows have uniform heights, so a
    tree of 16 000 items stays fast.
- **Checks** are tri-state.
  - Removing a folder or key takes everything inside it, so checking an item checks everything
    below it. Unchecking an item unchecks the items it is inside of. Their other contents stay
    checked and are still removed one by one.
  - Firewall rules are shown without a checkbox (report only).
- **Presets** (remembered): *Safe* checks Very likely items only, *Moderate* (the default) Likely
  ones too, and *Advanced* Uncertain ones too. Unlikely items (Bad) are hidden unless
  "Show unlikely matches" is on, and never checked by a preset. Hidden items are never removed.
  The scan itself is the same for every preset; it takes about half a second.
- **Columns:**
  - the item;
  - its type;
  - the confidence, as a color and a label (Very likely, Likely, Uncertain, Unlikely);
  - *Why it matched*: the evidence for it, strongest first, then what speaks against it.

  The tooltip lists every record with its points and what matched (the referencing value, for
  instance), plus the score.
- **Diagnostics** (not elevated, a user's hive unavailable) show above the tree, and the count of
  protected matches left out below it. Ctrl+C copies the selected items' paths.
- **Preview leftovers** (File menu, or the program's context menu) runs the same scan for
  installed programs and shows the dialog read-only: what would remain if their uninstallers
  removed nothing. It works without administrator rights, with the `not-elevated` note.
- **Translations:** `LeftoverText` holds the categories, kinds, confidence levels, evidence,
  reference kinds and diagnostics, translated to Russian and Armenian.

## Numbers from the development machine

`avel-cli scan --all` previews all 201 programs listed in Settings.
- **Results:** 15 921 items, of which 245 are VeryGood, 15 502 Good, 44 Questionable and 130 Bad.
  - About 13 000 of them are Java 8's COM classes, which it registered both machine-wide and for
    the user. They are real registrations, not false matches, so the GUI groups items rather
    than listing them flat.
  - The rest: 197 folders, 97 shortcuts, 10 services, 12 tasks and 45 firewall rules.
- **Speed:** 1.3 s for all of them.
  - The snapshots take about 0.5 s. The COM classes are read on a thread of their own, alongside
    the other snapshots.
  - Matching takes 0.75 s and protection 0.07 s.
  - A single program (as after an uninstall) takes about 0.5 s.
- **Safety:** the oracle finds zero hits across all 593 inventory entries (`LeftoverSmokeTest`).

7-Zip's preview shows what the BCU-style scanners alone would miss: six shell extension
registrations (`*`, `Directory`, `Folder` and `Drive` handlers), its COM class in both views and
the `Approved` entry.

## Testing

- **Unit tests:**
  - the confidence tables;
  - BCU's name matching, with reference vectors;
  - path handling (`C:\Foo` vs `C:\Foobar`, `\\?\`, command lines);
  - protection, merging and containment.
- **Fixture test (`leftover_scan_test.cpp`):** a fake program "Contoso Fixture" laid out in a
  temporary folder tree and volatile registry keys, standing in for the machine and a user. It
  has a service, a scheduled task, a shell extension, a firewall rule and decoys (shared folders,
  `Microsoft` subfolders, a driver, another program in the publisher's folder). The test checks
  each item's level against BCU's rules and that no decoy is reported.
- **Smoke test (`leftovers_smoke_test.cpp`):** scans every program on the machine (read-only)
  and applies the independent oracle.
- **Windows Sandbox (`tests/sandbox`, scenario `leftovers`):** installs the five samples,
  previews their leftovers, removes them in a quiet batch and checks the automatic scan.
  Notepad++'s sparse package must be reported, and nothing the oracle protects.

## Not covered yet

- TypeLib, Interface and AppID keys; signed-out users' `UsrClass.dat`.
- Prefetch, WER, UserAssist, AppCompat, tracing and event logs.
