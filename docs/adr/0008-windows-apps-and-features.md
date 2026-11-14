# ADR 0008: Windows apps and Windows features, from the Phase 6b spike

- **Status:** Accepted (2026-11-14). Extends [ADR 0007](0007-changeset-state-changes.md). The
  maintainer's choices on the points marked **(maintainer)** are under "Maintainer decisions".
- **Date:** 2026-11-14

## Context
Phase 6b adds two tools (plan.md, Phase 6, decided 2026-10-31):
- **Windows apps:** soft removal by default (the chosen users; the package stays staged or
  provisioned, so undo registers it again offline); deep removal (deprovisioned, all users),
  undone only by reinstalling. Critical packages are never offered.
- **Windows features and capabilities** through DismAPI: undo is the inverse operation; a
  capability that may need Windows Update to come back is marked; pending restarts are shown.

Every change goes through a ChangeSet with undo, as state-change locations in ADR 0007's style.
Before designing that, a spike established what works **offline** in Windows Sandbox and,
since cmdlets use private paths (`Remove-AppxPackage -User`), what Avel's own code can reach
through public APIs.

**How it was measured.**
- `avel_package_probe` (`tests/helpers/package_probe.cpp`) makes the exact calls Avel would
  make: C++/WinRT `Windows.Management.Deployment.PackageManager` (SDK 10.0.26100) and DismAPI.
  It prints one JSON object per call: the operation's error code, `DeploymentResult`'s extended
  error, text, activity id and `IsRegistered`, timings.
- The Sandbox scenario `spike_6b` (`tests/sandbox/scenarios/spike_6b.ps1`) runs it next to the
  Appx and DISM cmdlets, with **networking disabled** (the launcher's `<Networking>Disable`; the
  scenario logs no adapter and no ping). After every step it logs `FindUsers`, whether
  `C:\Program Files\WindowsApps\<full name>` exists ("staged"), `FindProvisionedPackages`,
  `IsPackageRemovalPendingForUser`, `Get-AppxPackage -AllUsers`, each user's
  `%LOCALAPPDATA%\Packages\<family>` and a marker file in its `LocalState`, and the keys under
  `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Appx\AppxAllUserStore` and the StateRepository
  rows (`PackageUser`, `PackageFamilyUser`, read from a copy of `StateRepository-Machine.srd`
  with `winsqlite3`) that changed.
- Packages: the unsigned test MSIX (`Avel.TestPackage_1.0.0.0_x64__vg2z0d3ea03zg`, installed
  with `AllowUnsigned`) and the image's only provisioned package, Edge's
  `Microsoft.MicrosoftEdge.Stable_157.0.4341.42_neutral__8wekyb3d8bbwe`.
- The other user is `AvelOther`, made as in the `startup` scenario (`New-LocalUser`,
  `CreateProfile`, never signed in interactively), here with a known password and the batch
  logon right (Performance Log Users), so a scheduled task can run the probe as them.
- Runs: 4 (2026-11-14), each about 25 minutes. Run 1 was stopped early (scenario bugs). Run 2 ran
  to the end with `RESULT: FAIL (1 problem(s))` (the scenario couldn't write the Appx policy
  key, its own step); runs 3 and 4 ended `RESULT: PASS`. Each run added probes to the last; the
  findings are those of runs 2-4, which agree wherever they overlap. Sandbox image: Windows 11
  24H2, build 26100.9974.

## Findings

### 0. The Sandbox image, and reaching a signed-out user
- **Packages:** 50 for all users (`Get-AppxPackage -AllUsers` and `FindPackagesWithPackageTypes`
  agree): 45 main and 5 frameworks; 49 have `SignatureKind` System and are `NonRemovable`
  (shell hosts, `windows.immersivecontrolpanel`, `Microsoft.Windows.*`, `MicrosoftWindows.Client.*`,
  the `*.CBS` frameworks), one is Edge's, `SignatureKind` Developer, not `NonRemovable`. One
  provisioned package (Edge's). No Store, no App Installer, no Calculator or Notepad: the image
  has nothing an "often removed" list would hold, so the inbox findings rest on Edge's package.
- **`NonRemovable` is not public:** the 26100 SDK's WinRT `Package` has no such property (it
  comes from the cmdlet). In this image `SignatureKind == System` coincided with it exactly.
