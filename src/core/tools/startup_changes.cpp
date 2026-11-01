#include "core/tools/startup_changes.h"

#include <algorithm>
#include <optional>
#include <variant>

#include "core/leftovers/startup_approved.h"
#include "core/platform/text.h"

namespace avel::tools {

namespace {

using leftovers::Location;

const StartupEntry* find_entry(std::span<const StartupEntry> entries, const std::wstring& id) {
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const StartupEntry& entry) {
        return platform::iequals(entry.id, id);
    });
    return found != entries.end() ? &*found : nullptr;
}

// The start a disable asks for.
std::uint32_t disabled_start(ServiceDisable disable) {
    return disable == ServiceDisable::Disabled ? SERVICE_DISABLED : SERVICE_DEMAND_START;
}

bool already_in_state(const StartupEntry& entry, StartupAction action, ServiceDisable disable) {
    if (action == StartupAction::Enable) {
        return entry.state == StartupState::Enabled;
    }
    if (entry.kind == StartupKind::Service && entry.state == StartupState::Disabled) {
        // Manual is disabled enough when Manual is asked for, not when Disabled is.
        return disable == ServiceDisable::Manual || entry.service_start == SERVICE_DISABLED;
    }
    return entry.state == StartupState::Disabled;
}

// The state change that disables or enables `entry`; nullopt when it has no state of its own.
std::optional<Location> state_change(const StartupEntry& entry, bool disable, ServiceDisable service_disable,
                                     FILETIME now) {
    switch (entry.kind) {
        case StartupKind::Run:
        case StartupKind::StartupFolder: {
            if (!entry.approval) {
                return std::nullopt;  // A user's 32-bit Run value: delete only
            }
            const StartupApproval& approval = *entry.approval;
            return leftovers::StartupApprovedLocation{
                approval.key, approval.value_name, approval.value,
                disable ? leftovers::disabled_approval(approval.value_name, now)
                        : leftovers::enabled_approval(approval.value_name)};
        }
        case StartupKind::Task:
            return leftovers::TaskStateLocation{entry.name, entry.task_enabled, !disable};
        case StartupKind::Service: {
            const leftovers::ServiceStart before{entry.service_start, entry.service_delayed};
            // The delayed flag stays: only the start type changes, so undo puts back exactly that.
            const leftovers::ServiceStart after{
                disable ? disabled_start(service_disable) : static_cast<std::uint32_t>(SERVICE_AUTO_START),
                entry.service_delayed};
            return leftovers::ServiceStartLocation{entry.name, before, after};
        }
        case StartupKind::RunOnce:
        case StartupKind::RunOnceEx:
            break;
    }
    return std::nullopt;
}

changes::PlannedItem planned(Location location, const StartupEntry& entry,
                             const std::function<std::wstring(const std::wstring&)>& program_name) {
    changes::PlannedItem item;
    item.category = leftovers::category_of(location);
    item.location = std::move(location);
    if (!entry.program_id.empty()) {
        item.programs.push_back(program_name ? program_name(entry.program_id) : entry.program_id);
    }
    // No evidence was scored: the user chose the item (the executor doesn't read these).
    item.confidence = leftovers::Confidence::Unknown;
    item.score = 0;
    return item;
}

}  // namespace

bool is_error(StartupPlanIssueCode code) { return code != StartupPlanIssueCode::AlreadyInState; }

StartupPlan plan_startup(std::span<const StartupEntry> entries, StartupAction action,
                         std::span<const std::wstring> ids, ServiceDisable service_disable, FILETIME now,
                         const std::function<std::wstring(const std::wstring& program_id)>& program_name) {
    StartupPlan result;
    std::vector<const StartupEntry*> seen;
    for (const auto& id : ids) {
        const StartupEntry* entry = find_entry(entries, id);
        if (!entry) {
            result.issues.push_back({id, StartupPlanIssueCode::UnknownId});
            continue;
        }
        if (std::find(seen.begin(), seen.end(), entry) != seen.end()) {
            continue;  // Asked twice
        }
        seen.push_back(entry);
        if (entry->protection != StartupProtection::None) {
            result.issues.push_back({id, StartupPlanIssueCode::Protected});
            continue;
        }
        if (action == StartupAction::Delete) {
            for (const auto& location : entry->delete_locations) {
                result.plan.items.push_back(planned(location, *entry, program_name));
            }
            result.planned.push_back(entry->id);
            continue;
        }
        auto change = state_change(*entry, action == StartupAction::Disable, service_disable, now);
        if (!change) {
            result.issues.push_back({id, StartupPlanIssueCode::NotSupported});
            continue;
        }
        if (already_in_state(*entry, action, service_disable)) {
            result.issues.push_back({id, StartupPlanIssueCode::AlreadyInState});
            continue;
        }
        result.plan.items.push_back(planned(std::move(*change), *entry, program_name));
        result.planned.push_back(entry->id);
    }
    return result;
}

std::vector<std::wstring> services_disabled_by_avel(changes::ChangeStore& store) {
    std::vector<std::wstring> decided;  // Lowercase names a newer set already settled
    std::vector<std::wstring> disabled;
    for (const auto& summary : store.list()) {  // Newest first
        const auto set = store.open_set(summary.id);
        if (!set) {
            continue;  // A live run holds it, or it can't be read
        }
        const auto& items = set->plan().items;
        const auto& statuses = set->statuses();
        for (std::size_t i = 0; i < items.size() && i < statuses.size(); ++i) {
            const auto* change = std::get_if<leftovers::ServiceStartLocation>(&items[i].location);
            const changes::ItemState state = statuses[i].state;
            // In effect: done, or done and its undo failed. Restored and skipped items aren't.
            if (!change ||
                (state != changes::ItemState::Done && state != changes::ItemState::RestoreFailed)) {
                continue;
            }
            std::wstring key = platform::to_lower(change->name);
            if (std::find(decided.begin(), decided.end(), key) != decided.end()) {
                continue;
            }
            decided.push_back(std::move(key));
            if (change->after.start_type != SERVICE_AUTO_START) {
                disabled.push_back(change->name);
            }
        }
    }
    return disabled;
}

std::string_view to_string(StartupAction action) {
    switch (action) {
        case StartupAction::Disable:
            return "disable";
        case StartupAction::Enable:
            return "enable";
        case StartupAction::Delete:
            return "delete";
    }
    return "unknown";
}

std::string_view to_string(ServiceDisable disable) {
    switch (disable) {
        case ServiceDisable::Manual:
            return "manual";
        case ServiceDisable::Disabled:
            return "disabled";
    }
    return "unknown";
}

std::string_view to_string(StartupPlanIssueCode code) {
    switch (code) {
        case StartupPlanIssueCode::UnknownId:
            return "unknown-id";
        case StartupPlanIssueCode::Protected:
            return "protected";
        case StartupPlanIssueCode::NotSupported:
            return "not-supported";
        case StartupPlanIssueCode::AlreadyInState:
            return "already-in-state";
    }
    return "unknown";
}

}  // namespace avel::tools
