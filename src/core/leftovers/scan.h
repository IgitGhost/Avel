#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "core/inventory/program.h"
#include "core/leftovers/environment.h"
#include "core/leftovers/leftover.h"
#include "core/leftovers/scan_target.h"

namespace avel::leftovers {

struct ScanTiming {
    std::string_view stage;  // "folders", "keys", "shortcuts", ..., "matching", "protection"
    std::chrono::milliseconds elapsed{};
};

struct ScanResult {
    std::vector<Leftover> leftovers;  // Sorted by category and location; see Leftover::inside
    std::vector<ScanDiagnostic> diagnostics;
    std::size_t protected_count = 0;  // Matches dropped because they are protected (see protection.h)
    std::vector<ScanTiming> timings;
    std::chrono::milliseconds elapsed{};
};

// Finds what the target programs left behind, or would leave if their uninstallers removed
// nothing (for a program that is still installed).
// `installed` is what is installed now; the targets' own entries in it are ignored, the rest
// tells which folders other programs still use. Every match passes the hard floor and the
// protected-folder policy before it is reported.
// Takes a few hundred milliseconds: it reads folders, keys and shortcuts once for all targets.
ScanResult scan_leftovers(const ScanEnvironment& environment, std::span<const ScanTarget> targets,
                          std::span<const inventory::Program> installed);

}  // namespace avel::leftovers
