# Avel Tools

Tools that work on the system rather than on one program. They live in `src/core/tools`, with
helpers in `src/core/platform`, and like the rest of `avel_core` they have no Qt dependency and
return codes rather than text. Each tool lists items with ids that stay the same from one run to
the next, and every change it makes goes through a ChangeSet (see [changes.md](changes.md)), so
undo can put it back.

## Startup manager (`tools/startup`)

`list_startup` lists everything that starts with Windows, for the machine and every user: Task
Manager's Startup apps plus RunOnce, scheduled tasks and services. It only reads. Each
`StartupEntry` has its kind, scope (the machine, or a user's SID and account name, and whether it
comes from a 32-bit `WOW6432Node` key), name, command as written, target, state, owner and the
locations that deleting it removes.

### Sources

| Kind | Where | One entry per | Delete location |
|---|---|---|---|
| `run` | `SOFTWARE\Microsoft\Windows\CurrentVersion\Run` and `SOFTWARE\WOW6432Node\...\Run` (HKLM); `Software\...\Run` and `Software\WOW6432Node\...\Run` of each user's hive | `REG_SZ` or `REG_EXPAND_SZ` value that isn't blank | the value |
| `run-once` | the same, `RunOnce` | value | the value |
| `run-once-ex` | `SOFTWARE[\WOW6432Node]\Microsoft\Windows\CurrentVersion\RunOnceEx` of HKLM only (Windows doesn't read a user's) | subkey; its first named value is the command (`DllFile\|Function\|Arguments` or `\|\|Command`), its default value the title | the subkey |
| `startup-folder` | the common Startup folder (`FOLDERID_CommonStartup`) and each user's (`Startup` in their own `User Shell Folders`, else `AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup`) | file directly in the folder, `desktop.ini` excepted; `.lnk` files are resolved to their target and arguments, other files run as they are | the file |
| `task` | the Task Scheduler, outside `\Microsoft` | task with an enabled logon or boot trigger; a task that only runs COM handlers is listed with no command and no target | the task |
| `service` | the Service Control Manager, Win32 services only (never drivers) | service whose start is Automatic (delayed or not) and whose file isn't Windows' own (see Protection), plus every service Avel disabled (below) | the service |

