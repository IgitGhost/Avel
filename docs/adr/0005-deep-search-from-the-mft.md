# ADR 0005: The deep search reads folders from the master file table

- **Status:** Accepted
- **Date:** 2026-10-21

## Context
The leftover scan walks a few common locations, up to three levels deep: Program Files,
ProgramData, the Start Menu, each user's AppData (see [leftovers.md](../leftovers.md)). Programs
also leave folders elsewhere: a tool unpacked to `C:\Tools`, a game library on `D:\Games`, a
portable copy on a second disk. Phase 4b adds an opt-in **deep search** (`ScanOptions::deep`,
`--deep`) that looks for folders named after the program anywhere on the fixed NTFS volumes.

That needs the name and parent of every folder on every fixed volume. A volume holds a few
hundred thousand folders and a few million files.

## Options
1. **Walk the tree** with `FindFirstFileExW` (`FindExInfoBasic`, `FIND_FIRST_EX_LARGE_FETCH`).
   - It needs no rights beyond reading.
   - It is one directory open per folder, scattered across the disk, so it takes tens of seconds
     on a large volume, and minutes on a spinning one.
   - Folders the user can't list are silently missing.
2. **Enumerate the master file table** with `FSCTL_ENUM_USN_DATA` on the volume handle, as
   Everything and WizTree do.
   - It is one sequential pass over the volume's index, returning a record per file (its file
     reference number, its parent's, its name and attributes) in large buffers.
   - It needs administrator rights (the volume is opened for reading).
   - It is NTFS only, and gives no full paths: those are rebuilt from the parent numbers.
3. **Windows Search's index.** It is often turned off, covers only indexed locations, and its
   freshness is unknown.

## Decision
Option 2, in `platform::VolumeFolders`:
- `read(root)` opens `\\.\X:`, takes the root folder's file reference number from
  `GetFileInformationByHandle` on `X:\`, and runs `FSCTL_ENUM_USN_DATA` (`MFT_ENUM_DATA_V0`,
  1 MB buffers) to the end, keeping folders only.
- `path_of(i)` rebuilds a path on demand by following parent numbers up to the root. Folders
  that don't lead there (NTFS's own metadata, records of folders being deleted) have no path,
  and a loop stops after 512 levels.
- The deep scanner (`leftovers/deep_scanner.cpp`) reads every fixed NTFS volume on its own thread,
  while the other snapshots are taken. It groups folders by name, so each distinct name is
  matched once per program, and builds paths only for the names that match.

Without administrator rights, or on a volume that can't be read, the scan reports
`deep-search-unavailable` for that volume and goes on without it. The search is never silently
partial.

## Consequences
- **Fast.** Reading the volumes overlaps the rest of the scan; `--json` and `--timing` report it
  as the stage `deep`. In the Sandbox (a fresh Windows) it took 16 ms, and the whole scan 217 ms.
  The development machine's volumes can't be measured from its unelevated shell.
- **Admin only.** The GUI and the CLI run elevated for removals anyway; an unelevated preview
  says the drives couldn't be searched.
- **NTFS only.** FAT and exFAT volumes (removable media, mostly) aren't searched; ReFS would need
  `USN_RECORD_V3` and isn't supported yet.
- **A snapshot.** Folders created or renamed during the scan may be missed or show stale names.
  The executor opens each item again when it removes it, and skips one that is gone (`missing`)
  or whose final path is no longer the recorded one (`path-changed`), so a stale path can't remove
  something else.
- **Tests.** Reading a real volume needs elevation, so `VolumeFoldersTest.ReadsTheVolumeOfATemporaryFolder`
  skips on an unelevated host and runs in the Sandbox scenario `force`. The path rebuilding and
  the scanner are tested on made-up records (`ScanEnvironment::volumes`).
