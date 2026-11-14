// The Windows apps tool's list (Phase 6b): the packaged apps on this PC, one per package family, with
// who has each, whether it is provisioned for new users, its category from the data list
// (windows_apps_data.h), its protection and the ways it can be removed. It only reads. Codes and data
// only; the front ends turn them into text. See docs/tools.md ("Windows apps") and ADR 0008.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/platform/packages.h"
#include "core/tools/windows_apps_data.h"

namespace avel::tools {

enum class AppProtection : std::uint8_t {
    None,
    System,    // Signed as part of Windows (SignatureKind System): Windows refuses to remove it
    Critical,  // On the data list's critical list (AppCategory::Critical)
    // It gives a desktop program its identity (a sparse package, or one with an external location:
    // WindowsApp::external_location), for its context menu, notifications or Windows integration.
    // It goes with that program, when the program is uninstalled; removed alone, the program breaks.
    ExternalLocation,
};

// A user the app is registered for.
struct WindowsAppUser {
    std::wstring sid;
    std::wstring account_name;  // "DOMAIN\user"; the SID when it can't be resolved
    bool current = false;       // The user Avel runs as
};

// One version of the family on this PC (usually there is one; another user may have another version).
struct WindowsAppPackage {
    std::wstring full_name;
    std::wstring version;
    std::vector<std::wstring> user_sids;  // Who has this one registered
};

struct WindowsApp {
    // Stable across listings and runs: 10 hex digits of a hash of the family name, lowercase
    // (windows_app_id).
    std::wstring id;
    std::wstring family_name;  // e.g. Microsoft.BingNews_8wekyb3d8bbwe
    std::wstring name;         // The identity name, e.g. Microsoft.BingNews
    // From the package the current user has, else the newest one.
    std::wstring display_name;
    std::wstring publisher;
    std::wstring version;
    std::wstring install_location;
    // The desktop program's folder when the app is part of one (AppProtection::ExternalLocation);
    // empty otherwise.
    std::wstring external_location;
    platform::PackageSignatureKind signature_kind = platform::PackageSignatureKind::None;
    std::vector<WindowsAppPackage> packages;  // Newest first
    // The full name of the current user's package; empty when they don't have the app.
    std::wstring current_user_full_name;
    std::vector<WindowsAppUser> users;  // The current user first, then by account name
    // Provisioned: installed for every new user. Unknown (false) without administrator rights.
    bool provisioned = false;
    std::wstring provisioned_full_name;
    AppCategory category = AppCategory::Other;
    AppWarning warning = AppWarning::None;  // For SemiCritical
    AppProtection protection = AppProtection::None;
    // Removal for the current user keeps the package on this PC, so undo can register it again:
    // another user has it, or it is provisioned (ADR 0008, finding 2). False when the listing
    // couldn't tell (without administrator rights).
    bool soft_undoable = false;
    // The ways it can be removed now (all false when protected, and when the listing isn't complete:
    // WindowsAppsList::all_users false, the planner's incomplete-listing):
    bool can_remove_for_me = false;        // The current user has it and soft_undoable
    bool can_deprovision = false;          // Provisioned
    bool can_remove_for_everyone = false;  // Not protected (irreversible)
};

// Where the list comes from. The live one asks PackageManager and resolves SIDs with
// LookupAccountSid; tests inject a listing, the current user and account names.
struct WindowsAppsEnvironment {
    // When set, this instead of platform::list_packages() (tests).
    std::optional<platform::PackageListing> listing;
    std::wstring current_user_sid;
    // "DOMAIN\user" of a SID; empty or unset: shown as the SID.
    std::function<std::wstring(const std::wstring& sid)> account_name;
};

// The current user and LookupAccountSid, with a cache of its own.
WindowsAppsEnvironment live_windows_apps_environment();

struct WindowsAppsList {
    std::vector<WindowsApp> apps;  // By display name
    // Every user's packages and the provisioned ones were read (administrator rights). Without, the
    // list holds only the current user's apps, nothing is known to be provisioned, no removal for
    // the current user counts as undoable, and no app offers a way to remove it: the planner refuses
    // every id (incomplete-listing). The front ends tell why from this flag.
    bool all_users = false;
    std::int32_t error = 0;  // The HRESULT that cut the listing short, 0 when complete
};

// The apps: every main and optional package, grouped by family. Frameworks and resource packages
// aren't apps of their own and aren't listed. Protected apps (System, Critical, ExternalLocation)
// are listed, with no way to remove them.
WindowsAppsList list_windows_apps(const WindowsAppsEnvironment& environment);

// An app's id, from its family name.
std::wstring windows_app_id(std::wstring_view family_name);

std::string_view to_string(AppProtection protection);

}  // namespace avel::tools