Each key is read once: two paths to a key that the 32- and 64-bit views share are recognized by
the key's kernel name, as in the leftover scan's references. Another user's hive is read through
`LiveEnvironment` (a signed-out user's `NTUSER.DAT` privately, read-only); without administrator
rights it, other users' Startup folders and some tasks and services are out of reach and simply
not listed. When the Task Scheduler or the Service Control Manager can't be read at all, the list
says so in its diagnostics (`tasks-unavailable`, `services-unavailable`).

**The target** is the file the entry runs: the command is expanded for its user (a user's
`%APPDATA%` points into their profile), split like CreateProcess does, and the script or DLL a
Windows host runs (`rundll32 x.dll,Entry`, `powershell -File x.ps1`, `wscript`/`cscript x.vbs`) wins
over the host. A bare program name is looked for in `System32`; a service's `\SystemRoot\`,
`\??\` and `System32\` forms are understood; a service svchost hosts is its `ServiceDll`. 8.3
names are expanded. `target_missing` is set only for "not found" on a fixed disk, as for broken
uninstallers (`platform::missing_command_file`). The target's version resource gives the product
and company, and `force::OwnerIndex` the installed program whose folder holds it.

### State

- **Run and Startup folder entries** are disabled the way Task Manager does it: a `REG_BINARY`
  value named after the Run value, or after the file (`AnyDesk.lnk`), in
  `...\Explorer\StartupApproved\Run`, `Run32` or `StartupFolder` of the same hive. Task Manager
  writes 12 bytes: `02` and 11 zeros to enable, `03 00 00 00` and the FILETIME of the change to
  disable. Other first bytes are seen in the wild (`06` enabled, `01` disabled), so the rule is the
  first byte's **low bit**: set means disabled. No value means enabled. The entry carries the key,
  the value name and the bytes as stored (`StartupApproval`), and its delete locations include the
  value when there is one.
- **Pairing.** The machine's 64-bit Run key pairs with HKLM's `StartupApproved\Run`, its
  `WOW6432Node` Run key with HKLM's `StartupApproved\Run32`, a user's Run key with their own
  `StartupApproved\Run`; the common Startup folder with HKLM's `StartupApproved\StartupFolder`, a
  user's with theirs. A user's `Software\WOW6432Node\...\Run` key has no list: HKCU's Run key is
  shared by both views, so 32-bit programs write the 64-bit one, and on the development machine no
  user hive has a `StartupApproved\Run32` key (and the WOW6432Node Run key doesn't exist). Its
  values are listed as delete only, like RunOnce and RunOnceEx, which Task Manager doesn't show and
  has no state for.
- **Tasks** are disabled when the task's own Enabled flag is off.
- **Services** are disabled when their start is Manual or Disabled. A service Avel disabled stays
  in the list as disabled (`disabled_by_avel`), so it can be enabled again: the front ends pass the
  names of the services whose start a done, not undone ChangeSet changed
  (`StartupEnvironment::avel_disabled_services`, from `services_disabled_by_avel`, below).

### Protection

Only **Windows' own** files are protected (the maintainer's decision of 2026-11-02). Microsoft
signs three kinds of files, told apart by where the signer's chain ends
(`platform::verify_signature`, `CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_MICROSOFT_ROOT)`
without and then with `MICROSOFT_ROOT_CERT_CHAIN_POLICY_CHECK_APPLICATION_ROOT_FLAG`):

| Signed through | Examples | Windows' own |
|---|---|---|
| a **product root** ("Microsoft Root Certificate Authority 2010" and older), signer "Microsoft Windows", "Microsoft Windows Publisher" | `svchost.exe`, `SecurityHealthSystray.exe`, Defender's `MsMpEng.exe` | yes |
| a product root, signer "Microsoft Windows Hardware Compatibility Publisher" (issued by "Microsoft Windows Third Party Component CA"): **WHQL** | hardware vendors' driver packages: Realtek's, Intel's, NVIDIA's services in the DriverStore | no: the vendor's |
| the **application root** ("Microsoft Root Certificate Authority 2011"), signer "Microsoft Corporation" | Edge and its updater (`edgeupdate`), SQL Server's `SQLWriter`, `WSLService`, Office | no: Microsoft's programs, listed like anyone's |

The signer's names are compared only once the chain has verified to a product root, where only
Microsoft issues certificates. A signature must also be valid (`SignatureState::Valid`) for a file
to be Windows' own. `Signature::root` keeps which root it was, whatever the state.

The signature check is injected (`StartupEnvironment::signer`, called from up to 8 threads, once
per distinct file); without one nothing counts as Windows' own. The front ends use
`tools::live_signer()`, `verify_signature` through a `SignatureCache` that the copies of the
function share, mapped to `StartupSigner` (`checked`: the file was examined; `valid`; `windows`;
`name`: the signer). Then:
- Windows' own tasks (`\Microsoft`) and services whose file is Windows' own aren't listed at all
  (unless Avel disabled the service).
- An entry whose target is a file of Windows' own under the Windows folder is **Windows' own**
  (`windows-own`), for example `SecurityHealth` (`%windir%\system32\SecurityHealthSystray.exe`):
  listed, never offered for a change. Programs that run whatever their arguments say (`cmd`,
  `powershell` without `-File`, `mshta`, `explorer`, `rundll32` without a DLL, ...) don't make an
  entry Windows' own.
- Every change also passes HardFloor and ProtectedPolicy, as everywhere. The floor's
  `ServiceStartLocation` rule uses the same test (`ServiceFacts::windows`, from
  `platform::query_service` and the service's file, `platform::service_file`), so a service the list
  offers is one the floor lets change.

### Changes (`tools/startup_changes`)

`plan_startup(entries, action, ids, service_disable, now, program_name)` turns a request into a
`changes::ChangePlan` and per-id issues. Ids are compared ignoring case; each entry is planned once.

| Kind | `disable` | `enable` | `delete` |
|---|---|---|---|
| `run` (machine, user's 64-bit key, machine's `WOW6432Node`) and `startup-folder` | `StartupApprovedLocation`: `expected` = the value as listed (absent: `nullopt`), `desired` = `03 00 00 00` + `now` | the same, `desired` = `02` + 11 zeros | the delete locations |
| `run` in a user's `WOW6432Node` key, `run-once`, `run-once-ex` | `not-supported` | `not-supported` | the delete locations |
| `task` | `TaskStateLocation` enabled → disabled | disabled → enabled | the task |
| `service` | `ServiceStartLocation`: start → Manual (`--service-start disabled`: Disabled) | start → Automatic | the service |

- A service keeps its delayed flag, so only `Start` changes and undo puts back exactly that.
- Delete takes the entry's delete locations as they are: the item itself, then its
  StartupApproved value when there is one (the list already added it).
- Issues (codes, no text): `unknown-id`; `protected` (Windows' own, for every action);
  `not-supported`; `already-in-state` (disabling a disabled entry or enabling an enabled one: not
  planned, and not an error). For a service, Disabled is "disabled enough" for a Manual request,
  but a Manual service can still be set to Disabled. `is_error` is true for all but the last; the
  front ends change nothing while any id has an error.
- Planned items get their category from `leftovers::category_of`, the owner program's name
  (`program_name`, or its id) as their program, and no confidence (`unknown`, score 0): the user
  chose them, and the executor doesn't read either. `StartupPlan::planned` lists the ids planned;
  the plan's description is the caller's (the CLI's English, the GUI's `tr()`).

**The services Avel disabled.** `services_disabled_by_avel(store)` reads the change store (under
`store.lock()`, as the Undo Center reads it; sets a live run holds are skipped): the
`ServiceStartLocation` items that are `done`, or whose undo failed, name the services whose start
Avel changed. When several sets changed one service, the newest such item decides; the service
counts when that item's `after` start isn't Automatic. Nothing new is stored.

### CLI (`avel-cli startup`)

```
avel-cli startup list [--all-users] [--json]
avel-cli startup disable <id>... [--service-start manual|disabled] [--dry-run] [--yes] [--json]
avel-cli startup enable <id>... [--dry-run] [--yes] [--json]
avel-cli startup delete <id>... [--dry-run] [--yes] [--json]
```

- `list` runs without administrator rights: other users' hives, some tasks and some services are
  then out of reach (a `not-elevated` diagnostic). It lists the machine's entries and the current
  user's (`ScanEnvironment::current_user_sid`); `--all-users` adds every other user. Elevated, it
  also reads the change store (without settling or purging it) for the services Avel disabled.
  It runs the inventory for the owner programs. Text: one line per entry (id, state, kind, user,
  name, command, then marks: `[32-bit]`, a service's start, `[target missing]`,
  `[Windows' own: protected]`, `[disabled by Avel]`, `[program: ...]`), a summary, and notes on
  stderr.
- The actions need administrator rights, except with `--dry-run`. They open the change store
  (settling interrupted sets and purging old ones, as every changing command does), read the
  services Avel disabled, list **every** user's entries, plan, print the entries and the issues,
  and change nothing (exit 1) while any id is unknown, protected or not supported. Then they ask
  (`--yes` doesn't), and run the ChangeSet under the store's lock. Disable and enable print
  "`n of m changed. Undo with: avel-cli undo <set>`"; delete prints the removal summary.
- `--json` (with an action, it needs `--yes` or `--dry-run`):
  - `list`: `avel_version`, `elevated`, `all_users`, `elapsed_ms` (the listing alone),
    `entries[]` and `diagnostics[]`. An entry has every field of `StartupEntry`: `id`, `kind`,
    `user_sid`, `user_name`, `wow64`, `name`, `display_name`, `command`, `value_type`, `target`,
    `target_missing`, `product` (`product_name`, `company_name`), `signer` (`checked`, `valid`,
    `windows`, `name`), `program_id`, `program_name`, `state`, `disabled_by_avel`, `approval`
    (`key`, `value_name`, `value`: `{type, data}` with the bytes in hex, or null; null when the
    entry has no list), `task_enabled`, `service_start` (the SCM's number), `service_delayed`,
    `delete_locations` (as `undo --show` writes locations) and `protection`.
  - A diagnostic is `{source, code, subject, error}`: `environment` (`not-elevated`,
    `user-hive-unavailable` with the user's SID), `startup` (`tasks-unavailable`,
    `services-unavailable`) or `change-store` (`busy`, or why the store can't be trusted).
  - Actions: `avel_version`, `elevated`, `dry_run`, `action`, `service_start` (disable only),
    `entries[]` (the ones asked for), `issues[]` (`{id, code, error}`), `description`, `plan[]`
    (locations; empty when refused), `change` (as `force` writes it, or null) and `diagnostics[]`.
- Exit codes: 0 done (also when everything asked for was already in that state, and for a dry
  run); 1 refused, cancelled, not elevated, or an item didn't change; 2 usage; 99 when
  `AVEL_TEST_FAULT_AFTER` stops the run (recovery tests).

### Ids

An entry's id is 10 hex digits of the FNV-1a hash of its first delete location's identity
(`leftovers::identity`, lowercase): the Run value, the subkey, the file, the task or the service.
It stays the same across listings and runs, so the CLI can take it as an argument.

### Lifetime of the environment

A signed-out user's hive can be loaded privately only once per process (`RegLoadAppKey`), and the
`LiveEnvironment` the list reads through keeps it loaded while it lives. The front ends destroy it
as soon as the list is taken, and before running a ChangeSet that writes to that hive (the
executor opens the hive itself, writable). The CLI scopes it to the function that lists, so it is
gone before the plan runs.

On the development machine (unelevated) `avel-cli startup list` shows 63 entries (22 Run, 1
RunOnce, 8 tasks, 32 services): the listing takes about 0.3 s with `live_signer` checking every
target, the whole command about 0.45 s with the inventory. Under the product-root rule
`edgeupdate`, `SQLWriter` and `WSLService` are listed (Microsoft's applications), Windows' own
services aren't, and `SecurityHealth` is listed as `windows-own`.
