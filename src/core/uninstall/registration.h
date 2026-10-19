#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/inventory/program.h"

namespace avel::uninstall {

// Where an Uninstall key lives, ready for RegOpenKeyExW.
struct UninstallKeyAddress {
    HKEY root = nullptr;
    std::wstring path;  // Below `root`
    REGSAM view = 0;    // KEY_WOW64_64KEY or KEY_WOW64_32KEY for HKLM
};

// Locates an inventory registry location; per-user views need the owner's SID. Pure.
std::optional<UninstallKeyAddress> uninstall_key_address(const inventory::RegistryLocation& location,
                                                         std::wstring_view user_sid);

// Whether the program is still registered: its Uninstall key, its Windows Installer product
// (installed for the machine or the current user) or its package still exists.
// When unsure (e.g. access denied), it says yes.
bool is_still_registered(const inventory::Program& program);

// Whether the program's install folder still exists (leftovers are dealt with in Phase 3).
bool install_location_exists(const inventory::Program& program);

// Removes a packaged app for the current user. Returns S_OK or the package manager's HRESULT.
std::int32_t remove_package(const std::wstring& package_full_name);

// Whether a package is installed for the current user.
bool is_package_installed(const std::wstring& package_full_name);

}  // namespace avel::uninstall
