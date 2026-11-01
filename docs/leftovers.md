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
| Windows Installer clients | The upgrade codes and components that list a target's MSI product, read only for products still registered (see [force.md](force.md)) |
| Volume folders | With `--deep` only: every folder of each fixed NTFS volume, from its master file table ([ADR 0005](adr/0005-deep-search-from-the-mft.md)) |

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
| `deep-search` | With `--deep`: folders named after the program anywhere else on the fixed volumes (see [force.md](force.md)) | name match, outside-common-locations, company name |
| `orphans` | Not run for a program: `avel-cli orphans` lists folders no installed program owns (see [force.md](force.md)) | no-owner |
| `trace` | For a program with an install trace: what the installer created, still there, and the edits that take back its changes to `Path` and `SharedDLLs` (see "Install traces") | traced-install-root, traced-created, traced-value-undo |

## Confidence (`confidence.cpp`)

The score is the sum of the records' points: BCU's values, plus fourteen of Avel's own (marked *).

| Evidence | Points |
|---|---|
| uninstaller-key, installer-registration * | +20 |
| startup-matched * | +6 |
| traced-install-root * | +5 |
| explicit-connection, path-reference *, company-name-match, all-subdirs-matched | +4 |
| traced-value-undo * | +3 |
| product-name-perfect-match | +2 |
| traced-created *, traced-value-restore * | +1 |
| no-owner *, excluded-in-trace-review *, holds-excluded-in-trace-review * | 0 |
| depth(n) | 2 − 2·n |
| directly-inside-known-folder | −1 |
| product-name-dodgy-match, item-name-equals-company-name, used-by-similar-named-app, other-user-data *, outside-common-locations * | −2 |
| questionable-directory-name | −3 |
| container-holds-more * | −4 |
| startup-is-run-once * | −5 |
| directory-still-used, shared-dll-still-used * | −7 |
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

**Excluded in a trace's review.** An item the user excluded in the program's install trace's
review, that another scanner finds all the same, gets `excluded-in-trace-review` (0 points; see
"Install traces"). It changes neither the score nor the level, only the selection: such an item
is never selected automatically (`held_back`), by a preset or by `--min-level`. A folder or key
of the program's that still holds such an item gets `holds-excluded-in-trace-review` (0 points,
the item as its detail) and is held back too, whether or not a scanner offers the item itself.

## Protection (`protection.cpp`)

Every finding passes two independent layers before it is reported. Both are applied again by the
removal code in 3b, right before it acts.

