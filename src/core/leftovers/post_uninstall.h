#pragma once

#include <span>
#include <vector>

#include "core/inventory/program.h"
#include "core/leftovers/scan.h"
#include "core/uninstall/uninstaller.h"

namespace avel::leftovers {

// The leftover scan that follows an uninstall batch, shared by the CLI and the GUI:
//   1. before the batch runs, capture_targets() records what the scan needs about each program
//      (its Uninstall key and paths disappear with it);
//   2. after the batch, scan_after_uninstall() scans the programs whose uninstall succeeded
//      (removed, or removed once Windows restarts) against a fresh inventory.
std::vector<ScanTarget> capture_targets(std::span<const inventory::Program* const> programs);

// `results[i]` is the outcome for `targets[i]`. Scans nothing (and returns an empty result) when
// no uninstall succeeded.
ScanResult scan_after_uninstall(std::span<const ScanTarget> targets,
                                std::span<const uninstall::UninstallResult> results);

}  // namespace avel::leftovers