- **Running as a signed-out user without their password is refused:** an S4U task for
  `AvelOther` can't be registered by the administrator (`Register-ScheduledTask`: "Access is
  denied", 0x80131500; Task Scheduler COM `RegisterTaskDefinition(TASK_LOGON_S4U)`:
  0x80070005; `schtasks /NP`: "Access is denied") nor by SYSTEM (a SYSTEM task registering it:
  0x80131500).
- **With their password it works:** a task with `LogonType Password` ran the probe as
  `AvelOther` (not elevated, session 0, profile `C:\Users\AvelOther`) in about 7 s per task.
  This is the spike's (and the `apps` scenario's) way to register a package for the other user:
  `RegisterPackageByFullNameAsync` (or `ByFamilyName`) run as them, on a staged package.
- **Side effects of such a logon:** the first one registered the 48 system packages for the user.
  Afterwards their hive **stays loaded** (`HKU\<sid>`, `Win32_UserProfile.Loaded = True`, no
  process of theirs left) for as long as the runs lasted (over 40 minutes), and `reg unload` is
  denied. Stopping `AppXSvc` and `ClipSVC` didn't release it (run 4). So the "signed-out" user of
  every later step had a loaded hive; removal for a user whose hive is unloaded could not be
  produced after a registration, and is **not verified**.
- A batch logon neither registers a provisioned package for the user (5c2) nor carries out a
  pending removal (3a). Whether an **interactive** logon does is not verified (the Sandbox has
  no second interactive session).

### 1. Removing a package for exactly one other (signed-out) user
| Way | Public? | Result | State afterwards |
|---|---|---|---|
| `Remove-AppxPackage -Package <full> -User <sid>` | cmdlet (private interface) | returns at once, no error | `Get-AppxPackage -AllUsers` shows `Installed(pending removal)` for them; `FindUsers` still says `installed`; `IsPackageRemovalPendingForUser` = False; new key `AppxAllUserStore\EndOfLife\<sid>\<full>`; no StateRepository change; their data folder and marker stay. A batch logon doesn't complete it; registering again as them doesn't cancel it (still `pending removal`) |
| `RemovePackageAsync(full, None)` **run as that user** | yes, but needs to run as them (their password, see 0) | ok, ~0.3 s | `FindUsers` drops them; their `PackageUser` row is deleted; `%LOCALAPPDATA%\Packages\<family>` is **deleted** (marker gone); key `Deleted\EndOfLife\<sid>\<full>` (an earlier `EndOfLife\<sid>\<full>` from the cmdlet moves there); for a provisioned package also `AppxAllUserStore\<sid>\<full>` (`Path`, `Regions`, `LastReturnValue`, `NumberOfAttempts`) |
| `RemovePackageAsync(full, PreserveApplicationData)` | yes | **refused**, 0x80073CFA: "the PreserveApplicationData flag can only be used on a package that was registered in development mode" | unchanged |
| `CleanupPackageForUserAsync(full, sid)` as the administrator | yes (documented "to clean up packages ... after their user profile is deleted") | without policy: 0x80073D01 "blocked by policy" at the call; with `HKLM\SOFTWARE\Policies\Microsoft\Windows\Appx\AllowDeploymentInSpecialProfiles = 1`: ok, 81 ms | `FindUsers` drops them, their `PackageUser` row is deleted, no `AppxAllUserStore` key; their data folder **and marker stay**. Per its documentation the registration in the user's own profile (tiles, file associations) is left behind |
| `RemovePackageAsync(full, RemoveForAllUsers)` | yes | ok | everyone, see 3 |

The public `PackageManager` has **no per-user removal for another user**: `RemovalOptions` is
`None`, `PreserveApplicationData` (0x1000), `PreserveRoamableApplicationData` (0x80),
`DeferRemovalWhenPackagesAreInUse` (0x2000) and `RemoveForAllUsers` (0x80000);
`RemovePackageOptions` adds nothing per user. The only public call taking a user's SID is
`CleanupPackageForUserAsync`.

