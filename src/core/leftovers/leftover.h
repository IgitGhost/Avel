#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/leftovers/confidence.h"
#include "core/platform/nt_registry.h"
#include "core/platform/registry_address.h"

namespace avel::leftovers {

struct FileLocation {
    std::wstring path;  // Normalized
    bool directory = false;
};

struct RegistryKeyLocation {
    platform::RegistryAddress key;
};

struct RegistryValueLocation {
    platform::RegistryAddress key;
    std::wstring value_name;  // Empty for the key's default value
};

struct ServiceLocation {
    std::wstring name;    // The service's key name
    bool driver = false;  // A kernel or file system driver: shown, never removed
};

struct TaskLocation {
    std::wstring path;    // e.g. \Contoso\Updater
    bool folder = false;  // A task folder (\Contoso): removed only when empty
};

struct PackageLocation {
    std::wstring full_name;
    std::wstring user_sid;  // The user the package is registered for
};

struct FirewallRuleLocation {
    std::wstring rule_id;  // The value name under FirewallPolicy\FirewallRules
};

// How a value that other programs share is edited (see value_edits.h).
enum class ValueEditKind : std::uint8_t {
    RemovePathEntries,   // Take the installer's entries out of a ;-separated list such as Path
    DecrementSharedDll,  // Lower a SharedDLLs reference count by the installer's increment
    DeleteValue,         // Delete a value the installer created
    RestoreValue,        // Put back the value as it was before the install
};

// An edit to a value in the environment or SharedDLLs, rather than its removal. It is applied only
// while the value is still `expected` (compare-and-set), so nothing changed since is overwritten.
struct ValueEditLocation {
    platform::RegistryAddress key;
    std::wstring value_name;
    ValueEditKind kind = ValueEditKind::RestoreValue;
    std::optional<platform::RawValue> expected;  // The value it applies to; nullopt: the value is absent
    std::optional<platform::RawValue> desired;   // The value written; nullopt: the value is deleted
    std::vector<std::wstring> entries;           // RemovePathEntries: the entries taken out, as written
    std::int64_t delta = 0;                      // SharedDLLs: how much the count goes down
};

// State changes (ADR 0007): an item whose state changes rather than its removal, put back on undo.
// Each holds the state it applies to and the state it sets; the executor sets it only while the
// item is still in the first (compare-and-set), and undo puts the first back only while the item is
// still in what was set.

// Explorer's record of whether a Run value or a Startup folder item starts (Task Manager's Startup
// apps, Settings > Apps > Startup): a 12-byte REG_BINARY named after the entry, in one of the
// StartupApproved keys (see startup_approved.h).
struct StartupApprovedLocation {
    platform::RegistryAddress key;  // ...\Explorer\StartupApproved\Run, Run32 or StartupFolder
    std::wstring value_name;        // The Run value's name, or the Startup folder item's file name
    std::optional<platform::RawValue> expected;  // The value it applies to; nullopt: the value is absent
    std::optional<platform::RawValue> desired;   // The value written; nullopt: the value is deleted
};

// A scheduled task turned off or on (IRegisteredTask::put_Enabled).
struct TaskStateLocation {
    std::wstring path;  // e.g. \Contoso\Updater
    bool enabled_before = true;
    bool enabled_after = false;
};

// How a service starts, as the Service Control Manager reports it.
struct ServiceStart {
    std::uint32_t start_type = 0;  // SERVICE_AUTO_START, SERVICE_DEMAND_START, SERVICE_DISABLED, ...
    bool delayed = false;          // SERVICE_CONFIG_DELAYED_AUTO_START_INFO (meaningful for automatic)

    bool operator==(const ServiceStart&) const = default;
};

// A service's start type changed: Automatic to Manual or Disabled, and back.
struct ServiceStartLocation {
    std::wstring name;  // The service's key name
    ServiceStart before;
    ServiceStart after;
};

using Location = std::variant<FileLocation, RegistryKeyLocation, RegistryValueLocation, ServiceLocation,
                              TaskLocation, PackageLocation, FirewallRuleLocation, ValueEditLocation,
                              StartupApprovedLocation, TaskStateLocation, ServiceStartLocation>;

// A case-insensitive key identifying the item, used to merge what several scanners found:
// "file:c:\x", "key:hklm\software\x", "value:hku\s-1-5-21-…\software\x\name", ... A value edit's
// key also holds what it does ("edit:hklm\…\environment\\path|remove-path-entries|c:\x\bin"), so
// two traces' edits to one value stay two items. A state change's key names only the item
// ("approved:…\\name", "task-state:\x", "service-start:x"), whatever states it goes between.
std::wstring identity(const Location& location);

// `outer` contains `inner`: a folder and something below it, or a key and a subkey or value of it.
bool contains(const Location& outer, const Location& inner);

// The user whose profile or hive holds the item; empty for machine-wide items.
std::wstring owner_sid(const Location& location);

// One item a created folder or key held when its install trace stopped, relative to it: a file or
// folder ("bin\app.exe"), a subkey ("Shell\Open"), or a value (`value` set: a value of the key at
// `path`, which is empty for the container itself; an empty name is the key's default value).
struct ContainedItem {
    std::wstring path;
    std::optional<std::wstring> value;
    bool container = false;  // A folder or a subkey

    auto operator<=>(const ContainedItem&) const = default;
};

// A created container's inventory is kept up to this many items; a larger one only by its count.
inline constexpr std::size_t kMaxContainedItems = 10000;

// How the item is grouped for review.
enum class Category : std::uint8_t {
    Files,      // Folders and files
    Shortcuts,  // .lnk files
    Registry,   // Keys and values
    Services,
    Tasks,
    Packages,
    Firewall,
};

// Which scanner found an item first (others may have added evidence).
enum class Scanner : std::uint8_t {
    InstallLocation,
    CommonDrive,
    SoftwareKey,
    UninstallerKey,
    PathReferences,
    Shortcuts,
    RelatedPackages,
    Services,
    Tasks,
    ComClasses,
    Firewall,
    MsiRegistration,  // Windows Installer's keys for the program's product code
    DeepSearch,       // Folders named after the program anywhere on the fixed volumes (ScanOptions::deep)
    Orphans,          // Folders no installed program owns (find_orphans)
    Trace,            // What the program's install trace recorded (ScanTarget::traced, traced_values)
};

struct Leftover {
    Location location;
    std::vector<Record> records;
    std::vector<std::wstring> owner_ids;  // Ids of the programs it was found for
    Scanner found_by = Scanner::InstallLocation;
    Category category = Category::Files;
    std::optional<std::size_t> inside;    // Index of the nearest leftover that contains this one
};

// The group an item falls in by its kind alone: .lnk files are shortcuts, value edits, startup
// approvals and other keys and values registry, a task's state tasks, a service's start type
// services, and so on.
Category category_of(const Location& location);

// Excluded in its program's install trace's review (Evidence::ExcludedInTraceReview).
bool excluded_in_trace_review(const Leftover& leftover);

// Per item of a scan's result: true for those never selected automatically, by a preset or by a
// minimum level, whatever their level: the items excluded in an install trace's review, and the
// items that contain one, since removing those would take it along: by Leftover::inside, or, for an
// excluded item no scanner offers, by Evidence::HoldsTraceReviewExclusion. The user can still select
// them.
std::vector<bool> held_back(std::span<const Leftover> leftovers);

std::string_view to_string(Category category);
std::string_view to_string(Scanner scanner);
std::string_view to_string(ValueEditKind kind);
bool parse(std::string_view text, ValueEditKind& kind);

}  // namespace avel::leftovers
