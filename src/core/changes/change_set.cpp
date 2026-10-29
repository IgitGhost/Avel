#include "core/changes/change_set.h"

#include <array>
#include <utility>

namespace avel::changes {

namespace {

template <typename Enum, std::size_t N>
bool parse_name(std::string_view text, Enum& value, const std::array<Enum, N>& all) {
    for (const auto candidate : all) {
        if (to_string(candidate) == text) {
            value = candidate;
            return true;
        }
    }
    return false;
}

constexpr std::array kItemStates = {ItemState::Pending, ItemState::BackedUp,       ItemState::Done,
                                    ItemState::Partial, ItemState::Skipped,        ItemState::Failed,
                                    ItemState::Restored, ItemState::RestoreSkipped, ItemState::RestoreFailed};
constexpr std::array kReasons = {
    Reason::None,          Reason::Protected,     Reason::PathChanged,     Reason::Missing,
    Reason::AccessDenied,  Reason::InUse,         Reason::RegistryLink,    Reason::UnsupportedVolume,
    Reason::HasDependents, Reason::NeedsPassword, Reason::MarkedForDeletion, Reason::OtherUser,
    Reason::NotUndoable,   Reason::ReportOnly,    Reason::Contained,       Reason::Interrupted,
    Reason::Exists,        Reason::BackupMissing, Reason::Error,           Reason::Changed,
    Reason::NotEmpty};
constexpr std::array kSetStates = {SetState::Running, SetState::Complete, SetState::Restored, SetState::PartlyRestored};

}  // namespace

PlannedItem make_planned_item(const leftovers::Leftover& leftover, std::vector<std::wstring> programs) {
    return {leftover.location, leftover.category, std::move(programs), leftovers::level(leftover.records),
            leftovers::score(leftover.records)};
}

ChangePlan make_plan(std::span<const leftovers::Leftover> leftovers, leftovers::Confidence minimum,
                     const std::function<std::wstring(const std::wstring& program_id)>& program_name,
                     std::wstring description) {
    ChangePlan plan;
    plan.description = std::move(description);
    for (const auto& leftover : leftovers) {
        if (leftovers::level(leftover.records) < minimum || !is_removable(leftover.location)) {
            continue;
        }
        std::vector<std::wstring> programs;
        for (const auto& id : leftover.owner_ids) {
            programs.push_back(program_name ? program_name(id) : id);
        }
        plan.items.push_back(make_planned_item(leftover, std::move(programs)));
    }
    return plan;
}

std::string_view to_string(ItemState state) {
    switch (state) {
        case ItemState::Pending:
            return "pending";
        case ItemState::BackedUp:
            return "backed-up";
        case ItemState::Done:
            return "done";
        case ItemState::Partial:
            return "partial";
        case ItemState::Skipped:
            return "skipped";
        case ItemState::Failed:
            return "failed";
        case ItemState::Restored:
            return "restored";
        case ItemState::RestoreSkipped:
            return "restore-skipped";
        case ItemState::RestoreFailed:
            return "restore-failed";
    }
    return "unknown";
}

std::string_view to_string(Reason reason) {
    switch (reason) {
        case Reason::None:
            return "none";
        case Reason::Protected:
            return "protected";
        case Reason::PathChanged:
            return "path-changed";
        case Reason::Missing:
            return "missing";
        case Reason::AccessDenied:
            return "access-denied";
        case Reason::InUse:
            return "in-use";
        case Reason::RegistryLink:
            return "registry-link";
        case Reason::UnsupportedVolume:
            return "unsupported-volume";
        case Reason::HasDependents:
            return "has-dependents";
        case Reason::NeedsPassword:
            return "needs-password";
        case Reason::MarkedForDeletion:
            return "marked-for-deletion";
        case Reason::OtherUser:
            return "other-user";
        case Reason::NotUndoable:
            return "not-undoable";
        case Reason::ReportOnly:
            return "report-only";
        case Reason::Contained:
            return "contained";
        case Reason::Interrupted:
            return "interrupted";
        case Reason::Exists:
            return "exists";
        case Reason::BackupMissing:
            return "backup-missing";
        case Reason::Error:
            return "error";
        case Reason::Changed:
            return "changed";
        case Reason::NotEmpty:
            return "not-empty";
    }
    return "unknown";
}

std::string_view to_string(SetState state) {
    switch (state) {
        case SetState::Running:
            return "running";
        case SetState::Complete:
            return "complete";
        case SetState::Restored:
            return "restored";
        case SetState::PartlyRestored:
            return "partly-restored";
    }
    return "unknown";
}

bool parse(std::string_view text, ItemState& state) {
    return parse_name(text, state, kItemStates);
}

bool parse(std::string_view text, Reason& reason) {
    return parse_name(text, reason, kReasons);
}

bool parse(std::string_view text, SetState& state) {
    return parse_name(text, state, kSetStates);
}

bool is_removable(const leftovers::Location& location) {
    if (const auto* service = std::get_if<leftovers::ServiceLocation>(&location)) {
        return !service->driver;  // Removing a driver can leave Windows unable to start
    }
    return !std::holds_alternative<leftovers::FirewallRuleLocation>(location);
}

bool is_undoable(const leftovers::Location& location) {
    return is_removable(location) && !std::holds_alternative<leftovers::PackageLocation>(location);
}

}  // namespace avel::changes
