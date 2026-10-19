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
| Package | Its name (`package.json`) | `RemovePackageAsync` for the current user | Not possible |
| Firewall rule | | Never: reported only | |

COM classes served from the program's folder, and the shell-extension handlers and `Approved`
entries that name them, are ordinary registry items. They are removed with undo like any other,
because they are what leave dead entries in Explorer's context menu. The plan first listed them
as report-only; the user chose removal with undo on 2026-10-19.

**Exactness.** Values are read and written through `NtEnumerateValueKey`, `NtQueryValueKey` and
`NtSetValueKey`. The Win32 calls may add a terminator to a string value that was stored without
one, depending on what follows the data in memory.

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
- Tasks that store a password are skipped.
- Packages registered for other users are skipped.

## Running a plan

`execute(store, plan, options)` does the following:
1. **Creates the set** and writes its plan.
2. **Orders the items:** services first, then tasks, packages, registry values, registry keys,
   and files last. A service or task stops using its files before they move.
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
| Value, service, task, package | `failed (interrupted)` | `done` | |

Items never reached are marked `skipped (interrupted)`. After that the set is complete and can
be undone like any other.

## Front ends

**CLI** (administrator only):
- `avel-cli uninstall ... --remove-leftovers [--min-level good|very-good] [--allow-irreversible]`
  removes what the uninstall left behind (Good and above by default) after the leftover scan.
  JSON output gains `leftover_removal`.
- `avel-cli undo --list | --show <id> | <id> [--item <n>]... | --purge [--json]`.
- `AVEL_TEST_FAULT_AFTER=<n>` ends the process after n journal records, as a crash would. Only
  the Sandbox's recovery test uses it.

**GUI.** Phase 3c.

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
- **Windows Sandbox (`tests/sandbox`, scenario `removal`):**
  - `Contoso Fixture` leaves one leftover of every kind behind, and `avel_state_dump` dumps them.
  - Uninstall with `--remove-leftovers` must remove all of them and Notepad++'s sparse package,
    and leave the canaries, the other program and the firewall rule as they were.
  - Undo, one item and then the rest, must give back identical dumps.
  - So must a run killed after 7 records, once recovered and undone.

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
