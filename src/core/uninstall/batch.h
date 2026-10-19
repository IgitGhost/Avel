#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/inventory/program.h"
#include "core/uninstall/process_runner.h"
#include "core/uninstall/uninstall_plan.h"
#include "core/uninstall/uninstaller.h"

namespace avel::uninstall {

struct BatchItem {
    const inventory::Program* program = nullptr;  // Must outlive the batch
    UninstallPlan plan;
};

struct BatchOptions {
    bool dry_run = false;
    bool restore_point = true;
    std::wstring restore_point_description;  // Shown in System Restore
};

enum class RestorePointStatus : std::uint8_t {
    NotRequested,  // Disabled, dry run, or nothing to uninstall
    Created,
    Failed,
};

struct BatchEvents {
    // Called when the restore point could not be created; return false to cancel the batch.
    // Not set: the batch goes on without one.
    std::function<bool(std::uint32_t error)> continue_without_restore_point;
    std::function<void(RestorePointStatus status, std::uint32_t error)> restore_point_done;
    std::function<void(std::size_t index)> item_started;
    std::function<void(std::size_t index, const UninstallResult& result)> item_finished;
};

struct BatchResult {
    RestorePointStatus restore_point = RestorePointStatus::NotRequested;
    std::uint32_t restore_point_error = 0;
    std::vector<UninstallResult> results;  // One per item, in order
};

// Uninstalls the items one after another: uninstallers, Windows Installer ones in particular,
// can't safely run side by side. Before the first one, creates a System Restore point when
// asked to. Events are called on the calling thread. Once cancellation.stop_batch is set, the
// remaining items are reported as Cancelled without running; terminate_current applies to the
// uninstaller running at the time only.
BatchResult run_batch(const std::vector<BatchItem>& items, const BatchOptions& options,
                      const BatchEvents& events, Cancellation& cancellation);

std::string_view to_string(RestorePointStatus status);

}  // namespace avel::uninstall
