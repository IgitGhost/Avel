#include "core/uninstall/batch.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <utility>

#include "core/leftovers/leftover.h"
#include "core/platform/packages.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "core/uninstall/registration.h"
#include "core/uninstall/restore_point.h"

namespace avel::uninstall {

namespace {

// Whether an uninstaller ran and may have changed the system (a terminated one may have).
bool ran(const UninstallResult& result) {
    switch (result.outcome) {
        case Outcome::NotPossible:
        case Outcome::Simulated:
            return false;
        case Outcome::Cancelled:
            return result.exit_code.has_value();
        default:
            return true;
    }
}

std::wstring family_of(const inventory::Program& program, const UninstallPlan& plan) {
    return !program.package_family_name.empty() ? program.package_family_name
                                                : leftovers::package_family_name(plan.package_full_name);
}

// A packaged app removed for the current user through a ChangeSet, when undo can bring it back
// (soft_removal_plan; ADR 0008). Nullopt when it can't, and the uninstall goes on as before.
std::optional<UninstallResult> remove_softly(const BatchItem& item, const BatchOptions& options) {
    const inventory::Program& program = *item.program;
    if (options.dry_run || !options.run_change_set || !item.plan.possible() ||
        item.plan.method != Method::Package) {
        return std::nullopt;
    }
    const std::wstring family = family_of(program, item.plan);
    if (family.empty()) {
        return std::nullopt;
    }
    auto plan =
        soft_removal_plan(program, item.plan,
                          options.package_facts ? options.package_facts(family) : live_package_facts(family));
    if (!plan) {
        return std::nullopt;
    }

    const auto started = std::chrono::steady_clock::now();
    const changes::ExecuteResult executed = options.run_change_set(std::move(*plan), program);
    UninstallResult result;
    const changes::ItemStatus status =
        executed.items.empty() ? changes::ItemStatus{} : executed.items.front().status;
    if (executed.set_id.empty()) {
        // The set couldn't be written, or another Avel process held the store: nothing was changed.
        result.outcome = Outcome::Failed;
        result.error = ERROR_BUSY;
    } else if (status.state == changes::ItemState::Done) {
        result.outcome = Outcome::Removed;
        result.change_set = executed.set_id;
    } else if (is_package_installed(item.plan.package_full_name)) {
        result.outcome =
            status.state == changes::ItemState::Failed ? Outcome::Failed : Outcome::StillInstalled;
        result.error = status.code;
    } else {
        result.outcome = Outcome::Removed;  // Gone meanwhile, by other means: nothing to undo
    }
    result.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    result.install_location_remains = install_location_exists(program);
    return result;
}

}  // namespace

PackageFacts live_package_facts(const std::wstring& family_name) {
    PackageFacts facts;
    facts.current_user_sid = platform::current_user_sid();
    facts.family_users = platform::family_users(family_name);
    facts.provisioned = platform::provisioned_package(family_name).has_value();
    return facts;
}

std::optional<changes::ChangePlan> soft_removal_plan(const inventory::Program& program,
                                                     const UninstallPlan& plan, const PackageFacts& facts) {
    if (!plan.possible() || plan.method != Method::Package || plan.package_full_name.empty() ||
        facts.current_user_sid.empty()) {
        return std::nullopt;
    }
    const std::wstring family = family_of(program, plan);
    const auto is_current = [&](const std::wstring& sid) {
        return platform::iequals(sid, facts.current_user_sid);
    };
    const bool current_has_it = std::any_of(facts.family_users.begin(), facts.family_users.end(), is_current);
    const bool someone_else = std::any_of(facts.family_users.begin(), facts.family_users.end(),
                                          [&](const std::wstring& sid) { return !is_current(sid); });
    // Without another user or the provisioning, Windows deletes the package with the current user's
    // registration, and there would be nothing left to register again.
    if (family.empty() || !current_has_it || !(someone_else || facts.provisioned)) {
        return std::nullopt;
    }
    changes::PlannedItem item;
    item.location =
        leftovers::PackageUserLocation{family, plan.package_full_name, facts.current_user_sid, true, false};
    item.category = leftovers::category_of(item.location);
    item.programs.push_back(program.display_name);
    // The user chose to uninstall it: no evidence was scored (the executor doesn't read these).
    item.confidence = leftovers::Confidence::Unknown;
    item.score = 0;
    changes::ChangePlan result;
    result.items.push_back(std::move(item));
    return result;
}

BatchResult run_batch(const std::vector<BatchItem>& items, const BatchOptions& options,
                      const BatchEvents& events, Cancellation& cancellation) {
    BatchResult batch;
    batch.results.resize(items.size());

    const bool anything_to_do =
        std::any_of(items.begin(), items.end(), [](const BatchItem& item) { return item.plan.possible(); });
    RestorePointResult restore_point;
    if (options.restore_point && !options.dry_run && anything_to_do) {
        restore_point = begin_restore_point(options.restore_point_description);
        batch.restore_point =
            restore_point.created() ? RestorePointStatus::Created : RestorePointStatus::Failed;
        batch.restore_point_error = restore_point.error;
        if (events.restore_point_done) {
            events.restore_point_done(batch.restore_point, batch.restore_point_error);
        }
    }
    bool cancelled = batch.restore_point == RestorePointStatus::Failed &&
                     events.continue_without_restore_point &&
                     !events.continue_without_restore_point(batch.restore_point_error);

    bool changes_made = false;
    for (std::size_t index = 0; index < items.size(); ++index) {
        UninstallResult& result = batch.results[index];
        cancelled = cancelled || cancellation.stop_batch;
        if (cancelled) {
            result.outcome = Outcome::Cancelled;
        } else {
            cancellation.terminate_current = false;  // A termination applies to one uninstaller only
            if (events.item_started) {
                events.item_started(index);
            }
            auto soft = remove_softly(items[index], options);
            result = soft
                         ? std::move(*soft)
                         : uninstall(*items[index].program, items[index].plan, cancellation, options.dry_run);
            changes_made = changes_made || ran(result);
        }
        if (events.item_finished) {
            events.item_finished(index, result);
        }
    }

    if (batch.restore_point == RestorePointStatus::Created) {
        end_restore_point(restore_point.sequence, changes_made);
    }
    return batch;
}

std::string_view to_string(RestorePointStatus status) {
    switch (status) {
        case RestorePointStatus::NotRequested:
            return "not-requested";
        case RestorePointStatus::Created:
            return "created";
        case RestorePointStatus::Failed:
            return "failed";
    }
    return "unknown";
}

}  // namespace avel::uninstall