### 2. Undo by registering again, and when the staged package goes
- **Current user:** after `RemovePackageAsync(full)` while the package is still staged (another
  user has it, or it is provisioned), `RegisterPackageByFullNameAsync(full, {}, None)` (140 ms)
  and `RegisterPackageByFamilyNameAsync(family, ...)` both register it again, offline, also for
  the unsigned test package without `AllowUnsigned`. The user's data **does not survive**: the
  removal deletes `Packages\<family>`; the registration makes a fresh one.
- **Other user:** registering requires running as them (`RegisterPackage*` act for the caller;
  no public call registers for another SID). Run as them, it works offline and is how every
  undo in the spike was done. After `CleanupPackageForUserAsync`, registering as them again
  brought the registration back **with their old data** (marker present), since Cleanup never
  touched it.
- **The staged package goes with its last user, at once:** after the last user's removal of a
  package that isn't provisioned, `WindowsApps\<full>` is gone before the next check (< 1 s),
  `FindPackage` finds nothing, and stays so (checked at 11 s and 61 s).
  `RegisterPackageByFamilyNameAsync` then fails with 0x80073CF9, extended 0x80070490 ("Element
  not found"). **Pitfall:** `RegisterPackagesByFullNameAsync` with `RegisterPackageOptions`
  reported success (`ErrorCode` 0) with `IsRegistered = False` for the vanished package, so
  success must be judged by `IsRegistered` (or by `FindUsers` afterwards).
- A package staged without users (`StagePackageByUriAsync`, shown as `S-1-5-18: staged`) stayed
  staged for the 30 s watched. Whether Windows' maintenance task removes such a package later is
  **not verified**.
- So soft removal stays undoable exactly while the package stays staged: it is provisioned, or
  someone else still has it. That is known before the change: `FindUsers(full)` without the
  users being removed, and `FindProvisionedPackages()`.

### 3. Deprovisioning and deep removal
- `DeprovisionPackageForAllUsersAsync(family)` (35-50 ms) and `Remove-AppxProvisionedPackage
  -Online -PackageName <full>` (0.2 s) do the same: `AppxAllUserStore\Applications\<full>` is
  deleted and `Deprovisioned\<family>` created. Users who have the package keep it; it stays
  staged. Nothing changed in the StateRepository tables watched (`ProvisionedPackage` and
  related tables show no row for either package; the `AppxAllUserStore` keys are the visible
  record).
- **Provisioning again offline works while it is staged:** `ProvisionPackageForAllUsersAsync(family)`
  (23-65 ms) restored `Applications\<full>` and deleted `Deprovisioned\<family>` after both ways
  of deprovisioning, for Edge's package and the unsigned test package (which a stage-only
  `StagePackageByUriAsync(AllowUnsigned)` had put there). It also removed the stale
  `EndOfLife\<sid>` and `AppxAllUserStore\<sid>\<full>` keys of the other user. Provisioning the
  stage-only package registered it for the calling user as well.
- **`RemovePackageAsync(full, RemoveForAllUsers)` also deprovisions:** on a provisioned package it
  creates `Deprovisioned\<family>`, deletes `Applications\<full>`, removes everyone's
  `PackageUser` rows and the staged folder at once. The other user (hive loaded) gets
  `EndOfLife\<sid>\<full>` and keeps their data folder; the caller's data folder is deleted.
- **Afterwards nothing brings it back offline:** `ProvisionPackageForAllUsersAsync` fails with
  0x80070490, `RegisterPackageByFamilyNameAsync` with 0x80073CF9 / 0x80070490, for Edge's
  package and the test package alike (still so after 20 s).

### 4. DismAPI in the Sandbox
- **DismAPI ships with the Windows ADK, not the Windows SDK.** `DismApi.h` and `DismApi.lib` are
  in neither SDK installed here (10.0.22621, 10.0.26100); Microsoft Learn lists the header and
  library under the ADK. `DismApi.dll` is in System32 of every Windows. The probe declares the
  types and functions it uses (from the documentation; structures packed as `DismApi.h` packs
  them) and loads the DLL with `LoadLibraryEx(..., LOAD_LIBRARY_SEARCH_SYSTEM32)`. Unelevated,
  `DismInitialize` returns 0x800702E4 (elevation required).
