#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

#include "core/inventory/diagnostic.h"
#include "core/inventory/program.h"

namespace avel::inventory {

struct ScanOptions {
    bool registry = true;
    bool msi = true;
    bool appx = true;
};

struct SourceTiming {
    Source source = Source::Registry;
    std::chrono::milliseconds elapsed{};
    std::size_t count = 0;
};

struct Inventory {
    std::vector<Program> programs;  // Sorted by display name
    std::vector<Diagnostic> diagnostics;
    std::vector<SourceTiming> timings;
    std::chrono::milliseconds elapsed{};
};

// Scans all sources in parallel and merges the results. Safe to call from any thread (each
// source runs on its own worker thread with the COM apartment it needs).
Inventory scan_inventory(const ScanOptions& options = {});

// Folds Windows Installer products into the registry entries they belong to: first by product
// code, then by Bulk Crap Uninstaller's relatedness score. Products that match nothing are
// appended as Source::Msi entries. Only descriptive fields are copied, so a matched entry never
// inherits another product's uninstall command.
// When `msi_scan_complete`, registry entries with WindowsInstaller=1 (machine-wide or owned by
// `current_user_sid`) whose product Windows Installer doesn't report get msi_product_unregistered.
// Exposed for tests.
void merge_msi_products(std::vector<Program>& programs, std::vector<Program> msi_products,
                        std::wstring_view current_user_sid, bool msi_scan_complete);

}  // namespace avel::inventory
