#pragma once

#include <vector>

#include "core/inventory/diagnostic.h"
#include "core/inventory/program.h"

namespace avel::inventory {

struct AppxScanResult {
    std::vector<Program> programs;  // source = Appx
    std::vector<Diagnostic> diagnostics;
};

// Lists the current user's packaged apps (Microsoft Store and MSIX), main packages only.
// Frameworks, resource packages and system-signed packages are kept but flagged.
// Safe to call from any thread: the WinRT work runs on its own multithreaded-apartment thread.
AppxScanResult scan_appx_packages();

}  // namespace avel::inventory
