#pragma once

#include <optional>
#include <string>

#include "core/inventory/program.h"
#include "core/leftovers/product_name.h"
#include "core/platform/registry_address.h"

namespace avel::leftovers {

// What the leftover scan needs to know about a program. It is captured before the uninstaller
// runs: afterwards the Uninstall key is gone, and paths that no longer exist can't be resolved
// (8.3 names, for instance).
struct ScanTarget {
    std::wstring program_id;
    std::wstring display_name;
    ProductNames names;
    inventory::Scope scope = inventory::Scope::Machine;
    std::wstring user_sid;  // The owner of a per-user program

    // Normalized long path. When the entry records none, the uninstaller's or the icon's folder,
    // if it is named after the program; otherwise empty.
    std::wstring install_location;
    std::wstring uninstaller;         // The file the uninstall command runs, unless it is a Windows tool
    std::wstring uninstaller_folder;  // Its folder

    std::optional<platform::RegistryAddress> uninstall_key;
    std::wstring msi_product_code;  // "{GUID}" of a Windows Installer product, for its registration
    bool store_app = false;
    std::wstring package_full_name;
};

ScanTarget make_scan_target(const inventory::Program& program);

}  // namespace avel::leftovers
