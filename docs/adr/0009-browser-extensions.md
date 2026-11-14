# ADR 0009: Browser extensions: only stubborn sources are removed

- **Status:** Accepted. Builds on [ADR 0004](0004-changeset-folder-journal.md) and
  [ADR 0007](0007-changeset-state-changes.md).
- **Date:** 2026-11-14

## Context
Phase 6c lists the extensions of every Chromium-family (Chrome, Edge, Brave, Vivaldi, Opera,
Chromium) and Firefox profile of every user, and removes the ones the user can't get rid of in the
browser: extensions that a **policy** (`ExtensionInstallForcelist`, `ExtensionSettings`, Firefox's
`Extensions\Install`) or an **external registration** (`Software\<vendor>\Extensions\<id>`,
Firefox's `Software\Mozilla\Firefox\Extensions`) installs. Adware and bundled installers use both,
so the browser's own "Remove" is greyed out or the extension comes back at the next start.

An extension also lives in its profile: Chromium's `Preferences` and `Secure Preferences`
(`extensions.settings`), Firefox's `extensions.json` and `prefs.js`. Both browser families protect
those files: Chromium's `Secure Preferences` carries MACs keyed per machine, and a profile edited
behind the browser's back is reset or flagged as tampered with; the browser may also be running and
write its own copy over the edit.

The maintainer's decisions (2026-10-31 and 2026-11-14, `plan.md`): every extension is listed;
only the policy-forced and externally registered ones are removed, with undo; the rest get "open
the extensions page"; profiles' files are never edited; a policy loses only the extension's own
entry; Firefox's `distribution\policies.json` is shown, not edited; no spike.

## Decision

**1. Only stubborn sources are changed, never a profile.** The listing reads profiles
(both Preferences files, manifests, `extensions.json`, `profiles.ini`) and never writes them. An
extension with no stubborn source is `not-removable`: the front ends open the browser's extensions
page for that profile (`--profile-directory=<folder>`, Firefox `-profile <path> about:addons`),
where the user removes it. With its sources gone, the browser uninstalls what it installed because
of them at its next start (Chromium's and Mozilla's documented behaviour for a removed policy entry
or registration).

**2. A policy loses only the extension's entry.**
- A numbered list (`ExtensionInstallForcelist`, Firefox's `Extensions\Install` and `Locked`) is
  **renumbered without a gap**: the values after the entry, in the run `"1".."k"` that Chromium
  reads (it stops at the first missing number), move down one, and the last is deleted. Values past
  an existing gap stay inert and in place (only a removed one is deleted), so nothing the browser
  ignored becomes active.
- A JSON `ExtensionSettings` value loses the extension's key, or its id from a `"id1,id2"` key; the
  rest is kept and re-serialized compactly in the value's type. A value left empty is **deleted**
  rather than written as `{}`: an empty dictionary policy and no policy mean the same, and undo
  writes the old value back either way.
- Each value written or deleted is a `ValueEditLocation` with `RestoreValue`: compare-and-set
  against the value as listed, exact undo while the value is what Avel wrote (ADR 0007's pattern).
  An administrator's change made in between is never overwritten.
- The subkey form of `ExtensionSettings` loses the id's subkey (`RegistryKeyLocation`); a subkey
  named for several ids is refused (`not-supported`): taking one id out would mean renaming a key.

**3. Registrations go whole**: `Software\<vendor>\Extensions\<id>` as a key, Firefox's value as a
value, both with their exact backup.

**4. All of an extension's sources go together**, as one ChangeSet; an extension id asked for takes
every removable entry of it. Sources several entries share (a machine policy installs the extension
in every profile of every user) are planned once, and the planner reports the entries affected.

**5. `policies.json` is read-only.** An extension it installs is `managed-by-policies-json`: the
GUI offers to open the file; Avel never edits it (it belongs to whoever deployed Firefox, and an
update of Firefox's folder may replace it).

**6. A narrow HardFloor rule.** Value edits were allowed only in the environment and SharedDLLs.
HardFloor now also allows a `RestoreValue` edit that writes a string or deletes the value, only on a
numbered value of a known vendor's `ExtensionInstallForcelist` (or Firefox's `Extensions\Install`,
`Locked`) or on a known vendor's `ExtensionSettings` value, in HKLM (either view) or a user's hive
(`tools::is_extension_policy_value`). The `Policies` keys themselves stay core keys.

**7. No spike.** Offline, Chrome and Edge outside a domain install forced and registry-registered
extensions only from their stores, so the Sandbox can't show the browser's own reaction to a removed
entry. That the extension goes at the next start comes from Chromium's and Mozilla's documentation.
The `extensions` Sandbox scenario checks the registry, the files and undo through dumps, and its GUI
round checks that Edge opens its extensions page.

## Options considered
- **Editing `Preferences`/`Secure Preferences` or `extensions.json`** to remove any extension:
  rejected (decision 1). It fights the browsers' tamper protection and a running browser, and the
  browser's page does it properly.
- **Deleting a whole policy value or key** (the forcelist, `ExtensionSettings`): simpler, but it
  takes the administrator's other extensions and settings with it. Rejected.
- **Leaving a gap in a list:** Chromium would silently stop reading at it and drop every later
  extension. Rejected for renumbering.
- **Keeping `{}` for an emptied JSON policy:** equivalent for the browser; deleting leaves the
  registry as it was before the policy was written.
- **Editing `policies.json`:** rejected (decision 5).

## Consequences
- Removal acts at the browser's next start; the GUI says so. A running browser keeps the extension
  until then.
- On a domain-joined or MDM-enrolled machine (`policies_managed`), Group Policy or the server may
  write a removed entry again; the front ends warn.
- Vendor paths are partly uncertain: Chromium hard-codes `Software\Google\Chrome\Extensions` (HKLM's
  32-bit view and HKCU); Edge uses `Software\Microsoft\Edge\Extensions`; Brave's, Vivaldi's and
  Chromium's own registration keys, Vivaldi's policy key and whether Opera reads anything are not
  documented. The listing reads the likely keys and matches by id, and an extension its profile says
  came from the registry also takes another vendor's registration of the same id
  ([tools.md](../tools.md), "Browser extensions").
- **Sources the browser doesn't read stay removable, flagged** (maintainer, 2026-11-14): a Chromium
  registration in HKLM's 64-bit view and a force-list value past the list's first gap are listed
  with `ignored_by_browser`; removing them is harmless and leaves the registry clean.
- **The user's own copy** (maintainer, 2026-11-14): when a program registers (or a policy names) an
  extension the profile says the user installed, from the store, a file or unpacked, removing the
  sources leaves that copy. Such an entry stays removable and is flagged (`keeps_user_copy`; the
  planner's `keeps-user-copy` note), so the front ends say the copy stays and point to the
  browser's extensions page.
- A partial run (a value changed in between) can leave a list with a value twice or a gap until it
  is undone; undo puts every value back exactly.
