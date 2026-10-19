#include "core/uninstall/batch.h"

#include <algorithm>

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

}  // namespace

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
            result = uninstall(*items[index].program, items[index].plan, cancellation, options.dry_run);
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
