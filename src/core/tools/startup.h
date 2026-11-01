// The Startup manager's list: everything that starts with Windows, for the machine and every
// user, with its state, what it runs, the program it belongs to, and the items that deleting it
// removes. It only reads. Codes and data only; the front ends turn them into text. See
// docs/tools.md ("Startup manager").
#pragma once

#include <cstdint>
#include <functional>
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
#include "core/platform/scheduled_tasks.h"
#include "core/platform/services.h"
#include "core/platform/version_info.h"

namespace avel::tools {

enum class StartupKind : std::uint8_t {
    Run,            // A value of a Run key
    RunOnce,        // A value of a RunOnce key: run at the next log on, then deleted by Windows
    RunOnceEx,      // A subkey of the machine's RunOnceEx key (delete only)
    StartupFolder,  // A file in a Startup folder, usually a shortcut
    Task,           // A scheduled task with a logon or boot trigger, outside \Microsoft
    Service,        // A Win32 service that starts automatically
};

enum class StartupState : std::uint8_t {
    Enabled,
    // Run, StartupFolder: disabled in StartupApproved (as Task Manager does it); Task: the task is
    // disabled; Service: its start is Manual or Disabled.
    Disabled,
};

enum class StartupProtection : std::uint8_t {
    None,
    WindowsOwn,  // Windows' own: a Microsoft-signed file under the Windows folder. Listed, never changed
};

// What the signature check says about a file (platform::verify_signature, plugged in by the front
// ends through StartupEnvironment::signer).
struct StartupSigner {
    bool checked = false;    // The file's signature was looked at
    bool valid = false;      // Signed, validly, by a trusted publisher
    bool microsoft = false;  // Signed by Microsoft
    std::wstring name;       // The signer's name
};

// Called from several threads at once, once per distinct file.
using SignerFunction = std::function<StartupSigner(const std::wstring& path)>;

// The StartupApproved value Task Manager writes to disable or enable a Run value or a Startup folder
// item: REG_BINARY, 12 bytes; the first byte's low bit set means disabled (03, 01), clear means
// enabled (02, 06). No value means enabled.
struct StartupApproval {
    platform::RegistryAddress key;            // ...\Explorer\StartupApproved\Run, Run32 or StartupFolder
    std::wstring value_name;                  // The Run value's name, or the Startup folder file's name
    std::optional<platform::RawValue> value;  // As stored; nullopt when there is none
};

struct StartupEntry {
    // Stable across listings and runs: 10 hex digits of a hash of the first delete location's
    // identity (startup_id).
    std::wstring id;
    StartupKind kind = StartupKind::Run;
    std::wstring user_sid;   // Whose it is; empty for the machine's
    std::wstring user_name;  // "DOMAIN\user"
    bool wow64 = false;      // From a WOW6432Node key (the 32-bit view)
    // The value's name (Run, RunOnce), the subkey's (RunOnceEx), the file's (StartupFolder), the
    // task's path or the service's name.
    std::wstring name;
    std::wstring display_name;  // A service's display name
    // What it runs, as written: the value's data, a shortcut's target and arguments, a task's first
    // Exec action, a service's image path. Empty for an advertised shortcut and a task that runs
    // only COM handlers.
    std::wstring command;
    std::uint32_t value_type = 0;  // Run, RunOnce: the value's type (REG_SZ, REG_EXPAND_SZ)
    // The file it runs, or the script or DLL a Windows host (rundll32, powershell -File, wscript,
    // cscript) or svchost runs: expanded for its user, normalized, 8.3 names expanded. Empty when
    // it can't be told.
    std::wstring target;
    bool target_missing = false;    // Known to be gone: "not found" on a fixed disk
    platform::FileProduct product;  // From the target's version resource
    StartupSigner signer;           // The target's signature
    std::wstring program_id;        // The installed program whose folder holds the target
    StartupState state = StartupState::Enabled;
    // A service listed only because Avel changed its start (it isn't automatic now).
    bool disabled_by_avel = false;
    // Run (but not a user's 32-bit Run key) and StartupFolder: where Task Manager's state is kept.
    std::optional<StartupApproval> approval;
    bool task_enabled = false;        // Task: the task's Enabled flag
    std::uint32_t service_start = 0;  // Service: SERVICE_AUTO_START, SERVICE_DEMAND_START, ...
    bool service_delayed = false;     // Service: delayed automatic start
    // What deleting it removes, first the item itself: the value (RegistryValueLocation), the
    // RunOnceEx subkey (RegistryKeyLocation), the file (FileLocation), the task (TaskLocation) or
    // the service (ServiceLocation); then its StartupApproved value, if it has one.
    std::vector<leftovers::Location> delete_locations;
    StartupProtection protection = StartupProtection::None;
};

// Where the list comes from. The live one wraps a leftovers::LiveEnvironment; tests point the
// machine root and users' hives at scratch keys and inject tasks and services.
//
// A signed-out user's hive can be loaded privately only once per process (RegLoadAppKey), and
// LiveEnvironment keeps it loaded while it lives: destroy it (and so stop using this) before a
// ChangeSet that writes to that hive runs.
struct StartupEnvironment {
    leftovers::ScanEnvironment scan;               // Machine root, folders, users and their hives (borrowed)
    std::span<const inventory::Program> programs;  // Installed programs, for owners (borrowed)
    SignerFunction signer;                         // Unset: nothing is checked, nothing counts as Microsoft's
    // Services Avel disabled (ChangeSets done and not undone): listed whatever their start.
    std::vector<std::wstring> avel_disabled_services;
    // When set, these instead of the Task Scheduler's and the Service Control Manager's (tests).
    std::optional<std::vector<platform::ScheduledTask>> tasks;
    std::optional<std::vector<platform::ServiceInfo>> services;
};

// A source the list couldn't read. Users' hives that couldn't be opened are in
// StartupEnvironment::scan.diagnostics.
enum class StartupDiagnosticCode : std::uint8_t {
    TasksUnavailable,     // The Task Scheduler couldn't be reached: no tasks listed
    ServicesUnavailable,  // The Service Control Manager couldn't be read: no services listed
};

struct StartupDiagnostic {
    StartupDiagnosticCode code;
    std::int32_t error = 0;
};

struct StartupList {
    std::vector<StartupEntry> entries;
    std::vector<StartupDiagnostic> diagnostics;
};

// Lists the entries, machine first, then each user. Skips what this account can't read. Microsoft's
// tasks (\Microsoft) and services whose file is Microsoft-signed aren't listed. Initializes COM on
// the calling thread if it isn't yet.
StartupList list_startup(const StartupEnvironment& environment);

// An entry's id, from its first delete location.
std::wstring startup_id(const leftovers::Location& location);

std::string_view to_string(StartupKind kind);
std::string_view to_string(StartupState state);
std::string_view to_string(StartupProtection protection);
std::string_view to_string(StartupDiagnosticCode code);

}  // namespace avel::tools
