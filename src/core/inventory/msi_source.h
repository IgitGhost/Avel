#pragma once

#include <vector>

#include "core/inventory/diagnostic.h"
#include "core/inventory/program.h"

namespace avel::inventory {

struct MsiScanResult {
    std::vector<Program> programs;  // source = Msi, msi_product_code always set
    std::vector<Diagnostic> diagnostics;
};

// Lists every product registered with Windows Installer, in all contexts (per-machine,
// per-user managed and unmanaged) and for all users when elevated. This catches products hidden
// from Settings and products whose Uninstall key is gone.
MsiScanResult scan_msi_products();

}  // namespace avel::inventory
