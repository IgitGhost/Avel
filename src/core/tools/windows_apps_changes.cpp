#include "core/tools/windows_apps_changes.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "core/platform/text.h"

namespace avel::tools {

namespace {

using Code = WindowsAppsPlanIssueCode;
using leftovers::Location;

const WindowsApp* find_app(const WindowsAppsList& list, const std::wstring& id) {
    const auto found = std::find_if(list.apps.begin(), list.apps.end(), [&](const WindowsApp& app) {
        return platform::iequals(app.id, id) || platform::iequals(app.family_name, id);
    });
    return found != list.apps.end() ? &*found : nullptr;
}

changes::PlannedItem planned(Location location, const WindowsApp& app) {
    changes::PlannedItem item;
    item.category = leftovers::category_of(location);
    item.location = std::move(location);
    item.programs.push_back(app.display_name);
    // No evidence was scored: the user chose the item (the executor doesn't read these).
    item.confidence = leftovers::Confidence::Unknown;
    item.score = 0;
    return item;
}

leftovers::PackageProvisioningLocation deprovisioning(const WindowsApp& app) {
    return {app.family_name, app.provisioned_full_name, true, false};
}

// The plan for the apps with these ids: `check` gives the issue that keeps an app from this change,
// if any, after the issues every change shares; `plan_app` adds the items of an app without one.
template <typename Check, typename Plan>
WindowsAppsPlan plan_apps(const WindowsAppsList& list, std::span<const std::wstring> ids, Check&& check,
                          Plan&& plan_app) {
    WindowsAppsPlan result;
    std::vector<const WindowsApp*> seen;
    for (const auto& id : ids) {
        const WindowsApp* app = find_app(list, id);
        if (!app) {
            result.issues.push_back({id, Code::UnknownId});
            continue;
        }
        if (std::find(seen.begin(), seen.end(), app) != seen.end()) {
            continue;  // Asked twice
        }
        seen.push_back(app);
        std::optional<Code> issue;
        if (app->protection != AppProtection::None) {
            issue = Code::Protected;
        } else if (!list.all_users) {
            issue = Code::IncompleteListing;
        } else {
            issue = check(*app);
        }
        if (issue) {
            result.issues.push_back({id, *issue});
            continue;
        }
        plan_app(*app, result.plan.items);
        result.planned.push_back(app->id);
    }
    return result;
}

}  // namespace

WindowsAppsPlan plan_remove_for_me(const WindowsAppsList& list, std::span<const std::wstring> ids) {
    return plan_apps(
        list, ids,
        [](const WindowsApp& app) -> std::optional<Code> {
            if (app.current_user_full_name.empty()) {
                return Code::NotInstalledForCurrentUser;
            }
            if (!app.soft_undoable) {
                return Code::WouldDeletePackage;
            }
            return std::nullopt;
        },
        [](const WindowsApp& app, std::vector<changes::PlannedItem>& items) {
            const auto current = std::find_if(app.users.begin(), app.users.end(),
                                              [](const WindowsAppUser& user) { return user.current; });
            const std::wstring sid = current != app.users.end() ? current->sid : std::wstring();
            items.push_back(planned(
                leftovers::PackageUserLocation{app.family_name, app.current_user_full_name, sid, true, false},
                app));
        });
}

WindowsAppsPlan plan_deprovision(const WindowsAppsList& list, std::span<const std::wstring> ids) {
    return plan_apps(
        list, ids,
        [](const WindowsApp& app) -> std::optional<Code> {
            if (!app.provisioned) {
                return Code::NotProvisioned;
            }
            // Deprovisioning a family no user has may let Windows delete the staged package, which
            // would leave nothing to provision again (not verified; ADR 0008): only deep removal then.
            if (app.users.empty()) {
                return Code::WouldDeletePackage;
            }
            return std::nullopt;
        },
        [](const WindowsApp& app, std::vector<changes::PlannedItem>& items) {
            items.push_back(planned(deprovisioning(app), app));
        });
}

WindowsAppsPlan plan_remove_for_everyone(const WindowsAppsList& list, std::span<const std::wstring> ids,
                                         bool irreversible_confirmed) {
    return plan_apps(
        list, ids,
        [&](const WindowsApp&) -> std::optional<Code> {
            return irreversible_confirmed ? std::nullopt : std::optional(Code::IrreversibleNeedsConfirmation);
        },
        [](const WindowsApp& app, std::vector<changes::PlannedItem>& items) {
            // One item per version. The executor deprovisions the family first when it is
            // provisioned (ADR 0008), so there is no separate provisioning item (docs/changes.md).
            for (const auto& package : app.packages) {
                items.push_back(planned(leftovers::PackageLocation{package.full_name, L"", true}, app));
            }
        });
}

std::string_view to_string(WindowsAppsPlanIssueCode code) {
    switch (code) {
        case Code::UnknownId:
            return "unknown-id";
        case Code::Protected:
            return "protected";
        case Code::IncompleteListing:
            return "incomplete-listing";
        case Code::NotInstalledForCurrentUser:
            return "not-installed-for-current-user";
        case Code::WouldDeletePackage:
            return "would-delete-package";
        case Code::NotProvisioned:
            return "not-provisioned";
        case Code::IrreversibleNeedsConfirmation:
            return "irreversible-needs-confirmation";
    }
    return "unknown";
}

}  // namespace avel::tools
