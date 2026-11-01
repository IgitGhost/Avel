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
| `service` | the Service Control Manager, Win32 services only (never drivers) | service whose start is Automatic (delayed or not) and whose file isn't Microsoft-signed, plus every service Avel disabled (below) | the service |

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
  (`StartupEnvironment::avel_disabled_services`).

### Protection

The signature check is injected (`StartupEnvironment::signer`, called from several threads, once
per distinct file); without one nothing counts as Microsoft's. Then:
- Microsoft's tasks (`\Microsoft`) and services whose file is Microsoft-signed aren't listed at
  all (unless Avel disabled the service).
- An entry whose target is a Microsoft-signed file under the Windows folder is **Windows' own**
  (`windows-own`), for example `SecurityHealth` (`%windir%\system32\SecurityHealthSystray.exe`):
  listed, never offered for a change. Programs that run whatever their arguments say (`cmd`,
  `powershell` without `-File`, `mshta`, `explorer`, `rundll32` without a DLL, ...) don't make an
  entry Windows' own.
- Every change also passes HardFloor and ProtectedPolicy, as everywhere.

### Ids

An entry's id is 10 hex digits of the FNV-1a hash of its first delete location's identity
(`leftovers::identity`, lowercase): the Run value, the subkey, the file, the task or the service.
It stays the same across listings and runs, so the CLI can take it as an argument.

### Lifetime of the environment

A signed-out user's hive can be loaded privately only once per process (`RegLoadAppKey`), and the
`LiveEnvironment` the list reads through keeps it loaded while it lives. The front ends destroy it
as soon as the list is taken, and before running a ChangeSet that writes to that hive (the
executor opens the hive itself, writable).

On the development machine (unelevated) the list has 63 entries and takes about 0.4 s, signature
checks of every target included (about 100 files, checked with a prototype of the signer).
