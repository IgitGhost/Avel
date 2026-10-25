# ADR 0006: Trace folders, the registry snapshot and the known spots

- **Status:** Accepted. Amends [ADR 0002](0002-install-tracker-without-driver.md).
- **Date:** 2026-10-24

## Context
ADR 0002 fixed how Avel Trace works: a diff of the system before and after an installation, with
no driver, files from the USN journal and the registry from a LastWriteTime-pruned snapshot.
Phase 5a builds it ([trace.md](../trace.md)). Four details were left open or turned out wrong:
- **Where traces live.** ADR 0004 kept SQLite in mind for "Phase 5's trace logs".
- **What the snapshot hashes.** ADR 0002 stores "a hash of its values" per key.
- **Shared values.** An installer appends to `Path` or raises a DLL's `SharedDLLs` count. The value
  existed before and other programs depend on it, so removal must take back exactly the
  installer's part.
- **The USN journal** may be missing, or small, and **non-NTFS volumes** have none. ADR 0002 gave
  those a directory snapshot of the install location.

## Decision

**1. A trace is a folder of JSON and binary files, not a database.** Each trace is a folder under
`%ProgramData%\Avel\Traces`, made like a ChangeSet's (ADR 0004) and sharing its mechanics
(`platform/record_folders`: time-sorted ids, a self-deleting `lock` file, listing; the same
protected, checked root and a named mutex):
- `trace.json`, the trace's state and summary, replaced atomically on every update;
- `changes.json`, the result, written once when the trace stops;
- `saved_values.json`, the known spots' values;
- `registry.snapshot` and a checkpoint per volume, binary, deleted once the trace is finished.

A trace is written once and read whole. Nothing queries across traces: listing reads each
`trace.json`. Crash safety comes from writing every file durably before the state that needs it
(the snapshot is on disk before the trace counts as open), and the files can be read without
Avel. SQLite would have been a new dependency for none of that.

**2. The registry snapshot is a binary file with a hash per value.** A per-key hash says that
something in a key changed, but not which value, nor whether it was added or changed; the diff
would have to report whole keys. So the snapshot keeps:
- per key (32 bytes): its name, flags (link, unreadable), LastWriteTime, and where its children and
  values are;
- per value (16 bytes): its name, its type, and an FNV-1a 64-bit hash of its type, length and exact
  bytes (`platform/hash.h`, header-only);
- one deduplicated UTF-16 name pool.

The file has a magic, a version and an FNV-1a checksum, and every index is checked on reading. On
the development machine about 500,000 keys and 790,000 values take about 48 MB, 3.2 to 3.5 s to
take with 8 threads, and 1.4 to 1.8 s to compare. The plan's fallback, compressing the file if it
passed 64 MB, wasn't needed. JSON would be several times larger and slower to write before the
trace can start.

FNV-1a is not a cryptographic hash: a value crafted to collide could hide a change. An installer
already runs as an administrator and can do worse, so this is accepted.

**3. The known spots are saved exactly.** A hash can tell that `Path` changed, not what to take
back. At the start and at the stop, the trace saves the exact bytes of every value in:
- `Session Manager\Environment` and each signed-in user's `Environment`;
- both `SharedDLLs` keys (64-bit and `WOW6432Node`).

Every value added, changed or deleted is a known-value change with its exact before and after;
after the stop, `saved_values.json` keeps only those values. The environment keys lie outside the
snapshot's roots and the snapshot leaves the `SharedDLLs` values out, so these values never appear
as raw registry changes.

**4. A missing USN journal is created; an existing one is never resized.** A volume without a
journal gets one of 64 MB (8 MB allocation delta) and keeps it; the trace reports
`created_journal`. An existing journal's size is a setting other software relies on, and Avel
doesn't change system settings behind the user's back. Instead, a process that drives the trace
reads the journals every 2 seconds and checkpoints what it read, so a journal wraps only during a
long manual trace on a busy volume. A wrapped or replaced journal makes the trace incomplete for
that volume, and says so; it is never hidden.

**5. ADR 0002's fallback for non-NTFS volumes is dropped.** A directory snapshot of the install
location can't work: the location is known only once the installation is over, and a snapshot of
one folder misses everything outside it. FAT32 and exFAT volumes are mostly removable media,
rarely install targets. Only fixed NTFS volumes with a drive letter are traced; the others are
left out of the trace's `volumes`, and an NTFS volume whose journal couldn't be created is
reported as `untracked`.

## Consequences
- **No new dependency,** and traces can be inspected or deleted by hand. The cost is no queries
  across traces, which nothing needs.
- **Exact diffs.** Added, deleted and modified values are told apart, and a value rewritten with
  the same data is no change. The snapshot is deleted once the trace is finished; only the result
  and the changed known values stay.
- **ADR 0004's remark** that SQLite "remains the choice for Phase 5's trace logs" no longer holds.
- **Value edits come in 5b.** The saved before and after values let removal take back exactly the
  installer's part: remove its `Path` entries from the live value, keeping the user's later edits;
  delete a variable it created; subtract its `SharedDLLs` increment. Each edit is a new
  `ValueEditLocation`, applied only if the live value is still the one expected (compare-and-set),
  and undone exactly or by the inverse operation. These keys are protected today. The hard floor
  gets a narrow allow-list for value edits only: `Session Manager\Environment`, the users'
  `Environment` and the `SharedDLLs` keys. Everything else stays protected.
- **Retention follows the program in 5b.** A trace is kept while its program is installed, then 30
  days after it's uninstalled. Until then, finished traces stay until deleted.
- **Untraced volumes.** Files installed to a non-NTFS volume aren't recorded. Listing such volumes
  in `volumes` as `untracked` too would make that visible.
