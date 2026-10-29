#include "core/leftovers/post_uninstall.h"

#include "core/inventory/inventory.h"

namespace avel::leftovers {

std::vector<ScanTarget> capture_targets(std::span<const inventory::Program* const> programs,
                                        TraceSource* traces) {
    std::vector<ScanTarget> targets;
    targets.reserve(programs.size());
    for (const auto* program : programs) {
        targets.push_back(make_scan_target(*program));
        if (traces) {
            traces->add_trace(*program, targets.back());
        }
    }
    return targets;
}

ScanResult scan_after_uninstall(std::span<const ScanTarget> targets,
                                std::span<const uninstall::UninstallResult> results,
                                const ScanOptions& options) {
    std::vector<ScanTarget> removed;
    for (std::size_t i = 0; i < targets.size() && i < results.size(); ++i) {
        const auto outcome = results[i].outcome;
        if (outcome == uninstall::Outcome::Removed || outcome == uninstall::Outcome::RebootRequired) {
            removed.push_back(targets[i]);
        }
    }
    if (removed.empty()) {
        return {};
    }
    // What other programs still use is judged by what is installed now, after the batch.
    const auto inventory = inventory::scan_inventory();
    const LiveEnvironment environment;
    return scan_leftovers(environment.environment(), removed, inventory.programs, options);
}

}  // namespace avel::leftovers
