# Changes: how Avel removes leftovers and undoes it

Removal lives in `src/core/changes`. Like the rest of `avel_core`, it has no Qt dependency and
returns codes rather than text. Every removal is a **ChangeSet**: a plan of items, each backed up
before it changes and recorded as it goes, so the set can be undone exactly and survives a crash.
How sets are stored is decided in [ADR 0004](adr/0004-changeset-folder-journal.md).

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
| Service | Configuration, security and its registry key (`service.json`) | Stopped (up to 30 s), then `DeleteService` | `CreateService`, the key's other values merged back, security, restarted if it was running |
| Scheduled task | Its XML and security (`task.json`, `task.xml`) | `DeleteTask` | Registered again as the principal it names |
| Task folder | Its security (`task-folder.json`) | `DeleteFolder`, only when empty: after its tasks, deeper folders first | Created again (parents too), then given its security where it differs |
| Value edit | The value before and the value written, raw (`edit.json`) | Written only while the value is exactly what was planned, otherwise `skipped (changed)` | Exactly back while the value is what was written; otherwise the inverse, where there is one |
| Package | Its name (`package.json`) | `RemovePackageAsync` for the current user | Not possible |
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
`Environment` and the two SharedDLLs keys.
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
- A value edit never overwrites a value that changed since it was planned (`changed`).
- Packages registered for other users are skipped.

## Running a plan

`execute(store, plan, options)` does the following:
1. **Creates the set** and writes its plan.
2. **Orders the items:** services first, then tasks, task folders (deepest first), packages,
   registry values and value edits, registry keys, and files last. A service or task stops using
   its files before they move, and a task folder is empty by its turn.
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

A value edit is `done` if the value is what the edit writes, and `failed (interrupted)` otherwise
(untouched, or changed by someone else since, which undo mustn't take back). If the interrupted
run had edited the environment, recovery sends the broadcast it never got to.

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
  changed since the scan, "Skipped: the value has changed since" (`changed`).

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
- **Serialization (`serialization_test.cpp`):** every location kind through JSON, value edits with
  raw values and names UTF-8 can't carry, the task-folder flag, and malformed edits refused.
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

- **Packages** can't be put back: the original package file is gone. They are removed only when
  explicitly allowed.
- **Firewall rules** are reported, never removed.
- **A restored service's triggers** take effect after a restart: they're restored through its
  registry key, which the Service Control Manager reads at startup.
- **Keys and folders get their owner back only with SeRestorePrivilege**, which elevated
  administrators have.
- **The folder holding a removed item** keeps the last-write time of the removal: it's not part
  of the item.
- **A volatile key under a non-volatile parent** comes back non-volatile.
