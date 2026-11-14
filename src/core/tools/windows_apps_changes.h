// Changes to the Windows apps tool's apps (tools/windows_apps): the ChangePlans that remove apps for
// the current user, stop installing them for new users, or remove them for everyone. Codes and data
// only; the front ends turn them into text. See docs/tools.md ("Windows apps") and ADR 0008.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/changes/change_set.h"
#include "core/tools/windows_apps.h"

namespace avel::tools {

enum class WindowsAppsPlanIssueCode : std::uint8_t {
    UnknownId,  // No app has this id or family name
    // Protected (AppProtection): signed as part of Windows, on the critical list, or part of a desktop
    // program (an external location). Never changed.
    Protected,
    // The listing was made without administrator rights, so who else has the app and whether it is
    // provisioned aren't known; nothing is planned from it.
    IncompleteListing,
    NotInstalledForCurrentUser,  // Remove for me: the current user doesn't have it
    // Remove for me: nobody else has it and it isn't provisioned, so Windows would delete the
    // package at once and undo couldn't bring it back. Only removal for everyone (irreversible) can.
    WouldDeletePackage,
    NotProvisioned,  // Don't install for new users: it isn't provisioned
    // Remove for everyone without the confirmation that it can't be undone.
    IrreversibleNeedsConfirmation,
};

struct WindowsAppsPlanIssue {
    std::wstring id;  // As asked
    WindowsAppsPlanIssueCode code;
};

struct WindowsAppsPlan {
    changes::ChangePlan plan;                  // Its description is the caller's to set
    std::vector<std::wstring> planned;         // The ids of the apps the plan changes, in order
    std::vector<WindowsAppsPlanIssue> issues;  // In the order of the ids
};

// Every plan takes apps by id or family name (compared ignoring case; each planned once) and
// plans nothing for an id with an issue. Planned items carry the app's display name as their
// program and no confidence: the user chose them.

// "Remove for me": the current user's registration (PackageUserLocation, registered to not). Undone
// by registering the family again, which works while the package stays on this PC: another user
// has it or it is provisioned (WindowsApp::soft_undoable). The app's data isn't kept.
WindowsAppsPlan plan_remove_for_me(const WindowsAppsList& list, std::span<const std::wstring> ids);

// "Don't install for new users": the family deprovisioned (PackageProvisioningLocation, provisioned
// to not). Users who have it keep it. Undone by provisioning it again while it is on this PC.
WindowsAppsPlan plan_deprovision(const WindowsAppsList& list, std::span<const std::wstring> ids);

// "Remove for everyone": deprovisioned first when provisioned (PackageProvisioningLocation), then
// every version of the family removed for all users (PackageLocation with all_users), which also
// deletes it from this PC. Irreversible: planned only with `irreversible_confirmed`.
WindowsAppsPlan plan_remove_for_everyone(const WindowsAppsList& list, std::span<const std::wstring> ids,
                                         bool irreversible_confirmed);

std::string_view to_string(WindowsAppsPlanIssueCode code);

}  // namespace avel::tools
