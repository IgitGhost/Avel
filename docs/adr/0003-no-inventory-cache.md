# ADR 0003: No inventory cache at startup

- **Status:** Accepted
- **Date:** 2026-10-18

## Context
The plan called for an "instant startup": on launch, show the inventory cached in SQLite from the
last run, then refresh in the background and reconcile the two lists. The reason was the expected
cost of a cold scan: registry, Windows Installer and packaged apps together, for about 300 programs.

Phase 1 measured the real cost (see [inventory.md](../inventory.md#performance)):
- The whole scan takes 100–120 ms, with registry, MSI and AppX running in parallel. The slowest is
  Windows Installer at about 100 ms.
- It takes about 220 ms on the first run after boot.
- The list appears 0.42–0.87 s after the process starts, so the "under 1 s cold" goal is met
  without a cache.

## Decision
Don't cache the inventory. Every start and every refresh (F5) runs a full scan on a worker thread,
and the window appears before the scan finishes.

## Consequences
- **Always current.** A cached list would be wrong exactly when it matters, right after something
  was installed or removed outside Avel. No code is needed to reconcile a stale list with a fresh one.
- **No SQLite dependency yet.** It arrives with the change journal and backups (Phase 3), where
  storage is actually needed.
- **Slower enrichment stays off the startup path.** Some work could push the scan past ~500 ms:
  real folder sizes, Authenticode signers, Steam/Epic libraries, winget. It will run as background
  enrichment that updates rows after the list is shown, the same way icons already load. If even the
  base scan becomes slow, this decision is revisited.
