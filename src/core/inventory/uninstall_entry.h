#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/inventory/program.h"
#include "core/platform/registry.h"

namespace avel::inventory {

// What the parser needs to know about where an Uninstall key came from.
struct UninstallKeyContext {
    RegistryLocation location;
    std::wstring id_prefix;  // e.g. "reg:HKLM64:"; the key name is appended
    Scope scope = Scope::Machine;
    Architecture architecture = Architecture::Unknown;
    std::wstring user_sid;
    std::wstring user_name;
    // Expands %VARIABLES% in REG_EXPAND_SZ values for the key's owner.
    std::function<std::wstring(std::wstring_view)> expand;
    // Resolves resource references such as "@C:\Windows\System32\mstsc.exe,-4000" to text;
    // returns an empty string on failure. Optional.
    std::function<std::wstring(std::wstring_view)> resolve_indirect;
};

// Turns the values of one Uninstall key into a Program. Returns nullopt for keys that carry no
// useful information (no name, publisher or uninstall command). Pure: no system access.
//
// Field rules follow Bulk Crap Uninstaller's RegistryFactory (Apache License 2.0), including its
// handling of "UninstallString_hidden"-style value names and of YYYYDDMM install dates.
std::optional<Program> parse_uninstall_entry(const UninstallKeyContext& context,
                                             const std::vector<platform::RegistryValue>& values);

// Parses an InstallDate value: "YYYYMMDD" (the documented format), "YYYYDDMM" (a common mistake),
// "YYYY-MM-DD" or asctime()-style "Sat Oct 17 22:38:01 2026".
std::optional<std::chrono::sys_days> parse_install_date(std::wstring_view text);

// Finds the first "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" in `text`; returns it upper-cased.
std::optional<std::wstring> extract_guid(std::wstring_view text);

// Guesses the installer technology from an uninstall command alone.
InstallerType installer_type_from_command(std::wstring_view uninstall_command);

}  // namespace avel::inventory
