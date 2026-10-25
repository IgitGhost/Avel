# Avel Trace: recording what an installer changes

Avel Trace records what an installer changes, so the program can later be removed exactly. It
lives in `src/core/tracker`, with helpers in `src/core/platform` (`usn_journal`, `nt_registry`,
`record_folders`, `hash.h`). Like the rest of `avel_core` it has no Qt dependency and returns codes
rather than text.

It works without a driver ([ADR 0002](adr/0002-install-tracker-without-driver.md)): it compares
the system after the installation with the system before it.
- **Files:** each fixed NTFS volume's USN journal, read on from the position recorded at the start.
- **Registry:** a snapshot taken at the start: every key's LastWriteTime and a hash of each value.
  At the end, values are read again only where a key's LastWriteTime changed.
- **Known spots:** the environment variables and SharedDLLs counts, saved byte for byte at the
  start and the end.

How traces are stored and what the snapshot holds is decided in
[ADR 0006](adr/0006-trace-folders-registry-snapshot.md).

Phase 5a, described here, records and reports. Linking a trace to the program it installed,
uninstalling by the trace, undoing value edits, and export and import come in 5b; the GUI comes in 5c.

```
start:  journal positions --> known values --> registry snapshot --> state "open" (all on disk)
open:   while an Avel process drives the trace, the journals are read every 2 s (checkpoints)
stop:   last read --> "stopping" --> file changes --> registry diff --> known values diff
        --> noise filter --> changes.json --> "complete" or "incomplete"
```

## What a trace reports

Each change is one `TraceChange` (`trace.h`):

| Change | Means | Later (5b) |
|---|---|---|
| `created` | Didn't exist at the start, exists at the end | Can be offered for removal |
| `deleted` | Existed at the start, is gone | Shown only |
| `modified` | Existed at the start; its content or values changed | Shown only |
| `renamed` | Existed at the start under another name or folder (files and folders) | Shown only |

Its location is the one the rest of Avel uses (`leftovers::Location`), so a trace item can go
through the same protection, review and ChangeSet as a leftover: a file or folder, a registry key
or value, or, for what the registry diff recognizes, a service, a driver, a scheduled task or a
firewall rule. Created folders and keys carry `contents`, the number of items created inside them;
only the outermost created item is listed.