- **HardFloor**, built from the live system and not replaceable by tests or settings:
  - the Windows folder, drive roots and volume system folders;
  - Program Files, ProgramData, `C:\Users`, every profile and its AppData folders, and every
    ancestor of them;
  - WindowsApps, Package Cache, packaged apps' data (`AppData\Local\Packages`, and the machine's
    `ProgramData\Packages`), OneDrive folders and Avel's own data;
  - the SYSTEM hive (services and firewall rules are their own item kinds), the top-level keys,
    the core keys (Run, Uninstall, App Paths, CLSID, Windows Installer's containers, ...) with
    their ancestors, and Avel's key;
  - the Task Scheduler's `TaskCache`, keys and values alike: tasks and task folders go through the
    Task Scheduler as their own kinds;
  - what Windows keeps for registered packages (`package-registration`), keys and values alike,
    with their ancestors: `Classes\PackagedCom`, `Classes\ActivatableClasses`,
    `Classes\Extensions\ContractId`, the `AppModel` and `AppContainer` keys under
    `Classes\Local Settings`, `AppModel`, `Appx`, `AppHost\IndexedDB` and
    `BackgroundAccessApplications` under `Microsoft\Windows\CurrentVersion`, and
    `SecurityManager\CapAuthz`, in the machine's hive and every user's; and a package's ProgIDs
    (`Classes\AppX` and 32 letters and digits), with the values that name one (a file type's
    `OpenWithProgids`). Windows writes them when it registers a package, also a sparse package an
    installer registers (Notepad++'s context menu), and removes them with it: a package goes only
    through `PackageLocation`;
  - Windows' own scheduled tasks (`\Microsoft\...`), and the root and `\Microsoft` task folders.
  - **Value edits** are refused everywhere except in a narrow allow-list, the floor's only one: the
    machine's `Session Manager\Environment`, each user's `Environment`, and both `SharedDLLs` keys
    (64-bit and `WOW6432Node`). There, counts may only go down or be deleted, and variables
    Windows relies on (`ComSpec`, `windir`, `TEMP`, `ProgramFiles`, ...) are never edited. Windows'
    own lists (`Path`, `PATHEXT`, `PSModulePath`) may only lose entries, or be deleted in a user's
    environment when the installer created them; no list is ever restored whole. Every other kind
    stays refused in these keys as before.
  - **State changes** (Avel Tools, [ADR 0007](adr/0007-changeset-state-changes.md)) have their
    own rules: a startup approval only in Explorer's `StartupApproved\Run`, `Run32` and
    `StartupFolder` keys and only as Task Manager writes it (`not-editable` otherwise); never the
    state of Windows' own tasks; never the start type of a driver or of a service whose image (a
    `svchost.exe` service's `ServiceDll`) Microsoft signed, nor to boot or system start
    (`windows-service`).
- **ProtectedPolicy**: the known folders (and their ancestors) of the machine and every scanned
  user, and BCU's blacklists of shared folder and key names (`Common Files`, `Microsoft`,
  `Windows`, ...). A folder name also counts without its extension (`Microsoft.NET`).

`LeftoverSmokeTest` checks every finding for every program on the machine against a third,
independent oracle: every known folder Windows reports (`IKnownFolderManager`), every profile,
the Windows folder and drive roots, and the core registry keys. The Sandbox scenario does the
same with .NET's special folders.

## Value edits (`value_edits.cpp`)

Some values are shared by many programs: `Path` and the other environment variables, and the
`SharedDLLs` reference counts. An installer adds to them rather than creating them, so removing
its traces means editing the value, not deleting it. A **`ValueEditLocation`** describes such an
edit: the key and value, the kind (`remove-path-entries`, `decrement-shared-dll`, `delete-value`,
`restore-value`), the value it expects and the value it writes (both raw, absent meaning no
value), the entries it takes out and the count it takes off. Its identity includes what it does,
so two traces' edits to one value stay two items.

`plan_value_edit(change, live)` is pure. It takes what a trace saved about the value (its exact
bytes before and after the install, `TracedValue`) and the value as it is now:

| Change | Planned edit | Basis |
|---|---|---|
| Entries added to a list (`Path`, `PATHEXT`, or any value the installer only added `;`-separated entries to) | Those entries taken out of the live value; everything else stays byte for byte | `undo` |
| A variable the installer created, unchanged since | Deleted | `undo` |
| A list it created, with entries added since | Its entries taken out; deleted if none are left | `undo` |
| Another variable it changed or deleted, unchanged since | Its old value restored | `restore` (Questionable) |
| A SharedDLLs count it raised | Lowered by its increment (never below 0); deleted at 0 if the installer created it | `undo` |

- **Lists.** Entries match ignoring case, surrounding quotes and spaces, and a trailing backslash
  (but `C:\` isn't `C:`). The edit keeps the live type (a `setx` rewrite to `REG_SZ` stays), the
  terminator, the user's later entries and order, and empty entries. An entry that was there
  before the install stays: of two equal entries, the one written as the installer wrote it goes,
  the last one for an appended entry. Entries the installer took out stay out (an upgrade that
  replaced its old entry), but a list it replaced whole is never left empty.
- **Nothing is planned** (`already-undone`, `changed-since`, `protected-variable`, `unsupported`)
  when the installer's entries or reference are gone already, when undoing would lose a later
  change (a created or changed variable that changed again), for the variables Windows relies on,
  and for a count the installer didn't raise.
- **Confidence** is the caller's: `undo` takes back exactly what the installer did, `restore` puts
  back a value the user may not want any more, so it is never checked by default. The `trace`
  scanner gives them `traced-value-undo` and `traced-value-restore` (see "Install traces").

The executor applies an edit only while the value is still what it expects (compare-and-set, see
[changes.md](changes.md)).

## Install traces (`trace_scanner.cpp`)

A program installed with Avel Trace ([trace.md](trace.md)) has a record of what its installer
did. The leftover scan uses it through an interface, `TraceSource` (`trace_source.h`), so
`leftovers` never depends on the tracker; `tracker::StoreTraceSource` reads the trace store, and
tests fake it. With no source, or no trace, the scan is exactly what it was.

- **The target.** `TraceSource::add_trace` fills in the target's `trace_id`, its `traced` items
  (the trace's changes that aren't noise and that its review doesn't exclude: location, category,
  how it changed, and whether it is tied to the program, with the tracker's reason; see
  [trace.md](trace.md), "The review") and its `traced_values` (the values it changed
  in the environment and `SharedDLLs`, exactly), each also as it was then (`captured_values`),
  and what the review excludes (`trace_excluded`: the excluded locations, and each excluded
  value as its registry value). Tying reads the live registry and Task Scheduler, so the target
  is captured before the uninstaller runs: `capture_targets(programs, source)`.
- **What the review excludes** never comes from the trace, but other scanners may find it by
  name or reference. After merging, an item of the target's whose identity is in its
  `trace_excluded` gets `excluded-in-trace-review`, the trace's id as its detail. It keeps its
  level and is listed, but `held_back` marks it, and every item that contains it
  (`Leftover::inside`, since removing a container takes its contents along), as never selected
  automatically: no preset of the review checks them, and `changes::make_plan` (the CLI's
  `--remove-leftovers` and `force` by `--min-level`) leaves them out. The user can still check
  them by hand. Only the program whose trace excluded the item is concerned; another program's
  match of it stands. With the trace left out (`--no-trace`, "Use the install trace" off), or not
  used because its review can't be read, nothing is marked.
- **What holds an excluded item.** A trace lists only the outermost item it saw created, so an
  excluded item is never inside a container the same trace offers. But the folder or key it was
  created in may have existed before the install (the publisher's folder, an older version's
  install folder or key), and other scanners offer that whole. No scanner may offer the excluded
  item itself, and then `Leftover::inside` can't tell, so an item of the target's that holds one
  of its `trace_excluded` still there (`leftovers::contains`) gets
  `holds-excluded-in-trace-review`, the excluded item as its detail, and `held_back` marks it too.
  An excluded item that is gone holds nothing back.
- **The scanner** runs last in the scan's loop, so an item other scanners found too keeps their
  scanner (and its group in the GUI) and merges, and protection and containment apply as to any
  item:
  - only `created` items are offered; deleted, modified and renamed items existed before the
    install and are never offered (they are counted as `shown_only`);
  - an item that no longer exists (the uninstaller removed it), or is a file where the trace saw a
    folder, is left out (`gone`). Services, tasks and task folders are looked up in the registry
    (`Services`, the Task Scheduler's `TaskCache\Tree`), firewall rules by their value;
  - firewall rules are reported and never removed, and drivers only shown, as from any scanner.
- **Created folders and keys** may since hold what other programs or the user put in them (another
  program's handler in a `DragDropHandlers` key the installer created, files in a folder it
  created). The trace keeps what each created folder and key held when it stopped
  (`TracedItem::contents`, and by name in `inside`, up to 10,000 items). One goes whole only while
  it holds no more than that, read again at scan time:
  - holding more, it isn't offered; the items the trace saw in it that are still there are offered
    in its place, with its evidence: a folder or subkey whole where nothing was added to it, else
    what the trace saw in it, down to single files and values. Counted as `split`, and the items
    offered for it as `split_into`;
  - with only the count kept (more than 10,000 items, part of it unreadable then, or a trace from
    before 5b), or when it can't be read now, it is offered whole with `container-holds-more`
    (−4): tied, Questionable; untied, Bad. Merged with another scanner's evidence, it takes 4 off
    that too.

  Gaps: a value the installer wrote and another program rewrote since (same name) still counts as
  the installer's; a folder or key someone emptied and refilled with as many items of the same
  names is taken as unchanged. A created folder holding items older than the trace (moved in) was
  never offered whole: its new items are listed one by one (see trace.md).
- **Evidence.** Only tied items, the installer's own folders and keys, get the strongest evidence,
  so nothing else the trace saw is checked by the default preset (Moderate: Good and above):

  | Item | Evidence | Level alone |
  |---|---|---|
  | Tied | `traced-install-root` (+5), with the tie's reason as its detail (`install-folder`, `references-tied`, ...) | VeryGood |
  | Untied | `traced-created` (+1) | Questionable |
  | Untied, named like the program (a perfect name match, with or without its extension) | `traced-created` (+1), `product-name-perfect-match` (+2) | Good |
  | A folder another installed program lives in | `directory-still-used` (−7) | |
  | A file whose `SharedDLLs` count stays above 0 once the planned edits are made, or a folder holding one | `shared-dll-still-used` (−7), the DLL as its detail | |
  | In another user's profile or hive, for a machine-wide program | `other-user-data` (−2) | |
  | A created folder or key holding more than its trace's count of it | `container-holds-more` (−4) | |

- **Value edits** are planned once for all the scan's targets, before their items, so that one
  value gets one edit:
  - each traced value is read as it is now and planned with `plan_value_edit`. Several traces that
    changed one value (two programs that each appended to `Path`) make one edit: each is planned
    on what the one before it leaves, and the edit expects the live value and writes the last
    result. A second edit would fail compare-and-set. One trace that two targets share (a WiX
    bundle and its MSI packages) takes the value back once;
  - a `SharedDLLs` count that changed between the capture and the scan was dealt with by the
    uninstaller: it isn't lowered again (`already-undone`);
  - an `undo` edit gets `traced-value-undo` (+3, Good), and `path-reference` (+4, VeryGood) when
    what it names lies in the program's folder or a tied item: an entry it takes out of a list, the
    variable it deletes, the DLL whose count it lowers. A `restore` gets `traced-value-restore`
    (+1, Questionable). The other bases plan nothing; the item isn't offered;
  - a raw value that a traced value names (the `SharedDLLs` value that `path-references` offers to
    delete) goes: its evidence joins the edit, and with no edit planned, the trace says nothing of
    the installer's is left in it.
- **What it brought** is in `ScanResult::traces`, one `TraceUse` per traced target: the trace's id,
  the reported items with its evidence (and how many are tied), its value edits, what was gone,
  shown only or protected, the created containers offered as what it saw in them, the reported
  items its review excludes (`excluded`), and each traced value's basis.

## Merging and containment (`merge.cpp`)

Items found more than once (by several scanners, or for several programs in one batch) are
merged: their records are joined, each piece of evidence counted once, and so are their owners.
The results are then sorted as a tree, and each item points at the nearest other item that
contains it (`Leftover::inside`). Removal will skip items whose container is removed too.

## After an uninstall (`post_uninstall.cpp`)

The CLI and the GUI share one path:
1. Before the batch runs, `capture_targets()` records each program's scan target, and with a
   `TraceSource`, its install trace, tied to it while it is still installed. The CLI and the GUI
   pass one unless the traces are turned off (`--no-trace`, "Use the install trace").
2. After the batch, `scan_after_uninstall()` scans the programs that were removed (or will be
   once Windows restarts), against a fresh inventory.

Nothing is scanned after a simulated run.

## Front ends

**CLI.**
- `avel-cli scan <id|name>... | --all [--json] [--min-level questionable|good|very-good] [--all-levels] [--timing] [--no-trace]`
  previews what each program would leave behind if its uninstaller removed nothing. It's useful
  to check the matching on a live system. The GUI's Preview leftovers does the same. Run as
  administrator, it adds what a program's install trace recorded (`--no-trace` leaves it out);
  unelevated it can't read the trace store, and the scan is as before.
- `avel-cli uninstall` scans after the batch and prints what was left behind, a program's install
  trace included (`--no-trace`: without it). JSON output gains a `leftover_scan` object (every
  level). `--no-leftover-scan` turns it off. With `--remove-leftovers` ([changes.md](changes.md))
  the items at or above `--min-level` go, but the held-back ones (`held_back`), which a line counts
  ("1 item at or above good is left in place: excluded in an install trace's review ..."); so
  does `force` ([force.md](force.md)).

The JSON lists each item with its category, location, confidence, score, evidence (kind,
points, reference, detail), owners, the scanner that found it, the index of its container and
`held_back` (never selected automatically: what a trace's review excludes, and what holds it),
plus the protected matches dropped, the time taken and diagnostics (`not-elevated`,
`user-hive-unavailable`). `traces` has one object per program whose install trace was used:
`program_id`, `trace_id`, `items` (the reported leftovers with the trace's evidence, value edits
aside), `tied`, `value_edits`, `gone`, `shown_only`, `protected`, `split` and `split_into` (created
folders and keys holding more than the trace saw, and the items offered in their place),
`excluded` (reported items its review excludes, which other scanners found), and `values` (`key`, `name`, and
the `basis` of each traced value: `undo`, `restore`, `already-undone`, `changed-since`,
`protected-variable` or `unsupported`). Value edits are items of kind `value-edit` (category
`registry`, found by `trace`).

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
  - Firewall rules and drivers ("Driver (shown only)") are shown without a checkbox (report
    only).
- **Presets** (remembered): *Safe* checks Very likely items only, *Moderate* (the default) Likely
  ones too, and *Advanced* Uncertain ones too. Unlikely items (Bad) are hidden unless
  "Show unlikely matches" is on, and never checked by a preset. Hidden items are never removed.
  No preset checks an item excluded in an install trace's review, or one that holds it
  (`held_back`), whatever its level; they can be checked by hand. The item's tooltip says why
  ("Excluded in the install trace's review: not checked automatically", or for a container "Holds
  an item excluded ...").
  The scan itself is the same for every preset; it takes about half a second.
- **Columns:**
  - the item;
  - its type;
  - the confidence, as a color and a label (Very likely, Likely, Uncertain, Unlikely);
  - *Why it matched*: the evidence for it, strongest first, then what speaks against it.
    Evidence worth no points is listed with what's for it ("No installed program owns it" for
    an orphan); only the depth is left to the tooltip. An exclusion in a trace's review comes
    first, since it is why the item isn't checked.

  The tooltip lists every record with its points and what matched (the referencing value, for
  instance), plus the score.
- **Diagnostics** (not elevated, a user's hive unavailable) show above the tree, and the count of
  protected matches left out below it. Ctrl+C copies the selected items' paths.
- **Preview leftovers** (File menu, or the program's context menu) runs the same scan for
  installed programs and shows the dialog read-only: what would remain if their uninstallers
  removed nothing. It works without administrator rights, with the `not-elevated` note.
- **Search the whole disk** (Review, Force and Preview modes) scans again with the deep search
  (see [force.md](force.md)), then starts the checks over from the preset.
- **Groups:** Windows Installer's registration of a forced MSI program has a group of its own,
  like COM classes.
- **Install traces** ([trace.md](trace.md), "Uninstalling by the trace"):
  - The uninstall's confirm dialog has **Use the install trace (n items)**, on, when a program has
    a trace. Preview leftovers and Force use the traces too.
  - Above the tree, one line per trace the scan used says what it brought (`TraceUse`): its items,
    how many are tied, its value edits, what was gone, protected or shown only, and how many items
    its review excludes the other scanners found ("not checked automatically").
  - **Use the install trace** ("Revert by the install trace" in Force mode, object name
    `useTraceBox`) is shown when the scan used a trace. Turning it off scans again without the
    traces (`LeftoverDialog::withoutTraces`), like `--no-trace`, and the checks start over.
  - **Environment and shared DLLs** holds the value edits, each described as what it does
    (`LeftoverText::valueEditText`), with the value and its text now and afterwards in the
    tooltip. They are checked by their confidence like any item, so a `restore` never is.
  - **Changed by the installer** lists what the traces saw the installer change, delete or move
    of what existed before: the targets' `traced` items that aren't `created`. Each is listed
    once, under the first program whose trace saw it, with "Shown only" for its confidence and how
    it changed. These rows have no checkboxes, aren't counted as leftovers, and are never planned.
  - The removal and the Undo Center show a value edit that was made as "Edited", and one whose
    value changed since the scan as "Skipped: the value has changed since" (`Reason::Changed`).
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
  - protection, merging and containment;
  - value edits (`value_edits_test.cpp`): entries appended, prepended, or there before; the user's
    later edits kept, or the entry already removed; `REG_EXPAND_SZ`, a `setx` rewrite, an
    unterminated value; trailing `;`, case, quotes and trailing `\`; created, changed and deleted
    variables; protected variables and Windows' lists; SharedDLLs increments and deletion at 0;
    putting entries back for undo;
  - install traces (`trace_scanner_test.cpp`, a fake `TraceSource` against the fixture's kind of
    scratch roots): the levels of tied and untied items and the name match, the SharedDLLs
    penalty, items gone and only created ones offered, protection of the Windows folder and a
    known folder, a package's own registration never offered, created keys and folders that hold
    more than the trace saw (offered as what it saw in them, or below Good with only a count),
    merging with the install-location scanner, value edits (undo, a path
    reference, restore at Questionable, one edit for two traces of one value and for one trace of
    two targets, a count the uninstaller lowered, the raw value an edit replaces), what the
    review excludes found by other scanners (`excluded-in-trace-review`, the level unchanged,
    left out of `make_plan` by level, another program's match untouched), a folder and a key
    that existed before holding an excluded file and value no scanner offers
    (`holds-excluded-in-trace-review`, held back; not when the item is gone or another
    program's), and
    `capture_targets` with a source;
  - `held_back` (`merge_test.cpp`): an excluded item and its containers, not its contents, left
    out of `make_plan`;
  - the GUI's tree (`tests/app/leftover_model_test.cpp`): the value edits' group, their words,
    tooltips and checks by confidence; "Changed by the installer" listed once per item, under the
    program whose trace saw it (also one the dialog didn't name), never checkable, counted or
    planned; the line for what a trace brought; "Edited" and `changed` in the removal's results;
    what a trace's review excluded, and what holds it, checked by no preset but by hand, with
    its reason first and in the tooltips.
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
