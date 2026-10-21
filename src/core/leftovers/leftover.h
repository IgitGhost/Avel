#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/leftovers/confidence.h"
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
    std::wstring path;  // e.g. \Contoso\Updater
};

struct PackageLocation {
    std::wstring full_name;
    std::wstring user_sid;  // The user the package is registered for
};

struct FirewallRuleLocation {
    std::wstring rule_id;  // The value name under FirewallPolicy\FirewallRules
};

using Location = std::variant<FileLocation, RegistryKeyLocation, RegistryValueLocation, ServiceLocation, TaskLocation,
                              PackageLocation, FirewallRuleLocation>;

// A case-insensitive key identifying the item, used to merge what several scanners found:
// "file:c:\x", "key:hklm\software\x", "value:hku\s-1-5-21-…\software\x\name", ...
std::wstring identity(const Location& location);

// `outer` contains `inner`: a folder and something below it, or a key and a subkey or value of it.
bool contains(const Location& outer, const Location& inner);

// The user whose profile or hive holds the item; empty for machine-wide items.
std::wstring owner_sid(const Location& location);

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
};

struct Leftover {
    Location location;
    std::vector<Record> records;
    std::vector<std::wstring> owner_ids;  // Ids of the programs it was found for
    Scanner found_by = Scanner::InstallLocation;
    Category category = Category::Files;
    std::optional<std::size_t> inside;    // Index of the nearest leftover that contains this one
};

std::string_view to_string(Category category);
std::string_view to_string(Scanner scanner);

}  // namespace avel::leftovers
