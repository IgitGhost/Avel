# ADR 0002: Install tracker without a kernel driver

- **Status:** Accepted
- **Date:** 2026-10-18

## Context
Avel Trace records what an installer changes so the program can later be removed exactly. Existing tools use either:
- **real-time kernel drivers** (a file-system minifilter plus registry callbacks). This is precise for the installer's process tree, but needs driver signing, misses changes made by other processes on the installer's behalf (msiexec's SYSTEM service, services and COM servers it starts), and a driver bug can crash the system; or
- **full before/after snapshots.** These catch everything, but they are slow, and they pick up changes from unrelated software.

## Decision
Use a **snapshot diff with no driver**, made fast by two Windows features:
- **Files: the NTFS USN change journal.**
  - At start, record each fixed NTFS volume's journal ID and next USN.
  - At stop, read the journal from that point (`FSCTL_READ_USN_JOURNAL`) to get every created, modified, renamed or deleted file, then resolve their paths.
  - If the journal ID changed or the start position was overwritten, flag the trace as incomplete.
- **Registry: a LastWriteTime-pruned snapshot.**
  - At start, store each key's LastWriteTime plus a hash of its values for HKLM\SOFTWARE, `SYSTEM\CurrentControlSet\Services` and every user's `Software` hive.
  - At stop, walk the keys again, but read values only where LastWriteTime changed.
- **Noise filter:** drop Temp, Prefetch, logs, browser caches and Avel's own files. The user reviews the rest.
- **Linking:** the trace is attached automatically to the program whose new Uninstall key appears in the diff.

## Consequences
- No driver, no signing, no risk of a system crash. Changes made by msiexec's service and other helper processes are captured because the method is system-wide.
- Unrelated activity during the tracing window still shows up. Mitigations: the noise filter, short tracing windows that end automatically when the installer's processes exit, and user review.
- Applies to NTFS volumes only (FAT/exFAT have no USN journal). Those fall back to a directory snapshot of the install location.
- A kernel-driver mode can still be added later as an optional backend if real-world noise proves too high.
