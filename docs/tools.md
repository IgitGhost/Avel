# Avel Tools

Tools that work on the system rather than on one program. They live in `src/core/tools`, with
helpers in `src/core/platform`, and like the rest of `avel_core` they have no Qt dependency and
return codes rather than text. Each tool lists items with ids that stay the same from one run to
the next, and every change it makes goes through a ChangeSet (see [changes.md](changes.md)), so
undo can put it back (all but the removal of an app for every user, which Windows can't
undo and which runs only once confirmed).

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
uninstallers (`platform::missing_command_file`). The target's version resource gives the product,
company and version (`ProductVersion`, else `FileVersion`), and `force::OwnerIndex` the installed
program whose folder holds it.

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
    `target_missing`, `product` (`product_name`, `company_name`, `version`), `signer` (`checked`,
    `valid`, `windows`, `name`), `program_id`, `program_name`, `state`, `disabled_by_avel`, `approval`
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

### GUI (Tools > Startup…)

`StartupWindow` (`src/app/startup_window.cpp`) is the GUI's first non-modal window: a top-level
window of its own (not owned by the main window, so "Go to program" can bring the main window to
the front), opened once and brought back to the front when it is open already. It closes with
the main window, and refuses to close while a change runs.

- **The list** is made on a worker thread (signature checks take time): the services Avel
  disabled (read under `store.lock()`), then `list_startup` for every user through a
  `LiveEnvironment` that lives only inside that task, with `live_signer()` kept by the window so
  refreshes reuse its cache. The owners' names come from the main window's inventory (or a scan
  of its own when the main window hasn't finished one). F5 lists again.
- **One flat sortable table** (`StartupTableModel` + `StartupFilterModel`): Name (a service's
  display name), State ("Enabled", "Disabled"; a service whose start is Disabled: "Disabled
  completely"), Source (Run, RunOnce, RunOnceEx, Startup folder, Scheduled task, Service; "(32-bit)"
  for `WOW6432Node`), User ("Machine" or the account name), Publisher (the signer when the
  signature is valid, else the version resource's company) and Command. Above it a search box
  (name, command, publisher, target, product, owner program) and two filters: the source and the
  user (every user, the machine, or one user). Windows' own entries are greyed out and no action
  is enabled for them.
- **The details pane** below shows the selected entry: state and how it is kept, source, user and
  SID, command, target file ("file not found" when `target_missing`), signature, product, company
  and version, the owner program, where it is stored, where Task Manager's state is kept, what
  Delete removes (`delete_locations`) and the id; notes for Windows' own entries, services Avel
  disabled and missing targets.
- **Actions** work on every selected row they fit, as **one ChangeSet** (`plan_startup` with all
  the selected ids; rows it reports as protected, not supported or already in that state are
  skipped, never refused as the CLI does). Enable and Disable (a service: Manual) apply at once;
  "Disable completely" (context menu, services: Disabled) too; Delete (Del) first asks in a
  message box that lists what it removes. The result goes to the window's status bar: how many
  entries changed, couldn't be changed and were skipped, with the reasons in its tooltip. Then the
  list is made again. The ChangeSet's description is translated text ("Disable startup entry
  Contoso").
- **Undo**: every change is a normal set in the Undo Center (a button opens it from the window).
  "Undo last change" (Ctrl+Z) restores the newest set this window made, then the one before it.
- **Context menu**: Enable, Disable, Disable completely (when a service is selected), Delete…,
  then Open file location (Explorer selects the target, or the Startup folder item), Copy command,
  Go to program (selects the owner program in the main window), Open in Registry Editor (Run,
  RunOnce, RunOnceEx and services: writes the key to Registry Editor's `LastKey`, keeping the
  localized root name it wrote last, and starts `regedit -m`; a signed-out user's hive isn't
  loaded where Registry Editor sees it, which the status bar says), Open Task Scheduler (tasks)
  and Open Services (services).
- One worker runs the listing, a change and an undo one at a time, so the listing's
  `LiveEnvironment` is always gone before a ChangeSet opens a user's hive. The Undo Center can be
  opened only while the window is idle.
- UI Automation finds the parts by object name: `startupWindow`, `startupTable`, `startupSearch`,
  `startupSourceFilter`, `startupUserFilter`, `startupDetails`, `startupResult` (the status bar's
  result), `startupCount`, `startupWarnings`; the actions `startup<Name>Action` (Refresh, Enable,
  Disable, DisableCompletely, Delete, Undo, UndoCenter, OpenLocation, CopyCommand, GoToProgram,
  OpenRegistry, OpenTaskScheduler, OpenServices) and their toolbar buttons `startup<Name>Button`;
  Delete's confirmation `startupDeleteConfirmation` with `startupConfirmDeleteButton` and
  `startupCancelDeleteButton`. The main window's menu item is `startupManagerAction`.

### Ids

An entry's id is 10 hex digits of the FNV-1a hash of its first delete location's identity
(`leftovers::identity`, lowercase): the Run value, the subkey, the file, the task or the service.
It stays the same across listings and runs, so the CLI can take it as an argument.

### Lifetime of the environment

A signed-out user's hive can be loaded privately only once per process (`RegLoadAppKey`), and the
`LiveEnvironment` the list reads through keeps it loaded while it lives. The front ends destroy it
as soon as the list is taken, and before running a ChangeSet that writes to that hive (the
executor opens the hive itself, writable). The CLI scopes it to the function that lists, so it is
gone before the plan runs. In avel.exe a scan from another window could still hold it, so the app
has one hive lock: scans hold it shared, ChangeSets exclusive ([changes.md](changes.md)).

On the development machine (unelevated) `avel-cli startup list` shows 63 entries (22 Run, 1
RunOnce, 8 tasks, 32 services): the listing takes about 0.3 s with `live_signer` checking every
target, the whole command about 0.45 s with the inventory. Under the product-root rule
`edgeupdate`, `SQLWriter` and `WSLService` are listed (Microsoft's applications), Windows' own
services aren't, and `SecurityHealth` is listed as `windows-own`.

## Windows apps (`tools/windows_apps`)

`list_windows_apps` lists the packaged apps on this PC (Store and MSIX apps, Windows' own
included), one `WindowsApp` per package family, and says which of them can be removed and how
([ADR 0008](adr/0008-windows-apps-and-features.md)). It only reads. Each app has its id, family and
identity name, display name, publisher, version and folder (from the package the current user has,
else the newest), its signature kind, every version on this PC with who has it, the users it is
registered for (SID, account name, whether it is the current user), whether it is provisioned
(installed for every new user) and with which package, its category and warning from the data
list, its protection, and the modes available.

### Sources

Everything comes from the public `PackageManager`, through `platform/packages` (every call on a
fresh multithreaded-apartment thread, so the GUI thread's apartment is left alone): no Appx
cmdlets, no StateRepository or `AppxAllUserStore` reads.

- `FindPackagesWithPackageTypes(Main | Optional)` for all users, and `FindUsers` for who has each
  package (`platform::list_packages`). Frameworks, resource packages and bundles aren't apps of
  their own and aren't listed; the main and optional packages of a family make one app.
- `FindProvisionedPackages` for what is provisioned.
- Both need administrator rights. Without them the listing holds only the current user's packages
  (`WindowsAppsList::all_users` is false): who else has an app and whether it is provisioned
  aren't known, so nothing counts as undoable and the planner plans nothing from it
  (`incomplete-listing`). The listing agrees: no app offers a mode then (`can_remove_for_me`,
  `can_deprovision` and `can_remove_for_everyone` are all false), and `all_users` is the reason
  the front ends show, once for the whole list rather than per app.
- Account names come from `LookupAccountSid` (`live_windows_apps_environment`, with a cache); a
  SID that doesn't resolve is shown as itself. Tests inject a listing, the current user and the
  names (`WindowsAppsEnvironment`).

### Categories and the data list (`tools/windows_apps_data`)

What Avel knows about Windows' own apps is a reviewed table, like the noise rules
(`app_data_entries`), matched by the package's identity name (the family name before its
publisher id), ignoring case; a trailing `*` matches a prefix. Change it only with a reason.

| Category | Meaning | Examples |
|---|---|---|
| `critical` | Windows or other apps need it: never offered | the Store and its purchase app, App Installer, Windows Security, Settings, the Start menu's and the shell's hosts, the sign-in and lock screen apps, SmartScreen, the frameworks other apps run on (`Microsoft.UI.Xaml*`, `Microsoft.WindowsAppRuntime*`, `Microsoft.VCLibs*`...) |
| `semi-critical` | Offered, with a warning code saying what removing it breaks | Photos, Notepad and Paint (`opens-files-by-default`), Terminal (`default-terminal`), Edge's package (`browser-integration`), the Xbox identity and overlay apps (`game-sign-in`, `game-bar`), Gaming Services (`gaming-services`), the Xbox app (`game-pass`), Widgets (`widgets`), Snipping Tool (`screen-capture`), the media extensions (`media-formats`), OneDrive's sync package (`cloud-files`), winget's source (`winget-catalog`), language packs (`display-language`), Media Player (`default-media-player`) |
| `often-removed` | Offered; the "often removed" preset checks it (nothing is checked by default) | Clipchamp, the Bing apps (News, Weather...), Solitaire, Get Help, Tips, Feedback Hub, To Do, the new Teams and Outlook, Mail and Calendar, Copilot, Phone Link, Power Automate, Quick Assist, Dev Home, Microsoft 365 (Office), Skype, Maps, and the third-party promotions Windows installs by itself (Candy Crush, Spotify's stub, Disney+, TikTok...) |
| `other` | Nothing known: offered, never checked by a preset | everything else |

### Protection

An app is **protected** (`AppProtection`), listed but never offered for a change, when:
- any of its packages is signed as part of Windows (`system`: `SignatureKind::System`, the public
  stand-in for `NonRemovable`; Windows refuses to remove those anyway), or
- its family is on the critical list (`critical`; some, like the Store, App Installer and Windows
  Security, aren't System-signed), or
- any of its packages has an external location (`external-location`): a sparse package, or one
  whose files are a desktop program's folder (`PackageInfo::external_location`, from
  `Package::EffectiveExternalPath`). It gives that program its identity (PowerToys' context
  menus, Edge's, OneDrive's or the Microsoft 365 Copilot app's integration), and removed alone the
  program breaks; it goes when the program is uninstalled, as its leftover (`related-packages`,
  [leftovers.md](leftovers.md)). `WindowsApp::external_location` keeps the program's folder for
  the details. This comes after the other two, and wins over the data list's category: Edge's
  and OneDrive's packages stay `semi-critical` with their warning, and Microsoft 365 (Office),
  now the Copilot app's package, `often-removed`, but none is offered.

The planner refuses protected apps (`protected`), and HardFloor refuses every package item for
them again right before it changes (`windows-package`), with frameworks, resource packages and
items whose family can't be told ([changes.md](changes.md), "Packaged apps"). For a package with
an external location HardFloor refuses registrations, provisioning and removal for every user
(`desktop-program-package`), but still lets a removal for one user through: that is how the
leftover scan takes it once its program is uninstalled.

### The three modes (`tools/windows_apps_changes`)

Each takes apps by id or family name (ignoring case; each planned once) and returns a
`changes::ChangePlan`, the ids planned and per-id issues. An id with an issue is never planned.
Items carry the app's display name as their program and no confidence: the user chose them.

| Mode | CLI | Item | Offered when | Undo |
|---|---|---|---|---|
| Remove for me | `remove` | `PackageUserLocation`, registered to not, for the current user | the current user has it, and the package stays on this PC: another user has the family or it is provisioned (`soft_undoable`) | `RegisterPackageByFamilyNameAsync` for the current user: whatever version of the family is staged then (an update may have replaced the one removed); `restore-skipped (missing)` once the package has left this PC. The app's data isn't kept: Windows deletes it with the registration. |
| Don't install for new users | `deprovision` | `PackageProvisioningLocation`, provisioned to not | it is provisioned and some user has it | `ProvisionPackageForAllUsersAsync` while the family is staged. Users who have the app keep it either way. |
| Remove for everyone | `remove-everywhere` | `PackageLocation` with `all_users`, one per version on this PC | it isn't protected, and only with the confirmation that it can't be undone | None. It deprovisions the family first when provisioned, then `RemovePackageAsync(RemoveForAllUsers)` deletes it from this PC. Every user's data in it is lost; it comes back only from the Store or winget. |

- **For the current user only.** Windows has no public call that removes or registers a package
  for another (signed-out) user, so "Remove for me" acts on the account Avel runs as. Other users'
  registrations are shown; for them Avel offers deprovisioning and removal for everyone (ADR 0008,
  maintainer decision A).
- **A removal that would delete the package isn't soft.** Windows deletes a package the moment its
  last user's registration goes, unless it is provisioned, and nothing brings it back offline. So
  "Remove for me" is refused when nobody else has the family and it isn't provisioned
  (`would-delete-package`): only removal for everyone, irreversible, removes it then. The executor
  checks it again when it runs (`skipped (not-undoable)`). Deprovisioning a family no user has is
  refused the same way (that Windows deletes it then is not verified).
- Issues (codes, no text): `unknown-id`, `protected`, `incomplete-listing` (no administrator
  rights), `not-installed-for-current-user` (Remove for me), `would-delete-package`,
  `not-provisioned` (Don't install for new users), `irreversible-needs-confirmation` (Remove for
  everyone without it). For the CLI every issue is an error: it changes nothing while any id has
  one. The window changes the apps that fit and reports the others (see "GUI" below).
- How the executor carries the items out, compare-and-set, recovery and what `state.json` records
  are in [changes.md](changes.md) ("Packaged apps").
- The program list's **Uninstall** of a packaged app uses "Remove for me" whenever it is undoable,
  and refuses the protected ones (`protected-package`) ([uninstall.md](uninstall.md), "Packaged
  apps").

### CLI (`avel-cli apps`)

```
avel-cli apps list [--all] [--json]
avel-cli apps remove <id>... [--dry-run] [--yes] [--json]
avel-cli apps deprovision <id>... [--dry-run] [--yes] [--json]
avel-cli apps remove-everywhere <id>... --allow-irreversible [--dry-run] [--yes] [--json]
```

- An `<id>` is an id from `apps list`, or a package family name.
- `list` runs without administrator rights; it then lists only the current user's apps and says
  so on stderr. Without `--all` the protected apps are left out (the summary counts them). Text:
  one line per app (id, category, who has it: `you`, `you +2`, `3 users`, with `new users` when it
  is provisioned; name, version and family), then its modes (`[remove, deprovision,
  remove-everywhere]`) or `[protected: system|critical|external-location]` (the last followed by
  `[part of: <folder>]`), and `[warning: <code>]` for a semi-critical app. Unelevated, an unprotected app shows `[needs administrator]` instead of its
  modes, and the note on stderr says that none can be changed.
- The actions need administrator rights, except with `--dry-run` (which, unelevated, reports
  `incomplete-listing` for every id). They open the change store (settling interrupted sets and
  purging old ones), list the apps, plan, print the apps and every refused id with its issue code
  (`error: <id>: <reason> (<code>)`), and change nothing (exit 1) while any id has an issue. Then
  they ask (`--yes` doesn't) and run one ChangeSet under the store's lock. `remove` and
  `deprovision` print "`n of m changed. Undo with: avel-cli undo <set>`"; `remove-everywhere`
  prints that it can't be undone.
- `--json` (with an action, it needs `--yes` or `--dry-run`):
  - `list`: `avel_version`, `elevated`, `all_users` (every user's packages were read),
    `include_protected` (`--all`), `error` (an HRESULT that cut the listing short, else 0),
    `elapsed_ms` and `apps[]`. An app is `id`, `family`, `name`, `display_name`, `publisher`,
    `version`, `install_location`, `external_location` (the desktop program's folder, else
    empty), `signature` (`none`, `developer`, `enterprise`, `store`, `system`), `versions[]`
    (`{full_name, version, users[]}`), `current_user_full_name`, `users[]` (`{sid, account_name,
    current}`), `provisioned`, `provisioned_full_name`, `category`, `warning`, `protection`
    (`none`, `system`, `critical`, `external-location`), `soft_undoable` and `modes` (`{remove,
    deprovision, remove_everywhere}`).
  - Actions: `avel_version`, `elevated`, `all_users`, `dry_run`, `action`, `apps[]` (the ones
    asked for), `issues[]` (`{id, code}`), `description`, `plan[]` (locations; empty when refused)
    and `change` (as `force` writes it, or null).
- Exit codes: 0 done (and a dry run with no issue); 1 refused, cancelled, not elevated, or an item
  didn't change; 2 usage; 99 when `AVEL_TEST_FAULT_AFTER` stops the run (recovery tests).
- The ChangeSet's description is "Remove Microsoft News for me", "Don't install 3 apps for new
  users" or "Remove Clipchamp for everyone".

```powershell
avel-cli apps list                                     # your apps (every user's when elevated)
avel-cli apps list --all --json                        # the protected ones too, for scripts
avel-cli apps remove Microsoft.BingNews_8wekyb3d8bbwe  # then: avel-cli undo <set>
avel-cli apps deprovision 41ed4e68c0 --dry-run
avel-cli apps remove-everywhere 41ed4e68c0 --allow-irreversible --yes
```

### GUI (Tools > Windows apps…)

`WindowsAppsWindow` (`src/app/windows_apps_window.cpp`) is a non-modal top-level window like the
Startup manager's: not owned by the main window, opened once and brought back to the front when
it is open already, closed with the main window, and it refuses to close while a change runs.

- **The list** is made on the window's worker thread: `list_windows_apps` with
  `live_windows_apps_environment()`. It asks the package manager only and loads no user's hive,
  so it takes no hive access. F5 lists again.
- **One sortable table** (`WindowsAppsTableModel` + `WindowsAppsFilterModel`): a check box, Name
  (the display name; its tooltip the family), Publisher, Version (sorted by number), Installed for
  ("You", "You and 2 other users", "1 other user", with "new users" when it is provisioned;
  "Nobody" when it is only staged) and Category ("Often removed", "⚠ Semi-critical" with the
  warning in its tooltip, "Other"; for a protected app its protection: "Part of Windows",
  "Critical", "Part of a desktop program"). Above it a search box (display name, identity and
  family names, publisher) and two filters: the category (all apps, can be removed, often
  removed, semi-critical, protected) and who has it (every user, one user, the current one marked
  "(you)", or new users).
- **Check boxes.** Nothing is checked by default. Protected apps are greyed, have no check box and
  no action, and their tooltip and details give the reason (for `external-location`, the desktop
  program's folder). "Check often removed" checks every unprotected `often-removed` app; "Uncheck
  all" clears them. A refresh keeps the checks of the apps still listed.
- **The details pane** below shows the selected app: every full name, the family, publisher,
  version, install folder, the users it is installed for (the current one marked), whether it is
  installed for new users (with the provisioned package), its category and warning, its signature
  kind and id; then, for each of the three actions, what it does, whether it can be undone, and
  whether it is available for this app or why not (the planner's issue for that app alone).
- **Actions** ("Remove for me", "Don't install for new users", "Remove for everyone…") work on
  the checked apps, or, when none is checked, on the selected ones. Each is enabled when it fits
  one of them (`can_remove_for_me`, `can_deprovision`, `can_remove_for_everyone`), and runs as
  **one ChangeSet** for all of them: the planner gets every id, plans the ones that fit and
  reports the others, which are skipped (never refused as the CLI does) and listed in the status
  bar's tooltip with their reasons. "Remove for me" and "Don't install for new users" apply at
  once. "Remove for everyone…" first opens a confirmation that lists the apps and says that every
  user's data in them is lost, that nothing can bring them back, and that they come back only from
  the Store or winget; its button is enabled only once "I understand this can't be undone" is
  checked, and only then does the set run with `allow_irreversible`. The ChangeSet's description
  is translated text ("Remove Microsoft News for me", "Don't install 3 apps for new users",
  "Remove Clipchamp for everyone"). The result goes to the status bar as in the Startup manager;
  then the list is made again, and the main window scans the installed programs again.
- **When the listing is incomplete** (`all_users` false: not elevated, which avel.exe never is),
  a banner above the table says why, and every action, "Check often removed" included, stays
  disabled. A listing cut short by an error is a warning in the status bar.
- **Undo**: "Undo last change" (Ctrl+Z) restores the newest undoable set this window made (removal
  for everyone isn't one); the Undo Center opens from the window too. Each set runs under
  `exclusiveHiveAccess()` and the store's lock, as every ChangeSet in avel.exe.
- **Context menu**: the three actions, Copy full name, Copy family name (both for every selected
  app) and Open install folder.
- UI Automation finds the parts by object name: `windowsAppsWindow`, `windowsAppsTable`,
  `windowsAppsSearch`, `windowsAppsCategoryFilter`, `windowsAppsUserFilter`, `windowsAppsBanner`,
  `windowsAppsDetails`, `windowsAppsResult`, `windowsAppsProgress`, `windowsAppsCount`,
  `windowsAppsWarnings`; the actions `checkOftenRemovedAction`, `uncheckAllAction`,
  `removeForMeAction`, `deprovisionAction`, `removeForEveryoneAction`, `windowsAppsRefreshAction`,
  `windowsAppsUndoAction`, `windowsAppsUndoCenterAction`, `windowsAppsOpenFolderAction`,
  `windowsAppsCopyFullNameAction`, `windowsAppsCopyFamilyNameAction`, and their toolbar buttons
  with `Button` for `Action` (`checkOftenRemovedButton`, `removeForMeButton`...). The check box
  cell is named after its app. The confirmation is `removeForEveryoneConfirmation`, with
  `removeForEveryoneUnderstandCheck`, `removeForEveryoneConfirmButton` and
  `removeForEveryoneCancelButton`. The main window's menu item is `windowsAppsAction`.

### Ids

An app's id is 10 hex digits of the FNV-1a hash of its family name, lowercase (`windows_app_id`):
the same across listings, runs and versions of the app, so the CLI can take it as an argument.

On the development machine (unelevated) `avel-cli apps list --all` lists 132 apps in about 0.4 s:
81 offered (22 often removed, 28 semi-critical), 44 protected as System-signed and 7 as critical.
