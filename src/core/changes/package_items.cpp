#include "core/changes/package_items.h"

#include <windows.h>

#include <algorithm>

#include "core/platform/text.h"
#include "core/platform/user_profiles.h"

namespace avel::changes {

std::wstring PackageSystem::current_user_sid() { return platform::current_user_sid(); }

std::vector<std::wstring> PackageSystem::family_users(const std::wstring& family_name) {
    return platform::family_users(family_name);
}

std::optional<std::wstring> PackageSystem::provisioned_package(const std::wstring& family_name) {
    return platform::provisioned_package(family_name);
}

bool PackageSystem::family_staged(const std::wstring& family_name) {
    return platform::family_staged(family_name);
}

platform::PackageOperation PackageSystem::remove_for_current_user(const std::wstring& full_name) {
    return platform::remove_for_current_user(full_name);
}

platform::PackageOperation PackageSystem::register_for_current_user(const std::wstring& family_name) {
    return platform::register_for_current_user(family_name);
}

platform::PackageOperation PackageSystem::deprovision(const std::wstring& family_name) {
    return platform::deprovision(family_name);
}

platform::PackageOperation PackageSystem::provision(const std::wstring& family_name) {
    return platform::provision(family_name);
}

platform::PackageOperation PackageSystem::remove_for_all_users(const std::wstring& full_name) {
    return platform::remove_for_all_users(full_name);
}

bool PackageSystem::registered(const std::wstring& family_name, const std::wstring& sid) {
    const auto users = family_users(family_name);
    return std::any_of(users.begin(), users.end(),
                       [&](const std::wstring& user) { return platform::iequals(user, sid); });
}

ItemStatus package_status(const platform::PackageOperation& operation) {
    if (operation.confirmed) {
        return {ItemState::Done, Reason::None, 0};
    }
    const std::int32_t code = operation.hresult < 0 ? operation.hresult : operation.extended;
    return {ItemState::Failed, code == E_ACCESSDENIED ? Reason::AccessDenied : Reason::Error,
            static_cast<std::uint32_t>(code)};
}

}  // namespace avel::changes
