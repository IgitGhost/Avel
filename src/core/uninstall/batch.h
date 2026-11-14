#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/changes/change_set.h"
#include "core/changes/executor.h"
#include "core/inventory/program.h"
#include "core/uninstall/process_runner.h"
#include "core/uninstall/uninstall_plan.h"
#include "core/uninstall/uninstaller.h"

namespace avel::uninstall {

struct BatchItem {
    const inventory::Program* program = nullptr;  // Must outlive the batch
    UninstallPlan plan;
};

// What decides whether a packaged app's uninstall can be a soft removal (soft_removal_plan).
struct PackageFacts {
    std::wstring current_user_sid;
    std::vector<std::wstring> family_users;  // The SIDs that have some version of the family
    bool provisioned = false;                // Installed for every new user
};

// Who has a family and whether it is provisioned, read through platform/packages. Other users and
// the provisioning are read only with administrator rights; without, nobody else seems to have it.
PackageFacts live_package_facts(const std::wstring& family_name);

// The one-item ChangePlan (a PackageUserLocation, registered to not) that removes a packaged app
// for the current user only, when undo can register it again: the current user has its family, and
// the package stays on this PC because another user has the family or it is provisioned (ADR 0008).
// Nullopt otherwise, and the uninstall removes it as before: Windows then deletes the package at
// once, with no way back. The plan's description is the caller's to set.
std::optional<changes::ChangePlan> soft_removal_plan(const inventory::Program& program,
                                                     const UninstallPlan& plan, const PackageFacts& facts);

struct BatchOptions {
    bool dry_run = false;
    bool restore_point = true;
    std::wstring restore_point_description;  // Shown in System Restore
    // Runs a packaged app's soft removal (soft_removal_plan) as a ChangeSet in the front end's change
    // store, under its lock, and returns the result; an empty set id means nothing was changed. The
    // front end sets the plan's description. Unset (no change store: a dry run, or no administrator
    // rights), every packaged app is removed as before. See docs/uninstall.md ("Packaged apps").
    std::function<changes::ExecuteResult(changes::ChangePlan plan, const inventory::Program& program)>
        run_change_set;
    // Unset: live_package_facts. Tests replace it.
    std::function<PackageFacts(const std::wstring& family_name)> package_facts;
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
// uninstaller running at the time only. A packaged app that can be removed softly
// (soft_removal_plan) goes through options.run_change_set when it is set.
BatchResult run_batch(const std::vector<BatchItem>& items, const BatchOptions& options,
                      const BatchEvents& events, Cancellation& cancellation);

std::string_view to_string(RestorePointStatus status);

}  // namespace avel::uninstall
