# Changes: how Avel removes leftovers and undoes it

Removal lives in `src/core/changes`. Like the rest of `avel_core`, it has no Qt dependency and
returns codes rather than text. Every removal is a **ChangeSet**: a plan of items, each backed up
before it changes and recorded as it goes, so the set can be undone exactly and survives a crash.
How sets are stored is decided in [ADR 0004](adr/0004-changeset-folder-journal.md). Since Phase 6
a set also holds **state changes**, items turned off or on rather than removed
([ADR 0007](adr/0007-changeset-state-changes.md)).

```
ScanResult ──make_plan()──▶ ChangePlan ──execute()──▶ ChangeSet (store) ──restore()──▶ items back
                                                         ▲
                                         recover(): settles a set a crash interrupted
```

## The store (`store.cpp`)

The store's root is `%ProgramData%\Avel\Changes`. It holds one folder per set:
- `plan.json`
- `journal.jsonl`
- `summary.json`
- `items\NNNN\`: each item's backup
- `files\NNNN\`: quarantined files from the system volume

Files on other fixed NTFS volumes go to that volume's `X:\$Avel.Quarantine\<set>`, so every move
is a rename on one volume.

- **Trust.** The root and every quarantine root are created with a protected DACL (SYSTEM and
  Administrators only). Before each use, each is checked: it must be a real folder (not a
  junction), it must resolve to its own path, and it must be owned by SYSTEM or Administrators.
  Anything else is refused: a folder a normal user created in ProgramData could otherwise collect
  backups.
- **Concurrency.** The named mutex `Global\Avel.ChangeStore` serializes the GUI and the CLI.
  While a set runs, its `lock` file is held open exclusively; the file deletes itself when
  closed, even if the process is killed.
- **Users' hives in avel.exe.** A signed-out user's hive can be loaded privately only once per
  process, so a set that writes it fails on it (in use) while a scan in the same process holds it.
  avel.exe has one hive lock for all its background work (`src/app/hive_access.h`): every scope
  that loads hives (a `LiveEnvironment`, an inventory scan through `scanInventory`, a trace's
  stop) holds it shared, and every execute, restore and recovery holds it exclusive. It is taken
  on worker threads only, before the ChangeStore mutex, never after it. A change that has to wait
  for a scan says so ("Waiting for a scan elsewhere in Avel to finish…") until it can go on. The
  CLI runs one command at a time and needs no lock.
- **Journal.** Each line is flushed to disk before the next step. A torn last line is ignored,
  and is closed off before the next record.
- **Retention.** Sets older than 30 days are purged, with their quarantine, every time the store
  is opened. A set that is still running is never purged.

## Items (`executor.cpp` and friends)

| Kind | Backup (written and flushed before anything changes) | Removal | Undo |
|---|---|---|---|
| Registry key | The whole tree: classes, raw values, each key's owner, group and DACL (`key.json`), plus a readable `key.reg` | `RegDeleteTree` | Recreated; security applied last, only where it differs from what was inherited |
| Registry value | Type and raw bytes (`value.json`, `value.reg`) | `RegDeleteValue` | `NtSetValueKey` |
| File or folder | `file.json`: where it was, where it went, every folder's attributes, times and SDDL | Renamed into quarantine through its handle (one rename for a whole folder) | Renamed back |
| Service | Configuration, security and its registry key (`service.json`) | Stopped (up to 30 s), then `DeleteService` | `CreateService`, the key's other values merged back, the delayed flag also through the SCM, security, restarted if it was running |
| Scheduled task | Its XML and security (`task.json`, `task.xml`) | `DeleteTask` | Registered again as the principal it names |
| Task folder | Its security (`task-folder.json`) | `DeleteFolder`, only when empty: after its tasks, deeper folders first | Created again (parents too), then given its security where it differs |
| Value edit | The value before and the value written, raw (`edit.json`) | Written only while the value is exactly what was planned, otherwise `skipped (changed)` | Exactly back while the value is what was written; otherwise the inverse, where there is one |
| Startup approval (state change) | The value before and the value written, raw, and the topmost key the change created (`state.json`) | Written only while the value is what was planned (the key created if missing), otherwise `skipped (changed)` | Back while the value is what was written, otherwise `restore-skipped (changed)`; created keys deleted again once empty |
| Task state (state change) | Enabled before and written (`state.json`) | `put_Enabled` only while the flag is what was planned | `put_Enabled` back while the flag is what was written |
| Service start type (state change) | Start type and delayed flag before and written (`state.json`) | `ChangeServiceConfig(2)W` only while both are what was planned, each only where it differs | The same, back, while both are what was written |
| Package | Its name (`package.json`) | `RemovePackageAsync` for the current user | Not possible |
| Package for every user (deep removal) | Its name, who had it and the provisioned version (`package.json`) | Deprovisioned when provisioned, then `RemovePackageAsync(RemoveForAllUsers)`; only with `allow_irreversible` | Not possible |
| Package registration (state change) | Registered before and written, the package acted on, the other users and whether it was provisioned (`state.json`) | Removed for the current user only while it is registered for them and something keeps it on this PC, otherwise `skipped (changed)` or `(not-undoable)` | Registered again while it isn't and some version of the family is staged; the version registered goes into `state.json` |
| Package provisioning (state change) | Provisioned before and written, the package acted on (`state.json`) | Deprovisioned only while it is provisioned | Provisioned again while it isn't and the family is staged |
| Firewall rule | | Never: reported only | |
| Driver | | Never: reported only (removing a driver can leave Windows unable to start) | |

COM classes served from the program's folder, and the shell-extension handlers and `Approved`
entries that name them, are ordinary registry items. They are removed with undo like any other,
because they are what leave dead entries in Explorer's context menu. The plan first listed them
as report-only; the user chose removal with undo on 2026-10-19.

**Value edits** (`ValueEditLocation`, planned by `leftovers/value_edits`, see
[leftovers.md](leftovers.md)) change a value that other programs share instead of removing it:
they take the installer's entries out of `Path`, lower a SharedDLLs count by its increment, delete
a variable it created, or restore one it changed. Each edit carries the value it expects and the
value it writes, both raw, and HardFloor allows them only in the machine's and each user's
`Environment` and the two SharedDLLs keys, and, for the browser extensions tool
([tools.md](tools.md), "Browser extensions"), as `restore-value` edits that write a string or
delete the value, in browsers' extension policies: a numbered value of
`Software\Policies\<vendor>\ExtensionInstallForcelist` or of Firefox's `Extensions\Install` and
`Locked`, and the `ExtensionSettings` value of a vendor's policy key
(`tools::is_extension_policy_value`; HKLM in either view, or a user's hive).
- **Compare-and-set.** The value is read right before the edit; if it isn't byte for byte what the
  plan expected (the user added an entry since, another program raised the count), the edit is
  skipped as `changed` and nothing is written.
- **Undo.** While the value is still what the edit wrote, the old value goes back exactly.
  Otherwise the inverse is applied to the value as it is now: removed entries go back into the
  list after the entry that preceded them (unless they are there already), and a count is raised
  by what was taken off. A restored or deleted variable has no inverse: it stays as the user left
  it (`restore-skipped (changed)` or `(exists)`).
- **Broadcast.** After a run, an undo or a recovery that changed an environment variable,
  `WM_SETTINGCHANGE "Environment"` is sent once (`ExecuteOptions::environment_changed`, which tests
  replace), so Explorer and new processes see the change.

**State changes** ([ADR 0007](adr/0007-changeset-state-changes.md)) change an item's state rather
than removing it, for Avel Tools (Phase 6). Each records the state it applies to and the state it
sets:
- `StartupApprovedLocation`: Explorer's record of whether a Run value or a Startup folder item
  starts, a 12-byte `REG_BINARY` named after the entry in `...\Explorer\StartupApproved\Run`,
  `Run32` or `StartupFolder` (machine or user; absent means enabled). The first byte's low bit
  set means disabled. Avel writes what Task Manager writes, `03 00 00 00` and the FILETIME of the
  change to disable, `02` and 11 zero bytes to enable, so Task Manager agrees
  (`leftovers/startup_approved.h`).
- `TaskStateLocation`: a scheduled task's Enabled flag.
- `ServiceStartLocation`: a service's start type and delayed flag (`ServiceStart`).

How they behave:
- **Compare-and-set both ways.** Execute changes the item only while its live state is the state
  planned (`skipped (changed)` otherwise). Undo puts the old state back only while the item is in
  the state written (`restore-skipped (changed)` otherwise, "its state has changed since"); there
  is no inverse to apply. A task or service gone since is `restore-skipped (missing)`.
- **Missing keys.** A `StartupApproved` key that doesn't exist yet is created with its missing
  parents. `state.json` names the topmost key created (`created_key`); undo, and recovery of a
  change a crash interrupted, delete those keys again, deepest first, each only while it's empty.
- **Only what differs.** A service's start type and its delayed flag are each written only when
  they differ from the live ones, so undo leaves the service's key as it was: setting the flag
  where it wasn't set adds a `DelayedAutostart` value.
- **Protection.** HardFloor allows approvals only in the three `StartupApproved` keys and only in
  Task Manager's two shapes (or the value deleted), never touches Windows' own tasks
  (`\Microsoft\...`), and never changes the start type of a driver or of a service whose image
  (a `svchost.exe` service's `ServiceDll`) is Windows' own (`windows-service`), nor to anything
  but Automatic, Manual or Disabled. Signatures are read by `platform/signature`: the embedded
  signature, then the catalogs, never the network. Windows' own means validly signed through one
  of Microsoft's product roots, and not by the publisher of hardware vendors' WHQL-signed driver
  packages; Microsoft's applications, signed through its application root (Edge's updater, SQL
  Server's writer, WSL), aren't (see [tools.md](tools.md#protection)).
- **Signed-out users.** A user's approval goes through the same writable private load of their
  `NTUSER.DAT` as removals do (`Roots`).

**Packaged apps** ([ADR 0008](adr/0008-windows-apps-and-features.md), Phase 6b) go through the
public `PackageManager` only (`platform/packages`), which the executor reaches through
`PackageSystem` (`changes/package_items.h`; tests replace it with a fake):
- `PackageUserLocation { family_name, full_name, user_sid, before, after }`
  (`package-user:<sid>\<family>`): whether the family (any version) is registered for a user
  (`FindUsers`). Avel changes it **only for the account it runs as**: Windows has no public call
  that removes or registers a package for another user, so another SID is `skipped (other-user)`.
- `PackageProvisioningLocation { family_name, provisioned_full_name, before, after }`
  (`package-provisioning:<family>`): whether the family is provisioned, that is, installed for every
  new user (`FindProvisionedPackages`).
- `PackageLocation` with `all_users` (`package:all-users:<full name>`): the removal for every user,
  irreversible like the current user's removal.

How they behave:
- **Compare-and-set**, as for the other state changes: execute changes the state only while it is
  `before`, undo only while it is what was written (`skipped (changed)`, `restore-skipped (changed)`).
- **Undo needs the package on this PC.** Windows deletes a package the moment its last user's
  registration goes, unless it is provisioned, and nothing brings it back offline. So execute
  refuses a removal for the current user that nobody else's registration and no provisioning would
  outlive (`skipped (not-undoable)`: that is a removal for every user, a choice of its own), and
  undo registers or provisions again only while some version of the family is staged; otherwise
  `restore-skipped (missing)` ("the app is no longer on this PC; reinstall it from the Store").
  Undo registers whatever version is staged then: an update may have replaced the one removed.
- **Success is read back.** An operation counts as done only when the state it is meant to reach
  is read back afterwards (`PackageOperation::confirmed`): a registration can report success with
  nothing registered. Otherwise the item failed with the operation's code.
- **`state.json`** records the full name acted on and, after undo, the one registered or
  provisioned again (`restored_full_name`). A registration also records the other users that had
  the family and whether it was provisioned: what made its undo possible. The app's data is not
  backed up: Windows deletes it with the registration, and undo gives back the registration only.
- **The removal for every user is one item**, not a deprovisioning item plus a removal item:
  - the user asked for one irreversible thing, and its confirmation covers both halves;
  - once the package is gone, a separate deprovisioning item could never be undone either, yet it
    would be offered in the Undo Center as undoable;
  - as two items, the deprovisioning would still run when the removal is refused (not allowed,
    protected), leaving a change the user didn't ask for on its own.

  It deprovisions first when the family is provisioned (so no new user gets it meanwhile), then
  removes it for every user. If the deprovisioning worked and the removal didn't, the item is
  `partial`. It holds (`contains`) the same family's registrations and provisioning in the plan,
  which are then skipped as `contained`, but only when it runs itself (`allow_irreversible`).
- **Order.** Removals for every user first, then registrations, then provisioning: a user's
  registration goes while the package is still provisioned, and undo provisions again before it
  registers.
- **Protection.** HardFloor refuses every package item (`windows-package`) for a package signed as
  part of Windows (`SignatureKind::System`, the public stand-in for `NonRemovable`), a framework or
  resource package, and the families on the critical list (`tools::app_category`: the Store, App
  Installer, the shell's hosts...); also a package item whose family can't be told. A package with
  an external location, which gives a desktop program its identity, is refused
  (`desktop-program-package`) for a registration, provisioning and a removal for every user; only
  a removal for one user, as the leftover scan offers it once its program is uninstalled, may take
  it. A package that isn't on this PC is left to the executor, which reports it missing.

**Exactness.** Values are read and written through `NtEnumerateValueKey`, `NtQueryValueKey` and
`NtSetValueKey`. The Win32 calls may add a terminator to a string value that was stored without
one, depending on what follows the data in memory.

A key's DACL that is marked auto-inherited (`D:AI(...)`, as `SetSecurityInfo` and Windows
Installer leave it) loses the mark when it goes back through `RegSetKeySecurity`, unless the
descriptor asks for inheritance (`SE_DACL_AUTO_INHERIT_REQ`); undo sets that flag. The Phase 4
Sandbox run found this on Windows Installer's `UserData` keys.

A file renamed to another folder gets the archive attribute, so the attribute is cleared again
where it wasn't set before.

**Locked files.** When a folder can't be renamed whole, its files are moved one by one. Files in
use stay where they are, with their folders, and the item is `Partial`. Restart Manager names
the processes holding them. Undo merges the moved files back and recreates the removed folders
exactly as recorded.

**Never forced.** Avel never takes ownership or overrides a refusal:
- A registry tree is opened for deletion as a whole before it's backed up, so a key Avel may not
  delete is skipped (`access-denied`) before anything changes.
- Registry links are refused.
- Paths that resolve somewhere else (a parent folder that became a junction) are refused.
- These services are skipped:
  - drivers;
  - services other services depend on;
  - services running as an account with a password, which undo couldn't restore.
- Tasks that store a password are skipped, and task folders that aren't empty (`not-empty`).
- A value edit or a state change never overwrites a value or a state that changed since it was
  planned (`changed`).
- Packages registered for other users are skipped, and so are other users' registrations.

## Running a plan

`execute(store, plan, options)` does the following:
1. **Creates the set** and writes its plan.
2. **Orders the items:** services first, then tasks, task folders (deepest first), packages
   (removals, users' registrations, provisioning), registry values and value edits, state changes,
   registry keys, and files last. A service or
   task stops using its files before they move, and a task folder is empty by its turn.
3. **Skips an item inside another planned item** (`contained`); it goes with its container.
4. **Checks each item again** right before it changes:
   - HardFloor;
   - ProtectedPolicy, for the known folders of the machine and every user;
   - packages, which can't be undone, are skipped unless `allow_irreversible` is set;
   - firewall rules are reported only.
5. **For each item:** writes the backup, records `backed-up`, removes the item, then records
   `done`, `partial`, `skipped` or `failed`.
6. **Writes the summary.**

## Undo

`restore(store, id, only)` puts items back in the reverse order: files first, services last.
- **Nothing new is replaced.** If something appeared in an item's place since (a reinstalled
  program's folder or key), the item stays in the backup as `restore-skipped (exists)`.
- **Retrying is safe.** An item that couldn't be restored can be tried again later.
- **Partial items** are merged back into what's left of them.
- **The set's state** becomes `restored` when every undoable item is back, otherwise
  `partly-restored`.

## Recovery

When the store is opened, `recover()` settles every set left `running` by a crash or a killed
process. It looks at each item that was backed up but not finished:

| Item | Still in place | Gone | Both |
|---|---|---|---|
| File or folder | `failed (interrupted)`: untouched | `done` | `partial`: some files moved |
| Registry key | `failed` if identical to the backup, `partial` otherwise | `done` | |
| Value, service, task, task folder, package | `failed (interrupted)` | `done` | |
| Package for every user | `failed (interrupted)`, or `partial` when it is no longer provisioned | `done` | |

| Item | In the state written | Anything else |
|---|---|---|
| Value edit | `done` | `failed (interrupted)` |
| Startup approval, task state, service start type | `done` | `failed (interrupted)`; keys the change created and left empty are deleted |
| Package registration, package provisioning | `done` | `failed (interrupted)` |

"Anything else" is untouched, or changed by someone else since, which undo mustn't take back. If
the interrupted run had edited the environment, recovery sends the broadcast it never got to.

Items never reached are marked `skipped (interrupted)`. After that the set is complete and can
be undone like any other.

## Front ends

**CLI** (administrator only):
- `avel-cli uninstall ... --remove-leftovers [--min-level good|very-good] [--allow-irreversible]`
  removes what the uninstall left behind (Good and above by default) after the leftover scan.
  `make_plan` leaves out what is never selected automatically (`leftovers::held_back`): an item
  excluded in an install trace's review that another scanner found, and what holds it
  ([leftovers.md](leftovers.md), "Install traces"). A line says how many were left in place.
  JSON output gains `leftover_removal`.
- `avel-cli undo --list | --show <id> | <id> [--item <n>]... | --purge [--json]`.
- `AVEL_TEST_FAULT_AFTER=<n>` ends the process after n journal records, as a crash would. Only
  the Sandbox's recovery test uses it.

**GUI** (`src/app`, administrator only, like the GUI itself):
- **After an uninstall,** `LeftoverDialog` shows what the leftover scan found, and Remove runs the
  checked items as one set in `ChangeSetProgressDialog` (see [leftovers.md](leftovers.md)). If
  checked packages can't be undone, the dialog asks first: remove them too, keep them, or cancel.
- **The progress dialog** runs `execute()` on a worker thread and shows each item's progress.
  Then it lists what wasn't removed, or not completely, with the reason and the processes holding
  files. Its **Undo** button runs `restore()` right away. It can't be closed while it runs.
- **Tools > Undo Center** lists the sets from the last 30 days: date, description, state, how many
  items were removed and restored, and the size of the backup. A set's items show their state from
  the journal; restorable ones have a checkbox. **Restore all**, **Restore checked** and
  **Delete backup** (with a confirmation) act on the selected set. A set is read and let go at
  once, because undo opens it again and only one holder may have it.
- **At startup** the GUI settles interrupted sets and purges old ones on a worker thread, as every
  `avel-cli` run does. If it settled any, it says so and offers the Undo Center.
- **Locks:** a run waits up to 30 s for the store's mutex, then reports that another Avel process
  is changing the system. The store is opened once (`ChangeStoreAccess`) and shared by the
  dialogs. Without administrator rights, or with an untrusted store, removal and undo are
  disabled with the reason shown.
- **Text:** `LeftoverText` turns item states, reasons, set states and folder statuses into
  translated text. A set's description ("Leftovers of Notepad++") is stored in the language that
  was active when the set was made. A value edit is described as what it does ("Remove
  C:\Contoso\bin from the system variable Path"). Once made, it reads "Edited", and when its value
  changed since the scan, "Skipped: the value has changed since" (`changed`). A state change is
  described the same way ("Disable Contoso at startup (HKLM\...\StartupApproved\Run)", "Change
  the start type of the service ContosoSvc from Automatic to Manual"); once made it reads
  "Disabled", "Enabled" or "Start type changed", and "...: its state has changed since" when
  compare-and-set left it alone. A package's registration reads "Remove Contoso.App_abc for you",
  then "Removed for you" and, after undo, "Registered for you again"; its provisioning "Don't
  install ... for new users", then "Won't be installed for new users" and "Will be installed for
  new users again"; a removal for every user "Removed for every user".

## Testing

- **Registry round trips (`registry_backup_test.cpp`).** A tree in the volatile scratch key with:
  - every value type at awkward lengths;
  - an unterminated string;
  - an empty default value next to a missing one;
  - names with unpaired surrogates and quotes;
  - a key class;
  - a protected DACL.

  The tree is dumped by an independent dumper, captured, passed through JSON, deleted, restored
  and dumped again; the dumps must match. More tests cover partial deletion, an undeletable
  subkey and registry links.
- **Quarantine (`file_quarantine_test.cpp`).** A temporary tree with:
  - read-only and hidden files;
  - an alternate stream;
  - a hard link;
  - a protected DACL;
  - a path over 260 characters;
  - a junction pointing out of the tree.

  Whole-folder and file-by-file round trips must compare equal, including file ids. A file held
  open stays and is attributed to its process. Something new in the item's place is never
  replaced.
- **Store (`change_store_test.cpp`):**
  - plan and journal round trips;
  - a torn journal line;
  - live locks;
  - listing;
  - purging with a fake clock;
  - refusing a foreign owner, a junction, a redirected path, or a file in the way.
- **Executor (`executor_test.cpp`).** A plan over scratch registry keys and a temporary tree,
  with protected, contained, report-only, irreversible and missing items. It is removed and
  undone exactly; it is undone item by item; and it is killed after every single journal record
  (14 crash points), recovered and undone, each time back to the original.
- **Value edits (`executor_value_edit_test.cpp`, `value_edits_test.cpp`).** A traced install's
  edits in scratch keys standing in for the environment and SharedDLLs, planned from their values
  before and after it: removed and undone exactly; skipped when a value changed after planning;
  undone by the inverse when the user edited a value after the removal; killed after every record
  (11 crash points), recovered and undone; the broadcast hook called once, and only for the
  environment. The planner's cases are in [leftovers.md](leftovers.md).
- **State changes (`executor_state_change_test.cpp`, `startup_approved_test.cpp`).** Startup
  approvals in scratch keys standing in for HKLM and a user's hive, with a machine list that
  exists, one that doesn't, and a user hive with no `Software` key at all: disabled and enabled
  with Task Manager's exact bytes and undone byte for byte, the created keys gone again; skipped
  when a value changed after planning; undo skipped when it changed after the change, and a
  created key kept when something else was put in it; killed after every record (7 crash points),
  recovered and undone; an empty key a crash left behind removed by recovery; the floor refusing
  other keys and other bytes. Tasks and services only read: a missing one is skipped as missing,
  EventLog is refused by the floor and, with the floor out of the way, skipped as changed for a
  plan made for another start type. The bytes' decoding (`01`, `02`, `03`, `06`) and the three
  keys have their own tests.
- **Packaged apps (`executor_package_test.cpp`).** A fake `PackageSystem` behaving as Windows did in
  the Phase 6b spike (a package goes with its last user unless provisioned; registering needs it
  staged). Removed for the current user and registered again (a newer version after an update),
  deprovisioned and provisioned again; another user's registration refused untouched; skipped when
  the state changed after planning; a removal that would delete the package refused; undo skipped
  when the state changed or the package went since; a registration that reports success without
  registering counted as failed, and retried; Windows' own packages refused by the floor; the
  removal for every user refused without `allow_irreversible`, holding the family's other items
  with it, partial when only the deprovisioning worked; killed after every record, recovered and
  undone; an interrupted removal for every user settled by looking. Real packages change only in
  the Sandbox.
- **Signatures (`signature_test.cpp`):** Windows' own files, embedded and catalog-signed, are
  valid and Microsoft's; the unsigned test helper is unsigned; a missing file is missing; a folder
  is an error; the cache answers the same.
- **Serialization (`serialization_test.cpp`):** every location kind through JSON, value edits and
  state changes with raw values and names UTF-8 can't carry, the task-folder flag, a state
  change's identity, package states and the `all_users` flag (absent in older plans), what a
  removal for every user contains, and malformed edits, state changes and package states refused.
- **Protection (`protection_test.cpp`):** the startup approval allow-list and its two shapes,
  Windows' own tasks, and drivers, Microsoft's services and boot or system start types refused
  (through injected service facts, and EventLog read live); System-signed, framework, resource and
  critical packages refused for every package kind (through injected package facts).
- **Windows Sandbox (`tests/sandbox`, scenario `removal`):**
  - `Contoso Fixture` leaves one leftover of every kind behind, and `avel_state_dump` dumps them.
  - Uninstall with `--remove-leftovers` must remove all of them and Notepad++'s sparse package,
    and leave the canaries, the other program and the firewall rule as they were.
  - Undo, one item and then the rest, must give back identical dumps.
  - So must a run killed after 7 records, once recovered and undone.
  - **The GUI round** does the same through UI Automation: uninstall the fixture in `avel.exe`,
    press Remove in the leftover dialog, then Restore all in the Undo Center. The dumps must show
    every leftover gone, then all of them back exactly.

## Known limits

- **Packages** can't be put back once removed from this PC: the original package file is gone.
  They are removed only when explicitly allowed. A user's registration and the provisioning are
  undone only while the package stays staged, and a registration only for the account Avel runs as.
- **A removal for the current user acts on the version planned.** If an update replaced it
  between the plan and the change, the removal fails and changes nothing.
- **Firewall rules** are reported, never removed.
- **A restored service's triggers** take effect after a restart: they're restored through its
  registry key, which the Service Control Manager reads at startup. It reads a delayed automatic
  start (`DelayedAutostart`) there only at startup too, so restore also sets that flag through
  `ChangeServiceConfig2W`; otherwise the service would count as plainly automatic until a restart.
- **Keys and folders get their owner back only with SeRestorePrivilege**, which elevated
  administrators have.
- **The folder holding a removed item** keeps the last-write time of the removal: it's not part
  of the item.
- **A volatile key under a non-volatile parent** comes back non-volatile.