- **Servicing doesn't work in the Sandbox's image.** `DismInitialize` and
  `DismOpenSession(DISM_ONLINE_IMAGE)` succeed (10 ms, 82 ms), but:
  - `DismGetFeatures` fails with **0x800F0831** (`CBS_E_STORE_CORRUPTION`, 250-410 ms), and so
    does `DismGetFeatureInfo` for every name tried (TelnetClient, TFTP, SimpleTCP,
    Printing-XPSServices-Features, WorkFolders-Client, MSRDC-Infrastructure, NetFx3, ... 16 names,
    about 15 ms each). `dism.exe /online /get-features` and `Get-WindowsOptionalFeature -Online`
    fail the same way. CBS.log: "Store corruption, manifest missing for package:
    UserExperience-Recall-Package~...~10.0.26100.9974", which fails the foundation package and
    with it every feature. `dism /online /cleanup-image /checkhealth`: "The component store is
    repairable". The image's own startup already hits it (a `DisableFeature Recall` at boot).
  - `DismGetCapabilities` and `DismGetCapabilityInfo` (12 names) fail with **0x80072EE6**
    (WinINet 12006), as do `dism /online /get-capabilities` and `Get-WindowsCapability -Online`.
    CBS.log: the local FOD metadata repository is missing ("Failed to enumerate inbox fod
    metadata", 0x80070003), so CBS turns to a Windows Update search, which fails offline. That
    is, **listing capabilities itself can need Windows Update** when a machine has no local FOD
    metadata.
  - `Component Based Servicing\SessionsPending` exists from boot on; 90 s more changed nothing.
- So **no feature or capability operation could be tried**: enable/disable round trips,
  `LimitAccess`, `ERROR_SUCCESS_REBOOT_REQUIRED` (3010), the `*Pending` states, re-adding a
  removed capability offline, timings, and whether a restart inside the Sandbox completes a
  pending operation are all **not verified**. The structure layouts the probe declares are not
  verified by a successful call either.

### Other observations
- Removing a `NonRemovable` package (`RemovePackageAsync`, current user) is refused with
  0x80073CFA, extended 0x80070032: "This app is part of Windows and cannot be uninstalled on a
  per-user basis".
- `AppxAllUserStore` before any change held `Applications` (provisioned), `Config`,
  `DeferredRemoval`, `Deleted`, `EndOfLife`, `InboxApplications` (49), one key per user SID that
  has registrations, `Staged`, `UpdatedApplications` and `Upgrade`.

## Decision

**1. Windows apps: the public `PackageManager`, nothing else.** Avel calls it as `uninstall/registration.cpp`
does (`run_in_mta`), elevated:
- **List:** `FindPackagesWithPackageTypes(Main | Optional)` for all users, `FindUsers(full)` for
  who has each, `FindProvisionedPackages()` for what is provisioned. No cmdlet, no
  StateRepository or `AppxAllUserStore` reads (the spike read them only as evidence).
- **Soft removal (the current user):** `RemovePackageAsync(full, RemovalOptions::None)`.
  **Undo:** `RegisterPackageByFamilyNameAsync(family, {}, DeploymentOptions::None, nullptr,
  nullptr)`, which registers whatever version of the family is staged then (an update may have
  replaced the one removed). Success is judged by `IsRegistered` and `FindUsers`, never by the
  error code alone (finding 2's pitfall).
- **Soft removal for other users:** none; soft removal is for the current user only
  (**(maintainer) A**, below).
- **Stop installing for new users (deprovision):** `DeprovisionPackageForAllUsersAsync(family)`,
  undone by `ProvisionPackageForAllUsersAsync(family)` while the package is staged. Offline and
  reversible (finding 3). Offered as a mode of its own (**(maintainer) E**).
- **Deep removal:** `DeprovisionPackageForAllUsersAsync(family)` when provisioned, then
  `RemovePackageAsync(full, RemoveForAllUsers)`. Irreversible (finding 3): its plan item is
  irreversible like today's `PackageLocation`, and runs only with the irreversible confirmation
  (`allow_irreversible`).
- **Never:** `PreserveApplicationData` (refused for normal packages), `CleanupPackageForUserAsync`
  and the `AllowDeploymentInSpecialProfiles` policy, the Appx
  cmdlets, scheduled tasks or credentials to act as another user.

**2. What is refused, before any plan.**
- `SignatureKind::System` packages (the public stand-in for `NonRemovable`, finding 0; Windows
  refuses them anyway with 0x80073CFA / 0x80070032), frameworks and resource packages (not apps
  of their own), and the data list of critical packages (Store,
  App Installer, the shell's hosts, Settings).
- A soft removal that would leave the package with **no user and not provisioned**: Windows
  deletes the staged package at once (finding 2), so it isn't soft. Avel knows before the plan
  (`FindUsers` without the users being removed is empty and `FindProvisionedPackages` lacks the
  family) and offers it only as an irreversible removal.

**3. ChangeSet locations** (new kinds in ADR 0007's pattern: before and after, compare-and-set on
execute and undo, recovery by looking):

| Kind (JSON) | Location | Identity | State |
|---|---|---|---|
| `package-user` | `PackageUserLocation { family_name, full_name, user_sid, before, after }` | `package-user:<sid>\<family>` | registered for that user or not (`FindUsers`, any version of the family) |
| `package-provisioning` | `PackageProvisioningLocation { family_name, provisioned_full_name, before, after }` | `package-provisioning:<family>` | provisioned or not (`FindProvisionedPackages`) |
| `windows-feature` | `WindowsFeatureLocation { name, before, after }` | `windows-feature:<name>` | `DismPackageFeatureState` (enabled, disabled, and the pending states) |
| `windows-capability` | `WindowsCapabilityLocation { name, before, after, may_need_download }` | `windows-capability:<name>` | `DismPackageFeatureState` |

- The plan of a `package-user` item also records what made undo possible: the other users that had
  the package (SIDs) and whether it was provisioned. Undo registers only while the user doesn't
  have the family and some version of it is staged (`FindPackagesByPackageFamilyName` for any
  user, or `FindProvisionedPackages`); otherwise `restore-skipped (missing)`: "the app is no longer
  on this PC; reinstall it from the Store".
- `state.json` of a package item records the full name actually removed and, after undo, the one
  registered. The user's app data is not part of the backup (the removal deletes it; finding 2):
  the review says so, and the undo is of the registration only.
- A feature or capability item records the DISM result code; 3010
  (`ERROR_SUCCESS_REBOOT_REQUIRED`) or a `*Pending` state marks the set "restart pending", and
  undo treats the pending state of `after` as `after`. All of this follows DismAPI's
  documentation and is **not verified** (finding 4).

**4. DismAPI.**
- Avel declares the DismAPI types and functions it uses in `platform/dism` (as the probe does) and
  loads `DismApi.dll` from System32 at run time, instead of depending on the ADK for its header
  and import library (in the build and in CI). **(maintainer) F**
- Calls: `DismInitialize(DismLogErrors, ...)`, `DismOpenSession(DISM_ONLINE_IMAGE)`,
  `DismGetFeatures`/`DismGetFeatureInfo`, `DismEnableFeature(LimitAccess = TRUE, no sources,
  EnableAll = FALSE)`, `DismDisableFeature(RemovePayload = FALSE)`, `DismGetCapabilities`/
  `DismGetCapabilityInfo`, `DismAddCapability(LimitAccess = TRUE)`, `DismRemoveCapability`.
- Every failure is data, not an error of the tool: a listing that fails (0x800F0831, 0x80072EE6
  offline) is shown as "Windows can't list its features/capabilities now" with the code.
- Every capability removal is marked "may need Windows Update to come back" until a verification
  shows which capabilities can be added back offline. A feature whose payload is removed
  (`DismStateRemoved`) isn't offered for enabling offline.
- Nothing here can be verified in Windows Sandbox (finding 4); see **(maintainer) G**.

## Maintainer decisions (2026-11-14)
- **A.** Soft removal and its offline undo are for the current user only. Other users'
  registrations are shown; for them Avel offers deprovisioning and deep removal. This narrows the
  2026-10-31 decision ("the chosen users").
- **E.** Deprovisioning ("don't install for new users") is a mode of its own, next to soft and
  deep removal.
- **F.** Avel declares the DismAPI types and functions itself (`platform/dism`) and loads
  `DismApi.dll` from System32; no ADK dependency.
- **G.** Phase 6 is split: 6b is Windows apps alone, verified in the Sandbox. Windows features and
  capabilities move to a later sub-phase of their own, which first needs a verification
  environment with a full Windows image (a Hyper-V VM with checkpoints). Decisions 3 (the
  feature and capability locations) and 4 (DismAPI) above are its starting point.

## Open choices the maintainer was given
- **Soft removal for other (signed-out) users.** The public API can't do it (finding 1):
  - **A. The current user only** (recommended). Soft removal and its offline undo are for the
    account Avel runs as. Other users' registrations are shown; for them Avel offers deep removal
    (all users, irreversible) and deprovisioning (no new users). This narrows the 2026-10-31
    decision ("the chosen users").
  - **B. `CleanupPackageForUserAsync`**, setting `AllowDeploymentInSpecialProfiles` for the call and
    restoring it after. Public, immediate, keeps the user's data, but documented for deleted
    profiles: it leaves the registration in the user's profile (tiles, file associations), it
    flips a machine policy, and Avel can't undo it (registering again needs that user to sign
    in; whether a provisioned package comes back at their interactive logon is not verified).
  - **C. `Remove-AppxPackage -User` through PowerShell.** Not a public API for C++; it only marks
    the removal pending (finding 1), which no public call reports (`IsPackageRemovalPendingForUser`
    said False) or cancels, and which a batch logon didn't carry out. No undo.
  - **D. Running as the user** (a task with their password). Works (finding 0), but Avel would
    ask for and handle other users' passwords. S4U, which needs none, is refused even to SYSTEM.
- **E. Deprovisioning on its own** ("don't install for new users"), reversible offline, as a third
  mode next to soft and deep removal, or only inside deep removal.
- **F. DismAPI declarations in Avel** (recommended) or the ADK as a build dependency.
- **G. Where to verify features and capabilities.** The plan's `features` Sandbox scenario can't
  work in this image. Options: a Hyper-V VM with a full Windows image (a new harness), read-only
  listing on a developer machine (`avel_package_probe dism-features`, elevated) plus round trips
  in a VM, or building 6b's features part on the documentation alone and marking it unverified.

## Options considered
- **The Appx cmdlets** (from C++ through PowerShell): they reach private paths (`-User`) whose
  effect (a pending removal) Avel can neither observe nor undo. Rejected for Avel's code; the
  Sandbox scenarios keep using them to build fixtures and as a cross-check.
- **Reading the StateRepository or `AppxAllUserStore`** for state: undocumented and changes
  between builds; `FindUsers` and `FindProvisionedPackages` answer what Avel needs.
- **`PreserveApplicationData` to keep data across soft removal:** refused by Windows for packages
  not registered in development mode.
- **The ADK's `DismApi.h`:** a large optional install for a dozen declarations, on every developer
  machine and the CI runner.

## Consequences
- A package with an **external location** (a sparse package, or one whose files are a desktop
  program's folder: `EffectiveExternalPath`) gives that program its identity: PowerToys' context
  menus, Edge's, OneDrive's and the Microsoft 365 Copilot app's integration. Removed alone, the
  program breaks, so it is refused like the kinds in decision 2 (protection `external-location`,
  HardFloor `desktop-program-package` for registrations, provisioning and removal for every user),
  in the Windows apps tool and in the program list's Uninstall. It goes only with its program, as
  that program's leftover removed for one user (`related-packages`), which HardFloor still allows.
- Soft removal is honest about what it keeps: the package (staged or provisioned) and the
  ability to register it again offline, not the app's data.
- 6b's `apps` Sandbox scenario can build its fixtures as the spike does: the test MSIX staged and
  provisioned through the public API, the second user's registrations through a password task,
  and their hive stays loaded afterwards, so "signed out with the hive unloaded" isn't covered
  there.
- The features part waits for a verification environment the project doesn't have yet (G).
- `tests/helpers/package_probe.cpp` and `tests/sandbox/scenarios/spike_6b.ps1` stay as the spike's
  tools; the probe is a quick way to check an API's behaviour on a new Windows build.
