// Avel Trace: what an installer changed, found by comparing the system before and after it ran
// (ADR 0002). These types are what the tracker's parts produce: the USN journal reader for files,
// the registry snapshot for keys and values, and the known spots whose exact values are saved.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/leftovers/leftover.h"
#include "core/platform/nt_registry.h"
#include "core/platform/registry_address.h"

namespace avel::tracker {

enum class ChangeKind : std::uint8_t {
    Created,   // Didn't exist when the trace started and exists at its end
    Deleted,   // Existed at the start and is gone at the end (shown only)
    Modified,  // Existed at the start; its content or values changed (shown only)
    Renamed,   // Existed at the start under another name or folder (shown only)
};

// Why a change is set aside as something Windows, the user or Avel did rather than the installer.
// Noise stays in the trace with its reason but is never offered for removal.
enum class NoiseReason : std::uint8_t {
    None,
    SystemActivity,  // Windows' own logs, caches, telemetry, update, indexing and network state
    UserActivity,    // Recent files, Explorer's view state, browser caches
    Temporary,       // Temp folders
    Avel,            // Avel's own files and keys
    Unresolved,      // Its path couldn't be determined
};

struct TraceChange {
    ChangeKind change = ChangeKind::Created;
    // Where the item is at the end of the trace; for Deleted, where it was. Files and folders are
    // FileLocation; registry items RegistryKeyLocation or RegistryValueLocation, except a created
    // service (ServiceLocation), scheduled task (TaskLocation) or firewall rule
    // (FirewallRuleLocation), which are named the way the rest of Avel removes them.
    leftovers::Location location;
    std::wstring old_path;       // Renamed files and folders: where it was at the start
    std::uint64_t contents = 0;  // Created folders and keys: items created inside (only roots are listed)
    bool replaced = false;       // Modified file: a new file took the old one's place (temp-then-rename)
    NoiseReason noise = NoiseReason::None;
};

// Places whose values the trace saves exactly at its start, so removal can take back precisely
// what the installer added to them (5b): a PATH entry, a SharedDLLs reference count.
enum class KnownSpot : std::uint8_t {
    MachineEnvironment,  // HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment
    UserEnvironment,     // HKU\<sid>\Environment
    SharedDlls,          // HKLM\SOFTWARE[\WOW6432Node]\Microsoft\Windows\CurrentVersion\SharedDLLs
};

struct KnownValueChange {
    KnownSpot spot = KnownSpot::MachineEnvironment;
    platform::RegistryAddress key;
    std::wstring name;
    std::optional<platform::RawValue> before;  // nullopt: the value didn't exist at the start
    std::optional<platform::RawValue> after;   // nullopt: it was deleted
};

std::string_view to_string(ChangeKind change);
std::string_view to_string(NoiseReason reason);
std::string_view to_string(KnownSpot spot);

}  // namespace avel::tracker
