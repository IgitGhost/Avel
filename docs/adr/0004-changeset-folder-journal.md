# ADR 0004: ChangeSets are folders with a JSON plan and an append-only journal

- **Status:** Accepted
- **Date:** 2026-10-19

## Context
Phase 3b removes leftovers, and every removal must be undoable exactly. Each removal is grouped
in a **ChangeSet**, which holds:
- the plan (what is to be removed);
- each item's state as the removal goes;
- the backups: registry trees, service and task definitions;
- the files moved to quarantine.

The original plan put this in SQLite. Two properties matter more than queries:
- **Crash safety.** A power cut or a killed process mid-removal must leave a state that can be
  recovered: every item either still in place, or removed with its backup intact.
- **Inspectability.** When something goes wrong, the user (or a bug report) should be able to
  see exactly what was removed and from where, without Avel.

## Decision
Each ChangeSet is a self-contained folder under `%ProgramData%\Avel\Changes`:

| File | Written | Holds |
|---|---|---|
| `plan.json` | Once, before anything changes | Schema version, id, time, description, the items |
| `journal.jsonl` | One line per state change, flushed to disk each time | Item index, state, details |
| `summary.json` | At the end, replaced atomically (`ReplaceFileW`) | Totals and the set's state |
| `items\NNNN\` | Before item NNNN changes | Its backup (`key.json` + a readable `key.reg`, `service.json`, `task.xml`, ...) |
| `files\NNNN` | When item NNNN is moved | The quarantined file or folder (system volume) |
| `lock` | Held open exclusively while the set runs | Nothing; its lock marks a live run |

On other fixed NTFS volumes, quarantined items go to that volume's `X:\$Avel.Quarantine\<set>`,
so a move is always a rename on the same volume, never a copy.

The journal's last line may be torn by a crash, so readers ignore a line that doesn't parse. An
item's state is the last journal line about it. A backup is written and flushed before the
journal says "backed-up", and only then does the item change.

Store roots get a protected DACL (SYSTEM and Administrators only). A root that is a reparse point,
resolves elsewhere, or has another owner is refused. A named mutex serializes the GUI and the CLI.
Sets older than 30 days are purged at startup.

## Consequences
- **No new dependency.** nlohmann-json is already used by the CLI; the core links it privately.
- **Recovery is simple.** A set with a stale `lock` and items that are "backed-up" but not "done"
  is reconciled by looking at the system: if the item is still in place it wasn't removed;
  otherwise its backup is the truth.
- **Readable.** A set can be inspected or restored by hand: `.reg` files, plain folders, JSON.
- **No queries across sets.** The Undo Center lists sets by reading each `summary.json`, which is
  cheap for the hundreds of sets 30 days can hold. SQLite remains the choice for Phase 5's trace
  logs, where the volume and the queries justify it.
