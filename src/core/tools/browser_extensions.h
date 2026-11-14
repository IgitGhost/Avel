// The browser extensions tool's list (Phase 6c): every extension of every Chromium-family and Firefox
// profile of every user, with how it was installed, and the "stubborn" sources that install it from
// outside the profile: policies and external registrations in the registry, and Firefox's
// distribution\policies.json. It only reads; profiles' files are never written. Codes and data only;
// the front ends turn them into text. See docs/tools.md ("Browser extensions") and ADR 0009.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/inventory/program.h"
#include "core/leftovers/environment.h"
#include "core/leftovers/leftover.h"
#include "core/platform/nt_registry.h"
#include "core/platform/registry_address.h"

namespace avel::tools {

enum class BrowserKind : std::uint8_t {
    Chrome,
    ChromeBeta,
    ChromeDev,
    ChromeCanary,  // Google\Chrome SxS
    Edge,
    EdgeBeta,
    EdgeDev,
    EdgeCanary,  // Microsoft\Edge SxS
    Brave,
    Vivaldi,
    Opera,
    OperaGx,
    Chromium,
    Firefox,
};

bool is_chromium_family(BrowserKind browser);

// How an extension came into its profile, from the profile's own record: Chromium's
// extensions.settings.<id>.location, Firefox's extensions.json location and install source.
enum class ExtensionInstall : std::uint8_t {
    Store,             // From the browser's store, by the user (Chromium internal + from the store;
                       // Firefox app-profile installed from addons.mozilla.org)
    User,              // By the user, not from the store (a .crx or .xpi file, another site)
    Developer,         // Unpacked or --load-extension (Chromium), a temporary add-on (Firefox)
    ExternalPref,      // Chromium: an external_extensions JSON file next to the browser or a program
    ExternalRegistry,  // Chromium: Software\...\Extensions\<id>; Firefox: Software\Mozilla\Firefox\Extensions
    Policy,            // Installed by a policy (Chromium policy locations; Firefox enterprise-policy)
    Sideload,          // Firefox: put in an extensions folder by another program (foreignInstall, app-global,
                       // app-system-share, app-system-local, app-system-user)
    Unknown,
};

// The store an extension comes from, by its update URL or (Firefox) Mozilla's signature.
enum class ExtensionStore : std::uint8_t {
    None,
    ChromeWebStore,  // https://clients2.google.com/service/update2/crx
    EdgeAddons,      // https://edge.microsoft.com/extensionwebstorebase/v1/crx
    OperaAddons,     // https://extension-updates.opera.com/api/omaha/update/
    Mozilla,         // Firefox: signed by addons.mozilla.org (signedState 2)
};

// A stubborn source: something outside the profile that installs the extension, so removing it in the
// browser doesn't stick.
enum class ExtensionSourceKind : std::uint8_t {
    // Chromium policy ExtensionInstallForcelist: a numbered value ("1", "2", ...), "id" or "id;update_url"
    ForceList,
    // Policy ExtensionSettings (Chromium or Firefox): the id's entry with installation_mode
    // force_installed or normal_installed, in the policy's JSON value or as a subkey (`settings_subkey`)
    Settings,
    // Chromium's external extension registration: Software\<vendor>\Extensions\<id>, with update_url, or
    // path and version
    ExternalRegistry,
    // Firefox's: a value of Software\Mozilla\Firefox\Extensions, named after the add-on id, holding a path
    FirefoxRegistry,
    // Firefox policy Extensions\Install: a numbered value with a URL or a path (not an id)
    FirefoxInstall,
    // Firefox policy Extensions\Locked: a numbered value with an add-on id (it can't be disabled or removed)
    FirefoxLocked,
    // Firefox's distribution\policies.json (ExtensionSettings, Extensions.Install or Extensions.Locked):
    // shown, never edited, never removable
    PoliciesJson,
};

struct ExtensionSource {
    ExtensionSourceKind kind = ExtensionSourceKind::ForceList;
    std::vector<BrowserKind> browsers;  // The browsers known to read it
    // Whose it is: empty for the machine's (HKLM, policies.json), else the user whose hive holds it.
    std::wstring user_sid;
    std::wstring user_name;
    bool wow64 = false;  // In HKLM's 32-bit view (WOW6432Node)
    // The key: the list's (ForceList, FirefoxInstall, FirefoxLocked), the policy's (Settings as a JSON
    // value), the id's own subkey (Settings as a subkey, ExternalRegistry), Software\Mozilla\Firefox\
    // Extensions (FirefoxRegistry). Unset for PoliciesJson.
    platform::RegistryAddress key;
    std::wstring value_name;                  // The numbered value, "ExtensionSettings" or the add-on id
    std::optional<platform::RawValue> value;  // As stored (for the planner's compare-and-set)
    // ForceList, FirefoxInstall, FirefoxLocked: every numbered value of the key as stored, by number,
    // so the planner can renumber the list without a gap.
    std::vector<platform::RawValue> list;
    // The id it installs; empty for a Firefox Install entry that matches no installed add-on.
    std::wstring extension_id;
    // What it says: the list entry, the install or update URL, the crx or xpi path.
    std::wstring data;
    std::wstring update_url;         // ForceList, Settings, ExternalRegistry
    std::wstring path;               // ExternalRegistry (path), FirefoxRegistry, a FirefoxInstall path
    std::wstring version;            // ExternalRegistry with a path
    std::wstring installation_mode;  // Settings: force_installed or normal_installed
    std::wstring settings_key;       // Settings: the key naming the id (may be "id1,id2")
    bool settings_subkey = false;    // Settings: written as subkeys rather than a JSON value
    std::wstring file;               // PoliciesJson: the policies.json file
    std::wstring program_id;  // ExternalRegistry, FirefoxRegistry: the program whose folder holds `path`
    // Listed and removable, but the browser doesn't read it: a Chromium registration in HKLM's 64-bit
    // view (Chromium reads HKLM's 32-bit view and HKCU only), or a ForceList value past the list's first
    // gap (Chromium stops reading there).
    bool ignored_by_browser = false;
};

// How a profile's extensions page opens: run `executable` with `arguments` (the profile's argument,
// then the page's URL). Data only: the GUI starts it.
struct ExtensionsPage {
    std::wstring executable;  // Empty when the browser's program wasn't found
    std::vector<std::wstring> arguments;
    std::wstring url;  // chrome://extensions, edge://extensions, ..., about:addons
};

struct BrowserProfile {
    BrowserKind browser = BrowserKind::Chrome;
    std::wstring user_sid;
    std::wstring user_name;
    // Chromium: the folder's name under User Data ("Default", "Profile 1"); empty for Opera, whose
    // profile is the folder itself. Firefox: the profile's folder name.
    std::wstring directory;
    std::wstring name;  // The display name: Local State's profile.info_cache, profiles.ini's Name
    std::wstring path;  // The profile's folder
    ExtensionsPage page;
};

// Why an extension can't be removed by Avel (only stubborn sources are, ADR 0009).
enum class ExtensionRemoval : std::uint8_t {
    Removable,  // Every stubborn source is a registry policy entry or registration Avel can take out
    // No stubborn source: the user or the browser's store installed it; the browser's page removes it
    NotRemovable,
    ManagedByPoliciesJson,  // Firefox's policies.json installs it: Avel never edits that file
    // A policy's subkey form names it with other ids ("id1,id2"): Avel can't take out one of them
    NotSupported,
};

struct BrowserExtension {
    // Stable across listings and runs: 10 hex digits of a hash of the browser, the user, the profile's
    // folder and the extension id (browser_extension_id). Registration-only entries hash their source.
    std::wstring id;
    BrowserKind browser = BrowserKind::Chrome;
    std::wstring
        user_sid;  // The profile's user; for a registration-only entry, its source's (empty: the machine)
    std::wstring user_name;
    std::wstring profile_directory;  // As BrowserProfile; empty for a registration-only entry
    std::wstring profile_name;
    std::wstring profile_path;
    // False for a stubborn source whose extension isn't (yet) in any profile: the browser installs it at
    // its next start, or it isn't installed at all.
    bool in_profile = true;
    std::wstring extension_id;  // Chromium: 32 letters a-p; Firefox: the add-on id
    std::wstring name;          // From the manifest, __MSG_x__ resolved from the default locale
    std::wstring description;
    std::wstring version;
    std::wstring folder;  // Where its files are (Chromium: Extensions\<id>\<version>; Firefox: the .xpi)
    bool enabled = true;
    std::uint32_t disable_reasons = 0;  // Chromium's bits, as stored
    ExtensionInstall install = ExtensionInstall::Unknown;
    std::wstring location;  // The profile's raw record: Chromium's location number, Firefox's location name
    bool installed_by_default = false;  // Chromium: was_installed_by_default or was_installed_by_oem
    ExtensionStore store = ExtensionStore::None;
    bool from_store = false;  // store != None
    std::wstring update_url;
    std::optional<std::int64_t> install_time;  // Milliseconds since 1970 (UTC)
    std::vector<std::wstring> permissions;     // API permissions
    std::vector<std::wstring>
        host_permissions;  // Sites: MV3 host_permissions, MV2 match patterns, Firefox origins
    std::vector<std::wstring> optional_permissions;  // Both kinds, optional
    std::vector<std::size_t> sources;                // Indexes into BrowserExtensionsList::sources
    ExtensionRemoval removal = ExtensionRemoval::NotRemovable;
    // It has sources, but its profile says the user installed it (install Store, User or Developer):
    // removing the sources leaves the user's own copy, which the browser's extensions page removes.
    bool keeps_user_copy = false;
    ExtensionsPage page;  // Its profile's extensions page; for a registration-only entry, the browser's
};

// Where the list comes from. The live one wraps a leftovers::LiveEnvironment; tests point the machine
// root and users' hives at scratch keys and their folders at temporary ones.
//
// A signed-out user's hive can be loaded privately only once per process, and LiveEnvironment keeps
// it loaded while it lives: destroy it (and so stop using this) before a ChangeSet that writes to that
// hive runs.
struct BrowserExtensionsEnvironment {
    leftovers::ScanEnvironment scan;               // Machine root, folders, users and their hives (borrowed)
    std::span<const inventory::Program> programs;  // Installed programs, for owners (borrowed)
    // Firefox's install folders, for distribution\policies.json and firefox.exe. Unset: found from App
    // Paths and Program Files.
    std::optional<std::vector<std::wstring>> firefox_folders;
    // Whether the machine's policies may be applied again (domain-joined or enrolled in MDM). Unset:
    // asked of Windows.
    std::optional<bool> managed;
};

enum class ExtensionDiagnosticCode : std::uint8_t {
    ProfileUnreadable,       // A profile's Preferences or Secure Preferences couldn't be read or parsed
    LocalStateUnreadable,    // A User Data folder's Local State couldn't be parsed (profiles taken from
                             // folders)
    AddonsUnreadable,        // A Firefox profile's extensions.json couldn't be read or parsed
    ProfilesIniUnreadable,   // A Firefox profiles.ini couldn't be read
    PolicyUnreadable,        // An ExtensionSettings value isn't JSON (the browser ignores it too)
    PoliciesJsonUnreadable,  // A policies.json couldn't be read or parsed
};

struct ExtensionDiagnostic {
    ExtensionDiagnosticCode code;
    std::wstring subject;  // The file, or the registry key and value
    std::int32_t error = 0;
};

struct BrowserExtensionsList {
    std::vector<BrowserProfile> profiles;      // Every profile found, with or without extensions
    std::vector<BrowserExtension> extensions;  // By browser, user, profile, then name
    std::vector<ExtensionSource> sources;
    // The machine is joined to a domain or enrolled in MDM: removed policy entries may come back.
    bool policies_managed = false;
    std::vector<ExtensionDiagnostic> diagnostics;
};

// Lists every profile's extensions, then the stubborn sources, matched to them by id. Component and
// built-in extensions (Chromium's component locations, Firefox's app-builtin and system add-ons) and
// themes aren't listed. Skips what this account can't read.
BrowserExtensionsList list_browser_extensions(const BrowserExtensionsEnvironment& environment);

// An entry's id from its parts (see BrowserExtension::id).
std::wstring browser_extension_id(BrowserKind browser, std::wstring_view user_sid,
                                  std::wstring_view profile_path, std::wstring_view extension_id);

// The value is one HardFloor lets the browser extensions planner edit (a RestoreValue edit): a numbered
// value of a browser's ExtensionInstallForcelist or of Firefox's Extensions\Install or Locked, or the
// ExtensionSettings value of a browser's policy key (Software\Policies\<vendor>, HKLM in either view or a
// user's hive).
bool is_extension_policy_value(const platform::RegistryAddress& key, std::wstring_view value_name);

// The store an update URL points at (ignoring case and a trailing slash or query).
ExtensionStore store_of_update_url(std::wstring_view update_url);

std::string_view to_string(BrowserKind browser);
std::string_view to_string(ExtensionInstall install);
std::string_view to_string(ExtensionStore store);
std::string_view to_string(ExtensionSourceKind kind);
std::string_view to_string(ExtensionRemoval removal);
std::string_view to_string(ExtensionDiagnosticCode code);
bool parse(std::string_view text, BrowserKind& browser);

}  // namespace avel::tools
