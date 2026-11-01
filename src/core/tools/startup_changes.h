// Changes to the Startup manager's entries (tools/startup): the ChangePlan that disables, enables
// or deletes some of them, and the services that Avel's ChangeSets disabled. Codes and data only;
// the front ends turn them into text. See docs/tools.md ("Changes").
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/changes/change_set.h"
#include "core/changes/store.h"
#include "core/tools/startup.h"

namespace avel::tools {

enum class StartupAction : std::uint8_t {
    // Run and Startup folder entries: turned off in StartupApproved, as Task Manager does it
    // (StartupApprovedLocation); a task: its Enabled flag off (TaskStateLocation); a service: its
    // start Manual or Disabled (ServiceStartLocation).
    Disable,
    Enable,  // The same, back on; a service's start Automatic
    // The entry's delete locations: the value, the RunOnceEx subkey, the file, the task or the
    // service, and its StartupApproved value if it has one.
    Delete,
};

// What a disabled service's start becomes.
enum class ServiceDisable : std::uint8_t {
    Manual,    // SERVICE_DEMAND_START: it still starts when a program asks for it
    Disabled,  // SERVICE_DISABLED: it never starts
};

enum class StartupPlanIssueCode : std::uint8_t {
    UnknownId,  // No entry has this id
    Protected,  // Windows' own entry (StartupProtection::WindowsOwn): never changed
    // Disable or enable of an entry that has no state of its own: RunOnce, RunOnceEx, a user's
    // 32-bit Run value. They can only be deleted.
    NotSupported,
    // Already disabled (for a service, at least as far as asked: Disabled covers Manual, not the
    // other way round) or already enabled: nothing planned. Not an error.
    AlreadyInState,
};

struct StartupPlanIssue {
    std::wstring id;  // As asked
    StartupPlanIssueCode code;
};

struct StartupPlan {
    changes::ChangePlan plan;              // Its description is the caller's to set
    std::vector<std::wstring> planned;     // The ids of the entries the plan changes, in order
    std::vector<StartupPlanIssue> issues;  // In the order of the ids
};

// The issue keeps the request from going ahead (all but AlreadyInState).
bool is_error(StartupPlanIssueCode code);

// The plan for doing `action` to the entries with these `ids` (compared ignoring case; each
// planned once). Disabling a Run or Startup folder entry is stamped with `now`, as Task Manager
// does. A service keeps its delayed flag, so only its start type changes and undo is exact.
// `program_name` turns an owner program id into the name the Undo Center shows (the id itself
// when empty). Planned items carry no confidence: the user chose them.
StartupPlan plan_startup(
    std::span<const StartupEntry> entries, StartupAction action, std::span<const std::wstring> ids,
    ServiceDisable service_disable, FILETIME now,
    const std::function<std::wstring(const std::wstring& program_id)>& program_name = {});

// The services Avel disabled: named by ServiceStartLocation items that are done (or whose undo
// failed) in the store's sets, and whose start Avel set to something other than Automatic. When
// several sets changed one service, the newest such item decides. Only reads the sets; sets a live
// run holds are skipped. Hold store.lock() around it, as for any read of the sets. Feeds
// StartupEnvironment::avel_disabled_services.
std::vector<std::wstring> services_disabled_by_avel(changes::ChangeStore& store);

std::string_view to_string(StartupAction action);
std::string_view to_string(ServiceDisable disable);
std::string_view to_string(StartupPlanIssueCode code);

}  // namespace avel::tools