Every change also carries a noise reason, `none` for what the installer did (see "The noise
filter"). Values in the known spots are reported apart, as known-value changes with their exact
bytes before and after.

## Files: the USN journal (`journal_trace`, `file_changes`)

**At the start** (`start_volume`), for every fixed NTFS volume with a drive letter:
- `platform::UsnVolume` opens `\\.\X:` (administrator rights) and identifies the volume by its GUID
  path (`\\?\Volume{...}\`) and serial number;
- `FSCTL_QUERY_USN_JOURNAL` gives the journal's id and the next USN, where reading will start.
- A volume without a journal gets one (`FSCTL_CREATE_USN_JOURNAL`, 64 MB maximum, 8 MB allocation
  delta). It stays after the trace, and the volume reports `created_journal`. If creating it fails,
  the volume is `untracked`.
- An existing journal is never resized, whatever its size.

**While the trace is open,** a process that drives it (`trace run`; later the GUI) reads every
volume's journal every 2 seconds (`TraceSession::poll`). Records are collected before the journal
can overwrite them, and each volume's position and what was read so far are written to its
checkpoint (`volume-<n>.checkpoint`: binary, versioned, with an FNV-1a checksum), so the trace
survives a crash or a restart. A manual trace (`trace start`) has no such process: its journals are
read once, at `stop`.

**Reading** (`drain_volume`): `FSCTL_READ_USN_JOURNAL` (`READ_USN_JOURNAL_DATA_V1`, records of
versions 2 and 3) from the saved position up to the next USN queried just before, so a read ends
even while the disk stays busy. Records that say nothing about what programs see are dropped:
auxiliary-data records (a virus scanner's private stream), NTFS's own metadata (MFT records below
24, items in `$Extend`, `$` names in the root). The volume is found again by its GUID path, so a
drive letter that changed across a restart doesn't matter.

**Incomplete volumes.** Before each read the journal is checked (`check_journal`). A volume whose
journal can't be followed stops being read; what was read before stays valid, and the other
volumes and the registry are unaffected. The trace ends `incomplete`, naming the volume:

| Status | When |
|---|---|
| `untracked` | At the start: a volume that has no journal and couldn't get one |
| `volume-missing` | No drive letter leads to the volume any more |
| `serial-changed` | The volume was formatted |
| `replaced` | The journal was deleted, or deleted and created again (another id, or a position past its end) |
| `wrapped` | The records still to be read were overwritten (`ERROR_JOURNAL_ENTRY_DELETED`, or the position is below the journal's first USN) |
| `unreadable` | The volume or its journal couldn't be read, or its checkpoint was damaged |

**Net effects.** `FileChangeAggregator` gathers every record per file reference number, which
includes the MFT record's sequence number, so a reused record counts as another file. It keeps the
item's name before the trace changed it and after its last change, its reasons, and the names of
hard links added or removed. `resolve_file_changes` turns that into one change per item:

| Records | Reported |
|---|---|
| Created, then deleted | Nothing; but `deleted` if its creation wasn't recorded during the trace (see "Items open across the start") |
| Created | `created`, at its path at the end |
| Deleted | `deleted`, at its path at the start |
| Deleted while still open: moved into `$Extend\$Deleted` until its last handle closes | As deleted |
| Already in `$Extend\$Deleted` at the start (deleted before the trace), its last handle closed | Nothing |
| Renamed or moved (the path differs) | `renamed`, with `old_path` |
| Content changed (data, named streams, reparse point) | `modified` |
| Attributes, times, security or extended attributes only | `modified`, noise `metadata-only` |

Extended attributes count as metadata because Windows keeps its own state there (Code Integrity
notes in one what it checked of a signed file). A folder's own metadata changes are reported the
same way: an installer that opens up a folder's permissions or marks it read-only for its
`desktop.ini` did change something, though removal never takes it back.

**Windows containers** (Windows Sandbox, Windows containers): the system's files and folders are
Windows Container Isolation placeholders for a read-only base layer. NTFS gets each one as a new
item, with a new reference number, the first time it is used: a folder when it is listed
(`FILE_CREATE`, `REPARSE_POINT_CHANGE`), a file when it is read (and `DATA_EXTEND`), a program when
it is loaded (and `EA_CHANGE`, Code Integrity's catalog hint). The container's filter hides the
reparse point from everything above it (its tag can't be read, even by id with
`FILE_FLAG_OPEN_REPARSE_POINT`), but the journal's records carry its attributes. An item whose
first record creates it with `REPARSE_POINT_CHANGE` as an offline reparse point (`REPARSE_POINT`
and `OFFLINE`; not a cloud file's placeholder, which is recalled on access), that is older than the
trace and that programs see without a reparse point existed before the trace: those reasons say
nothing, and only what else its records say (written, renamed, deleted, secured) is a change.
Deleting or renaming a base-layer item leaves a tombstone under its old name: a new item, created
with `REPARSE_POINT_CHANGE` as a hidden, system reparse point, never offline, that programs don't
see. One that is gone again or isn't what programs find at its path is no change: the item's own
records already say it was deleted or renamed.

Then a few passes by path:
- **Replaced files.** A created file where another existed at the start (temp-then-rename,
  `MoveFileEx` with `REPLACE_EXISTING`, Windows Installer moving the old file aside) is `modified`,
  with `replaced: true`, never `created`. An old file moved aside stays listed as `renamed`. So is
  an older file renamed where another was at the start (WMI's performance adapter renames
  `WmiApRpl_new.ini` over `WmiApRpl.ini`), and its old name is `deleted`, unless another file took
  that place in turn.
- **Roots only.** An item inside a created folder isn't listed; it counts in the folder's
  `contents`.
- **Moved-in folders.** A created folder that holds something older (moved in, or modified) can't
  be offered whole. It isn't listed; its created children are, one by one.
- **Items open across the start.** Reasons accumulate from an item's first open to its last close,
  so an item opened before the trace and closed during it can carry a leftover "created" reason; its
  own creation record, `FILE_CREATE` alone, isn't among the trace's. Deleted, it is `deleted`;
  renamed, and older than the trace, it is renamed like any other item. A
  created root whose creation record wasn't seen and whose
  creation time is earlier than the trace's start counts as `modified`. Its children may become
  roots then, so this repeats until nothing changes.
- **Hard links.** A link added to an older file is a `created` file of its own (a new name). A
  created file is listed under each of its names that still leads to it. A write through a new
  link is a `modified` of the file's old name.
- **Paths.** Names come from the records. A folder the journal said nothing about is opened by its
  id (`OpenFileById`, `GetFinalPathNameByHandleW`); if that fails, it is looked up in one read of
  the volume's folders (`VolumeFolders`, [ADR 0005](adr/0005-deep-search-from-the-mft.md)). Paths
  at the start follow a folder renamed during the trace. A path that can't be found has `?` for
  the unknown part and the noise reason `unresolved`.

## Registry: the snapshot (`registry_snapshot`)

**Roots:**

| Root | Reported as |
|---|---|
| `HKLM\SOFTWARE`, opened physically, so `WOW6432Node` and `Classes` are included | `HKLM\SOFTWARE\...` |
| `HKLM\SYSTEM\CurrentControlSet\Services` | `HKLM\SYSTEM\CurrentControlSet\Services\...` |
| `HKU\<sid>\Software`, for every user signed in at the start | `HKU\<sid>\Software\...` |
| `HKU\<sid>_Classes`, the same users' classes | `HKU\<sid>\Software\Classes\...` (read once, as this root) |

Not walked, because only Windows changes them: Component Based Servicing, Windows Defender and
`Services\bam`. Packages' state (`Appx`, `AppModel`, each user's `Local Settings\...\AppModel`) and
the Start menu's (`CloudStore`) are walked: Windows writes them for what an installer registers,
and the noise filter sets them aside.

**The walk** (`take_registry_snapshot`) uses the native API directly, since it opens and lists every
key:
- Below a root, every key is opened as itself (`NtOpenKeyEx` with `OBJ_OPENLINK`). A registry link
  is recorded, never followed, so the aliases between the 32-bit and 64-bit views aren't read twice.
  The roots themselves are opened normally: `CurrentControlSet` is a link.
- A key that denies access is opened again with backup intent when `SeBackupPrivilege` can be
  enabled. Otherwise it is marked unreadable: changes in it and below it are unknown, the snapshot
  reports `key-unreadable`, and the trace ends `incomplete`.
- It runs on one thread per hardware thread, at most 8. A key with 256 or more children within 4
  levels of a root (CLSID, Installer's components) has them read in jobs of 128.
- Keys are laid out depth first, each key's children in one block, so one thread and eight give
  the same bytes.
- What is read is the hive. The `Counter` and `Help` values Win32 shows below `Perflib\009` (each
  language) and `Perflib\CurrentLanguage` aren't in it: Win32 makes them up from the
  `perf?###.dat` files in `System32`, which the trace reports as files.

**Data,** all fixed-size records:
- per key (32 bytes): its name, flags (link, unreadable), LastWriteTime, and where its children and
  values are;
- per value (16 bytes): its name, its type, and an FNV-1a 64-bit hash of its type, length and exact
  bytes. A value added can be told from a value changed, and a value rewritten with the same data
  is no change;
- names in one deduplicated UTF-16 pool, exactly as the registry holds them (unpaired surrogates
  included).

The values of the `SharedDLLs` keys aren't hashed; the known values cover them.

**The file** `registry.snapshot` holds `AVELRSN1`, a version, the counts, the roots and the arrays,
with an FNV-1a checksum of everything after the header. It is written and flushed before the trace
counts as open, so it survives a restart. Reading it back checks the size, the checksum and every
index; a damaged file means the registry isn't compared, and the trace ends `incomplete`
(`snapshot-damaged`). It is deleted once the trace is finished.

**Size and speed,** from `RegistrySnapshotSmokeTest` on the development machine (unelevated, so
`HKLM\SOFTWARE` and the current user's `Software` and classes, without `Services`):

| Measure | Value |
|---|---|
| Keys, values | about 500,000 keys, 790,000 values, 265,000 distinct names |
| File | about 48 MB, written in 65 ms, read in 60 ms |
| Snapshot | 3.2 to 3.5 s with 8 threads (5 s with one) |
| Comparison right after | 1.4 to 1.8 s, no changes |

A trace runs elevated and adds `Services` and every signed-in user, so its snapshot is somewhat
larger. At 48 MB, compressing the file wasn't needed.

**The comparison at the end** (`diff_registry`):
- The same roots are opened again. A user who signed out during the trace is read from their
  `NTUSER.DAT` and `UsrClass.dat`, loaded privately and read-only; the noise rules, the registry
  and the known values share these hives (`UserHiveCache`).
- Children are merged by name everywhere: a parent's LastWriteTime isn't something to rely on.
- Values are read only where a key's LastWriteTime changed, and compared by type and hash.
- A created key is one item, whose `contents` counts the subkeys and values inside it. A deleted
  key is one item too. A key the snapshot couldn't read is reported as `key-unreadable-at-start`.

**Mapping to Avel's locations:**

| Change | Location |
|---|---|
| A key created directly under `Services` | `service`, or `driver` when its `Type` is a kernel, file system or recognizer driver |
| A key created under `TaskCache\Tree` | a `task` for every key in it with an `Id` value; task folders aren't listed. The task's keys under `TaskCache\Tasks`, `Plain`, `Logon`, `Boot` and `Maintenance` aren't listed apart |
| A value under `FirewallPolicy\FirewallRules` | `firewall-rule`, named by the value (report-only, like everywhere in Avel) |
| A value under either `SharedDLLs` key | a known-value change, never a raw value |
| Anything else | `registry-key` or `registry-value` |

The task's XML file in `C:\Windows\System32\Tasks` comes from the journal, as a created file (or
folder, for a new task folder) next to the task.

## Known spots (`known_values`)

Some values are shared by many programs. An installer appends to `Path`, or raises a DLL's
`SharedDLLs` count; removing the program must take back exactly that, not delete the value or
restore an old copy over the user's later edits. So the trace saves these keys' values byte for
byte, through the native API:
- `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment`;
- `HKU\<sid>\Environment`, for every user signed in at the start;
- `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs` and its `WOW6432Node` twin (a key
  both views share is read once, by its kernel name).

They are captured when the trace starts (`saved_values.json`: the data in hex, and names that
UTF-8 can't carry in hex too, so nothing is lost) and again when it stops. Every value added,
changed or deleted is a known-value change with its exact `before` and `after`. Once the trace is
finished, `saved_values.json` keeps only the values that changed.

The environment keys lie outside the snapshot's roots, and the snapshot leaves the `SharedDLLs`
values out, so these values are never reported as raw registry values. In 5a they are only
reported. In 5b, removal by the trace takes back exactly what the installer added (its `Path`
entries, its `SharedDLLs` increment), and only if the live value still allows it.

## The noise filter (`noise_filter`)

Windows, the user and Avel keep writing while a trace runs. The noise filter sets that aside, so the
result shows what the installer did. Noise stays in `changes.json` with its reason, `show --all`
displays it, and it is never offered for removal.

| Reason | Examples |
|---|---|
| `system-activity` | `System Volume Information`, `Config.Msi`; in the Windows folder `Prefetch`, `Logs`, `SoftwareDistribution`, `AppReadiness`, `appcompat\Programs` and `pca`, `setupapi` and ngen logs, the performance counters' files (`perf?###.dat`, `PerfStringBackup.INI`, WMI's `WmiApRpl` and `wbem\Performance`), AppLocker's policy, Windows' own tasks (`System32\Tasks\Microsoft\Windows`), `LogFiles`, `winevt\Logs`, `sru`, `catroot2`, the WMI repository and the hive files; service accounts' caches; ProgramData's Defender, WER, Search, Diagnosis and delivery folders; users' notification and shell caches, packaged apps' caches, WebView2 data and crash dumps, the shell's app index, PowerShell's startup cache, crypt32's caches in `LocalLow`. Keys: `Tracing`, `Cryptography\RNG`, `AuthRoot`, `SQMClient`, `PolicyManager`, Windows Update, Windows Search, the Microsoft account's `IdentityCRL`, Windows Security's own state, WMI's provider tracing, performance adapter and `DREDGE`, BITS, WER, `Diagnostics`, `Reliability`, Group Policy, `Installer\InProgress`, packages' state (`Appx`, `AppModel`), `Prefetcher`, notifications, the Task Scheduler's lists of tasks by id and Windows' own tasks in `TaskCache\Tree\Microsoft\Windows`; network services (`Tcpip*`, `Dhcp`, `NlaSvc`, `iphlpsvc`), `W32Time`, Defender, update and delivery services, VSS's and DsmSvc's state, and every service's volatile `Enum` subkey; per user: Search, notifications, Content Delivery Manager and `IrisService`, Security and Maintenance, Compatibility Assistant, `IdentityCRL` and `AuthCookies`, the Start menu's `CloudStore`, packages' state and the resource strings cache (`Local Settings\MuiCache`) |
| `user-activity` | The Recycle Bin; users' INetCache, WebCache, History, Explorer caches, `Recent`, browser profiles (Edge, Chrome, Firefox's cache). Keys: `UserAssist`, `RecentDocs`, `FeatureUsage`, `ComDlg32`, `RunMRU`, `TypedPaths`, Explorer's `Bags` and `BagMRU` |
| `temporary` | Windows' `Temp` and `SystemTemp`, each user's `Temp`, packaged apps' `TempState` |
| `avel` | `%ProgramData%\Avel`, the folder Avel runs from, `$Avel.Quarantine`, `Software\Avel` |
| `metadata-only` | A file or folder whose attributes, times, security or extended attributes changed, and nothing else (set by the journal's reasons, not by a rule) |
| `unresolved` | A file whose path couldn't be found |

Some values change by themselves in keys that otherwise hold real data: `ProfileList`'s load and
unload times, a scheduled task's `DynamicInfo`, the last performance counter numbers, Windows
Security noting a product's registration moved, BITS making itself start delayed, the Game Bar's
revision of its list of games, and the shell's count of association changes
(`Explorer\GlobalAssocChangedCounter`, which announcing a change of file associations bumps).

**Never noise,** whatever else matches: Explorer's `MuiCache`, `StartupApproved` and `App Paths`,
which a program's removal relies on. Services, tasks and firewall rules are never noise either.

**The rules are data:** lowercase paths whose components may hold `*` and `?`, each matching the
item and everything below it; the first rule that matches gives the reason. Machine rules match any
`ControlSet00n` as well as `CurrentControlSet`, those below `SOFTWARE` also cover their
`WOW6432Node` twins, and a user's classes match through `<sid>_Classes` as well as
`Software\Classes`. A user's rules follow their folders: the profile `ProfileList` names and the
shell folders in their hive. The
Sandbox's idle round tunes the list.

## Install and trace (`installer_watch`, `install_trace`)

`trace run` starts a trace, launches the installer, reads the journals every 2 seconds while it
waits, decides by itself when the installation is over, and stops the trace.

**Launch** (`launch_and_watch`):
- The installer runs in a job object, like an uninstaller. It is created suspended and resumed
  once it is in the job, so none of its children can start outside.
- A `.msi` runs as `msiexec.exe /i`. MSIX and AppX packages (and their bundles) are refused:
  Windows installs and removes them itself.
- It starts in its own folder, with `NUL` for its standard input, output and error, the only
  handles it inherits. Otherwise Windows hands a console installer Avel's own, and what a setup
  script's tools print would end up in the JSON `trace run --json` prints.

**Following its processes.** The job reports new processes. Every poll (500 ms) a process snapshot
also finds the processes that tracked ones started, wherever they run, which catches children that
broke away from the job (WiX Burn's engine). A child counts only if it started after its parent,
and every tracked process is held by a handle, so a reused process id can't mislead the watch.
Console hosts are ignored.

**What holds the trace:**
- the installer itself, while it runs;
- its **helpers**, while they run: `msiexec`, programs in a Temp folder (every user's, Windows'
  `Temp` and `SystemTemp`), in `C:\Windows\Installer` or in `%ProgramData%\Package Cache`, programs
  named `*.tmp`, and programs in the folders installers unpack themselves to (`*.tmp` such as Inno
  Setup's `is-*.tmp`, NSIS's `~nsu*`, Burn's `.cr` and `.be`);
- **any other process**, only until 10 seconds after both the installer's exit and the moment it
  was first seen. After that it counts as a program the installer started for the user ("Run X
  now"): it is listed under `left_running`, and never waited for or ended.

**Windows Installer busy:** the mutex `Global\_MSIExecute` exists and `NtQueryMutant` says a thread
owns it. The mutex is opened only to query it, never waited on or taken, so the watch can't make a
concurrent installation fail with 1618. If it exists but can't be queried, Windows Installer counts
as busy.

**When the installation is over:**

| Phase | Waiting for |
|---|---|
| `running` | The installer to exit |
| `waiting-for-helpers` | Its helpers (and programs still within their 10 s) to exit |
| `waiting-for-windows-installer` | Windows Installer to become idle |
| `settling` | All of the above to hold for 5 s (a new process starts the 5 s over), then the disk to settle |

The disk has settled after 2 seconds without journal records outside the folders Windows writes to
all the time (its Temp, Prefetch, logs, `config` and `sru`, Defender's folders, Avel's data, each
profile folder with its `Local\Microsoft\Windows` and `Temp`, and folders created in them during
the trace), or 20 seconds after the quiet period at most. This is a heuristic: writes deeper
inside those folders still count.

**Ending early.** Ctrl+C stops the trace at once and leaves the installer running; a second Ctrl+C
also ends every process in the installer's job. `--timeout` stops the trace that long after the
launch, and leaves the installer running too. An installer that can't be started leaves no trace:
it is discarded.

## Restarts

Everything a trace needs is on disk from the moment it is open: `trace.json`, the snapshot, the
saved values and a checkpoint per volume. So:
- **The installer asks for a restart** (exit code 3010 or 1641): `trace run` leaves the trace open,
  with the stop reason `reboot-pending`, and says "Restart Windows, then run `avel-cli trace stop`".
  What the installer still does during the restart belongs to the trace.
- **After a restart, or from another process,** `trace stop` resumes the trace from its
  checkpoints (`TraceSession::resume`), reads the journals on from there and finishes it. A volume
  whose journal wrapped or was replaced in the meantime makes the trace incomplete; the rest is
  still reported.
- **A killed `trace run`:** the trace's lock file goes with the process, and `trace stop` finishes
  the trace.
- **A crash while stopping:** a trace left `stopping` has everything it needs in its files (the
  last read and its checkpoints happened before), so stopping it again repeats the comparison
  without reading the journals.
- **A crash while starting:** the trace has nothing to stop, and is removed.

Nothing reminds the user of an open trace yet; the GUI will ask at startup (5c).

## Storage (`trace_store`)

Each trace is a folder `%ProgramData%\Avel\Traces\<YYYYMMDD-HHMMSS>-<8 hex digits>\` (the time in
UTC, so ids sort by age). The root gets the same treatment as the ChangeStore's
([ADR 0004](adr/0004-changeset-folder-journal.md)): SYSTEM and Administrators only, and refused if
it is a link, resolves elsewhere or has another owner. The mutex `Global\Avel.TraceStore`
serializes processes, and a process that drives a trace holds its `lock` file open.

| File | Holds | Kept |
|---|---|---|
| `trace.json` | State, mode, description, command, times, stop reason, volumes with their journal positions, registry counts and diagnostics, the watch's result, counts. Replaced atomically on every update | Always |
| `registry.snapshot` | The registry at the start | Until the trace is finished |
| `saved_values.json` | The known spots' values at the start | After stop, only the values that changed |
| `volume-<n>.checkpoint` | Each volume's position and what was read from it | Until the trace is finished |
| `changes.json` | The changes, noise included, and the known-value changes | Once stopped |
| `lock` | Nothing; held open by the process driving the trace, deleted when it closes | While held |

**States:** `starting` (being set up), `open` (recording), `stopping` (read to the end, comparison
under way), `complete`, or `incomplete` (a volume or part of the registry couldn't be compared).

**Rules:**
- Only one trace can be starting, open or stopping at a time.
- Every `avel-cli trace` command first tidies the store (`purge`). It removes traces that died
  while starting and trace folders whose `trace.json` is missing or unreadable, and finishes
  deleting the snapshot and checkpoints of finished traces whose process died before it could. It
  touches only folders named like traces, leaves a `trace.json` of another schema version alone,
  and never touches a trace that is open, stopping or held.
- Finished traces are kept until `trace delete`. Retention by program links comes with linking in
  5b: a trace is kept while its program is installed, then 30 days after it's uninstalled.

## Limits

- **No driver, so the whole system is recorded.** Changes other processes make during the trace
  are recorded too, unless the noise filter knows them. That is also how msiexec's service and
  other helpers are caught. Keep traces short: `trace run` ends by itself.
- **Volumes.** Only fixed NTFS volumes with a drive letter are traced. FAT32, exFAT and ReFS
  volumes, removable drives and volumes mounted only in a folder aren't, and don't appear in the
  trace's `volumes`. ADR 0002's fallback for them was dropped (ADR 0006).
- **Files kept open.** The journal records each kind of change to a file once per open, when it
  first happens. A file that was opened and written before the trace and stays open (a database a
  service holds) gets no new record for writes during the trace until it is closed, so those
  writes are missed.
- **Manual traces aren't drained.** Between `trace start` and `trace stop` no Avel process reads
  the journals, so a long manual trace on a busy volume can wrap its journal (reported as
  `wrapped`).
- **Registry coverage.** The snapshot covers its roots only. Elsewhere in `HKLM\SYSTEM` only the
  machine environment is seen, as known values. The subtrees left out as churn are never compared.
- **Users.** The snapshot and the known spots cover the users signed in when the trace starts. A
  user who signs out during the trace is compared from their hive files; one who signs in during
  it isn't compared at all.
- **The installer runs as the elevated administrator** (like uninstallers), so a per-user install
  through `trace run` lands in that administrator's profile and hive. To trace an install as
  another, signed-in user, use `trace start` and `trace stop` around it.
- **Signed-out users' classes.** A user's classes are reported under `HKU\<sid>\Software\Classes`.
  The trace can compare them after the user signed out, since it loads `UsrClass.dat` itself, but
  the leftover scan and the executor reach that path only while the user is signed in.
- **Firewall rules** are reported, never removed, even when traced.

## The CLI (`avel-cli trace`)

```
avel-cli trace start  [--description <text>] [--json]
avel-cli trace stop   [--json]
avel-cli trace run    [--description <text>] [--timeout <seconds>] [--json] -- <installer> [<args>...]
avel-cli trace list   [--json]
avel-cli trace show   <id> [--all] [--json]
avel-cli trace cancel
avel-cli trace delete <id>
```

- **General rules:**
  - All commands need administrator rights.
  - Only one trace can be open at a time.
  - Progress goes to stderr, one line per step; none with `--json`.
  - Exit codes:
    - 0: success. An incomplete trace still exits 0, with a warning on stderr.
    - 1: failure.
    - 2: usage error.
- **`start`:** opens a trace.
  1. It records each volume's journal position, saves the known values and takes the registry
     snapshot, which takes a few seconds.
  2. It returns while the trace stays open. The trace survives restarts.
- **`stop`:** finishes the open trace and prints the result. It also finishes a trace that
  `run` left open for a restart, or whose process was killed.
- **`run`:** launches the installer and decides by itself when the installation is over:
  - the installer has exited;
  - its helper processes have exited (another program it started counts for 10 seconds only);
  - Windows Installer is idle;
  - all of that has held for 5 seconds, with no new process;
  - the journal has settled.

  It then stops the trace and prints the result. A program the installer launches at the end ("Run X now") doesn't hold the trace; it's listed under `left_running`.
  - Everything after `--` is the installer's command line. A `.msi` runs through `msiexec /i`.
  - The first Ctrl+C stops the trace at once and leaves the installer running.
  - A second Ctrl+C also ends the installer.
  - `--timeout` stops the trace that long after the launch (stop reason `timed-out`) and leaves the installer running.
  - If the installer asks for a restart (exit code 3010 or 1641), the trace stays open: restart, then run `trace stop`.
  - If the installer can't be started, the trace is discarded and `run` exits 1.
- **`list`:** shows every trace, newest first.
- **`show`:** prints a trace's result. Noise is hidden unless `--all` is given. For a trace that
  isn't finished, only the trace itself.
- **`cancel`:** discards the open trace, or one that died while starting.
- **`delete`:** deletes a finished trace.

```
avel-cli trace run --description "7-Zip" -- C:\Installers\7z.msi /qn
avel-cli trace start --description "Tool X"     # then install it, by hand or as another user
avel-cli trace stop
avel-cli trace show 20261023-210501-1a2b3c4d --all
```

### JSON

`stop`, `run` and `show` with `--json` print one object:

```json
{
  "avel_version": "0.5.0",
  "trace": {
    "id": "20261023-210501-1a2b3c4d",
    "state": "complete",
    "complete": true,
    "incomplete_reasons": [],
    "mode": "launch",
    "description": "7-Zip",
    "command": "C:\\Installers\\7z.msi /qn",
    "started": "2026-10-23T21:05:01Z",
    "stopped": "2026-10-23T21:05:40Z",
    "stop_reason": "installer-done",
    "volumes": [ { "root": "C:\\", "status": "ok", "created_journal": false } ],
    "registry": { "keys": 312004, "values": 801233, "snapshot_ms": 1840, "diff_ms": 410 },
    "watch": { "end": "installer-done", "exit_code": 0, "reboot_requested": false,
               "left_running": [ { "pid": 4242, "image": "C:\\Program Files\\X\\x.exe" } ] },
    "counts": { "changes": 12, "noise": 85 }
  },
  "changes": [
    { "change": "created", "location": { "kind": "folder", "path": "C:\\Program Files\\7-Zip" },
      "contents": 104, "noise": "none" },
    { "change": "renamed", "location": { "kind": "file", "path": "C:\\x\\new.txt" },
      "old_path": "C:\\x\\old.txt", "noise": "none" },
    { "change": "modified", "location": { "kind": "registry-value", "key": "HKLM\\SOFTWARE\\X", "value": "Theme" },
      "noise": "none" }
  ],
  "known_values": [
    { "spot": "machine-environment",
      "key": "HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
      "name": "Path",
      "before": { "type": 2, "data": "<hex of the exact bytes>" },
      "after":  { "type": 2, "data": "<hex>" },
      "before_text": "C:\\Windows\\system32;...", "after_text": "C:\\Windows\\system32;...;C:\\X\\bin" }
  ]
}
```

- **`trace`:**
  - `state`: `starting`, `open`, `stopping`, `complete` or `incomplete`. `complete` is true for `complete` only.
  - `mode`: `manual` or `launch`.
  - `command`: for `launch`, the installer (its full path) and its arguments; empty for `manual`.
  - `started` and `stopped` are UTC; `stopped` is `null` until the trace is stopped.
  - `stop_reason`: `null` while open, then `manual`, `installer-done`, `stopped`, `timed-out`, or `reboot-pending` (the trace stays open; `trace stop` later records `manual`).
  - `volumes[]`: one entry per fixed NTFS volume with a drive letter. `root` is its drive letter now, `created_journal` says the trace created its journal.
  - `volumes[].status`: `ok`, `untracked`, `volume-missing`, `serial-changed`, `replaced`, `wrapped` or `unreadable`.
  - `registry`: the snapshot's keys and values, how long it took, and how long the comparison took (0 until stopped).
  - `incomplete_reasons`: one `{ "volume", "status" }` entry per volume that isn't `ok`, plus registry entries:
    - `{ "registry": "<code>", "key", "error" }` for a part of the registry that wasn't compared, where the code is `root-unavailable`, `key-unreadable` or `key-unreadable-at-start` and `error` a Win32 error code (0 if none);
    - `{ "registry": "snapshot-damaged" }`: the snapshot couldn't be read, so the registry wasn't compared;
    - `{ "registry": "saved-values-damaged" }`: the saved values couldn't be read, so the known values weren't compared.
  - `watch`: only for traces `run` started. `end` is `installer-done`, `stopped`, `timed-out` or `launch-failed`; `exit_code` is the installer's (0 while it still runs); `reboot_requested` means it exited with 3010 or 1641; `left_running` lists its processes still running at the end (after `installer-done`, the programs it started).
  - `counts`: changes that aren't noise, and noise.
- **`changes`:** without `--all`, only items with `noise` = `none`. Files come first, volume by volume, then the registry, each in path order.
  - `change`: `created`, `deleted`, `modified` or `renamed`.
  - `location`: the same object `scan --json` prints, so `kind` is one of `folder`, `file`, `registry-key`, `registry-value`, `service`, `driver`, `task`, `package` or `firewall-rule`.
  - `old_path`: for `renamed`, where it was at the start.
  - `contents`: for created folders and keys, how many items were created inside them (also for created services and tasks: the keys and values inside). Only the outermost created item is listed.
  - `replaced`: true for a modified file that a new file replaced.
  - `noise`: `none`, `system-activity`, `user-activity`, `temporary`, `avel`, `unresolved` or
    `metadata-only`.
- **`known_values`:** the values the installer changed in the known spots.
  - The known spots: the machine's and each user's `Environment`, and both `SharedDLLs` keys.
  - `spot` is `machine-environment`, `user-environment` or `shared-dlls`.
  - `key` is the display path.
  - `before` and `after` are `null` when the value didn't exist.
  - `*_text` is the string form, for string types only (`REG_SZ`, `REG_EXPAND_SZ` and `REG_MULTI_SZ`, whose strings are joined with `\n`); it's left out for other types.

`list --json` prints an array of `trace` objects without `changes`. `start --json` prints
`{ "avel_version", "trace" }` with `state` `open`. So do `run --json` when the trace stays open for a
restart, and `show --json` for a trace that isn't finished yet.

## Testing

- **Unit tests** (311 in all; 306 pass on an unelevated host, and the 5 that need administrator
  rights run in the Sandbox):

  | Test | Covers |
  |---|---|
  | `UsnParseTest` | Version 2 and 3 records, ids that don't fit 64 bits, unknown versions, records running past the buffer |
  | `UsnLiveTest` (elevated) | A real temporary folder: net changes read and resolved, reading up to a given position, folders named by their ids |
  | `FileChangesTest`, `FileChangeCheckpointTest` | A made-up resolver: created then deleted, the kinds of replacement (an older file renamed over another, also one open since before the trace, and two swapped), metadata-only changes as noise, a file deleted while still open, one deleted that was open since before the trace, one deleted before the trace, a container's placeholders (and what only looks like them) and tombstones, renames, a renamed ancestor and a folder moved as a whole, hard links, a reused record, roots with their contents, moved-in folders, items open across the start, unknown folders; the checkpoint round trip and damage |
  | `JournalTraceTest` | Replaced, wrapped, formatted and unreadable journals, read failures, a changed drive letter, a missing volume, NTFS metadata, checkpoints |
  | `RegistrySnapshotTest` | Scratch-key roots: created, deleted and modified keys and values, a rewrite with the same data, links not followed, odd names, the file round trip, a damaged file, one thread against eight, services, tasks and firewall rules, churn and nested roots left out (packages' and the Start menu's state read), keys that deny access, a missing root |
  | `RegistrySnapshotSmokeTest` | The live `HKLM\SOFTWARE` and current user, read-only: sizes and times, and an immediate comparison that finds almost nothing |
  | `KnownValuesTest` | Added, changed and deleted values, unreadable keys left out, exact JSON |
  | `NoiseFilterTest` | Each user's own folders, Windows' folders, keys and self-changing values, what the Sandbox's idle round found Windows keeping up to date, Avel's own, never-noise keys, rules for one user, a whole trace |
  | `InstallerWatchTest`, `InstallerHelperTest`, `WatchDeciderTest` | Real processes through `avel_test_spawner`: a program left running, a helper in Temp, a child that broke away from the job, a busy Windows Installer (injected), stopping, ending the job, the timeout, a restart request, nothing written into Avel's own output, refused packages; which programs are helpers; the decision rules on made-up polls |
  | `TraceStoreTest`, `TraceRecordTest` | One open trace, traces that died while starting, held traces, purge and tidying; `trace.json` and `changes.json` round trips |
  | `TraceSessionTest`, `TraceSessionLiveTest` (elevated) | Made-up journals and scratch keys: start and stop, activity, resuming from another session, draining in the background, stopping again after a crash, a damaged snapshot or checkpoint, a volume that can't be followed, cancel; and one real trace, and the live noise rules finding the current user's profile |
  | `InstallTraceTest` | The trace stops when the installation is over, stays open on a restart request, and leaves nothing when the installer can't start or the user stops first |
  | `HashTest`, `NtRegistryTest`, `RecordFoldersTest` | FNV-1a's test vectors, exact values through the native API, record ids |
- **Windows Sandbox, scenario `trace`** ([tests/sandbox/README.md](../tests/sandbox/README.md)),
  checked against an independent oracle. `avel_capture` (`tests/helpers/capture.cpp`) shares no
  code with Avel. With plain Win32 calls it lists every folder and file on `C:` (size and last
  write time) and every key and value (type, size, hash) below `HKLM\SOFTWARE`, `Services`, the
  machine environment, and the user's `Software`, `Environment` and classes. A capture right
  before and right after each traced installation, diffed, is what really changed. The scenario
  keeps its coarse exclude lists as data and logs how many lines each pattern took out.

  In rounds 0, 0b and 1 both captures are taken while the trace is open (into Temp, which the trace
  sets aside), the second after 15 seconds for Windows to react, so every line of the diff
  happened inside the trace; but an item can change again inside the trace outside the captures
  (WMI's performance adapter writes `WmiApRpl_new.h` and renames it over `WmiApRpl.h` 3 seconds
  later). Such a line is logged as KNOWN only with evidence: a journal record of its name in its
  folder within the trace's own USN window (from its `trace.json`) after the second capture began
  that takes the name away or brings it back, or before the first one that created or removed it;
  for a key or value gone or back, its key's last write time between the second capture and the
  stop. Around `trace run` they can't be: a line is put down to the moments
  between a capture and the trace (logged as KNOWN) only with evidence that it changed then, its
  creation time, its last write in the capture, its key's last write time, or a watch of the
  volume's names (`FileSystemWatcher`) that saw it go away (for a deletion) or appear (for a file
  moved in, which keeps its creation time) then, by itself or with a folder above it. A folder
  moved as a whole takes what it holds along with no name event or journal record of their own
  (Edge's component unpacker fills a folder in its Temp and moves it into place), so the folder's
  event counts only while the watch noted nothing more of the folder or the item after it. A move
  inside the trace is reported as the folder renamed, and its old path accounts for everything
  below it. The trace's window comes from its `trace.json`:
  the journals from `trace_start`, the registry from the end of the snapshot, until the second it
  stopped in (the comparison with the snapshot reads the registry after that). In any round, a
  created or gone file or folder that the volume's journal has no record of between the two
  captures is logged as KNOWN: nothing on the volume made it. That is no record of its id when it
  is there now. When it isn't there now, there is no record of its name in its folder, and none of
  that folder or one above it being created, deleted or renamed. A container's base-layer folder
  can be missing from one capture's listing that way: in the Sandbox, `Branding\shellbrd\en-US` went
  from the listings, and `System32\PerceptionSimulation\en-US` showed in one and was gone again
  seconds later. The journal had no new item of that name after the first capture's placeholders,
  and the trace saw nothing of either.

  | Round | What it does | Checks |
  |---|---|---|
  | Unit | The elevated unit tests, and the user profile's | All pass, none skipped (`KeysThatDenyAccessAreUnreadable` is left out: elevated, backup intent reads every key) |
  | 0 | Idle: `trace start`, a minute with a stock app opened and closed (once before the trace too: its first launch caches its name in Explorer's `MuiCache`, never noise), `trace stop` | Complete, and nothing that isn't noise. What the trace doesn't account for fails here and is logged as KNOWN later |
  | 0b | A container's base layer: two of Windows' sounds, one deleted and one renamed after `takeown` and `icacls`, between `trace start` and `trace stop` | Exactly that deletion and rename in the folder, no tombstone reported as created; completeness and strict precision |
  | 1 | `Contoso Trace Fixture` between `trace start` and `trace stop`: one item of every kind, plus edits to both `Path` values, a new machine variable, an existing `SharedDLLs` count raised and a new one, and an unrelated value | Completeness, strict precision, every expected item (the program's folder as one item), and the known values' exact bytes |
  | 2-4 | 7-Zip (MSI), Notepad++ (NSIS) and Inno Setup through `trace run` | Ends by itself (`installer-done`), complete, the installer's exit code; precision exceptions are logged |
  | 5 | The fixture through `trace run`, starting its app at the end | Done within 45 s, the app under `left_running` and still running |
  | 6 | A `trace run` killed while its installer runs; a journal deleted and created again during a trace | `trace stop` finishes the killed run's trace; the other ends `incomplete` with `C:` `replaced`, its registry part still reported |

  How a round is judged:
  1. **Completeness:** every line of the oracle's diff that the exclude lists leave in is accounted
     for by a reported change of the same kind, noise included: `+` by a created item, `~` by a
     modified one, `-` by a deleted or renamed one, or a known value. A created folder or key
     accounts for everything below it; a task for its keys and its file in `System32\Tasks`. A line
     reported as another kind of change fails.
  2. **Precision:** every reported change that isn't noise is in the oracle's diff, with the right
     sign. Items outside the oracle's roots or exclusions are logged. Services, tasks and firewall
     rules are checked item by item instead.
  3. **Contents:** each created root's `contents` is compared with the oracle's lines below it
     (logged).

  Rounds 1 to 5 keep their traces and captures for 5b, which removes the programs by their traces
  and checks the result against the pre-install capture.
