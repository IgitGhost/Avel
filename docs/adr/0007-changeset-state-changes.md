# ADR 0007: State changes in the ChangeSet

- **Status:** Accepted. Extends [ADR 0004](0004-changeset-folder-journal.md).
- **Date:** 2026-11-02

## Context
Until Phase 6 the ChangeSet removed things and put them back. Value edits (Phase 5) were the first
items that change a value instead of removing it, and they set the pattern: the plan carries the
value it expects and the value it writes, and the executor writes only while the value is still
the one expected (compare-and-set).

Avel Tools (Phase 6) **change an item's state** and must put the old state back:
- the Startup manager turns a Run value or a Startup folder item off and on again through
  Explorer's `StartupApproved` record (what Task Manager does), disables and enables a scheduled
  task, and sets a service from Automatic to Manual or Disabled;
- 6b adds package removal modes and Windows features.

The user, another program or Windows may change the same item between the plan and the change, or
between the change and its undo. A tool that overwrote that would undo the user's own choice, or
turn back on what an update turned off.

## Decision

**1. One pattern for every state change.**
- The plan records the item's state before (`before`, or `expected`) and the state wanted
  (`after`, or `desired`).
- **Execute** reads the live state right before the change. It applies `after` only while the
  live state equals `before`; otherwise the item is `skipped (changed)` and nothing is written.
  The state before and the state written go to `items\NNNN\state.json`, flushed before the
  journal says `backed-up`, as for every item.
- **Undo** applies `before` only while the live state equals what was written. Otherwise the item
  changed since and stays as it is: `restore-skipped (changed)`, shown as "its state has changed
  since". An item that is gone since (a deleted task or service) is `restore-skipped (missing)`.
- **Recovery** settles a `backed-up` item by looking at it: `done` if its live state is what the
  change writes, `failed (interrupted)` otherwise (untouched, or changed by someone else, which
  undo mustn't take back).

**2. Three new location kinds**, not more modes of `ValueEditLocation`:

| Kind (JSON) | Location | State | Applied with |
|---|---|---|---|
| `startup-approved` | `StartupApprovedLocation { key, value_name, expected, desired }` | Explorer's 12-byte `REG_BINARY` (absent = enabled) | `NtSetValueKey`, `RegDeleteValue` |
| `task-state` | `TaskStateLocation { path, enabled_before, enabled_after }` | The task's Enabled flag | `IRegisteredTask::put_Enabled` |
| `service-start` | `ServiceStartLocation { name, before, after }` | `ServiceStart { start_type, delayed }` | `ChangeServiceConfigW`, `ChangeServiceConfig2W` |

Their identity names the item only (`approved:…\\name`, `task-state:\path`, `service-start:name`),
whatever states the change goes between, so one item has one identity.

**3. Each kind has its own protection rule in HardFloor**, the last check before the change:
- A startup approval is allowed only in Explorer's three keys, `...\Explorer\StartupApproved\Run`,
  `Run32` and `StartupFolder`, in the machine's hive or a user's, and only as Task Manager writes
  it: 12 bytes of `REG_BINARY` starting with `02` (enabled) or `03` (disabled; the next eight
  bytes after three zeros are the FILETIME of the change), or the value deleted. Anything else is
  `not-editable`.
- A task's state: never one of Windows' own tasks (`\Microsoft\...`, `windows-task`).
- A service's start type (new reason `windows-service`): never a driver's, never a service whose
  image is Windows' own (for a `svchost.exe` service, its `ServiceDll`; checked by Authenticode
  through `platform/signature`, embedded signature or catalog), never to anything but Automatic,
  Manual or Disabled. A service that can't be read is refused; one that doesn't exist is left to
  the executor, which reports it missing. `ServiceLocation` (removal) keeps its rules: the
  Sandbox's leftover fixtures run a copy of `cmd.exe` as a service, which the catalog would call
  Microsoft's.
  - *Amended 2026-11-02 (maintainer's decision):* "Windows' own" means a valid signature whose
    chain ends in one of Microsoft's **product** roots, and whose signer isn't the publisher
    Microsoft signs hardware vendors' driver packages as (WHQL: "Microsoft Windows Hardware
    Compatibility Publisher", issued by "Microsoft Windows Third Party Component CA"). Microsoft's
    **application** root (Edge and its updater, SQL Server, WSL, Office) no longer counts: those
    services can be disabled, with undo, like anyone's. The first version refused every chain
    ending in either root.

**4. Exactness.**
- A `StartupApproved` key can be missing (a fresh profile, the Sandbox). Execute creates it, and
  `state.json` records the topmost key it created (`created_key`). Undo, and recovery of an
  interrupted change, delete those keys again, deepest first and each only while it's empty.
- A service's delayed flag is written only when it differs: `ChangeServiceConfig2W` adds a
  `DelayedAutostart` value where there was none, and the Sandbox compares the service's key
  exactly after undo. The start type is written only when it differs too.

## Options considered
- **Reuse `ValueEditLocation`** with a new `ValueEditKind` for startup approvals. Rejected:
  - Value edits have their own allow-list (the environment and SharedDLLs), their own planner,
    their own inverse operations for undo (entries back into a list, a count raised again) and
    their own wording ("Remove C:\Contoso\bin from the system variable Path"). A startup approval
    shares none of that, and mixing them would widen the edit allow-list for every edit kind.
  - Tasks and services aren't registry values: their state goes through the Task Scheduler and
    the Service Control Manager, which keep more than the registry shows (a task's XML, a
    service's cached configuration). Writing their registry values directly would bypass them.
- **A generic "state" location holding opaque before/after blobs.** Rejected: every kind needs its
  own reading, writing, protection and wording anyway, and an opaque blob would hide from
  HardFloor what is written.

## Consequences
- Every visitor over `leftovers::Location` handles three more kinds: identity, category, owner,
  protection, JSON, the executor's three phases, CLI kinds (`startup-approved`, `task-state`,
  `service-start`) and the GUI's text. State changes run after registry values and before
  registry keys.
- Undo never fights the user: a state changed by someone else since stays theirs. The cost is a
  set that is only `partly-restored` when that happens.
- `put_Enabled` lets the Task Scheduler rewrite the task's XML; whether undo gives back the XML
  byte for byte is for the Sandbox's `startup` scenario to show.
