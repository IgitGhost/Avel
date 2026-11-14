#include "core/platform/packages.h"

#include <windows.h>
#include <appmodel.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>

#include <algorithm>
#include <array>
#include <functional>
#include <utility>

#include "core/platform/elevation.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "core/platform/winrt_apartment.h"

namespace avel::platform {

namespace {

namespace wam = winrt::Windows::ApplicationModel;
namespace wf = winrt::Windows::Foundation;
namespace wmd = winrt::Windows::Management::Deployment;

// The listing's types: frameworks and resource packages too, flagged, so the callers can tell
// them from apps; bundles aren't listed (their main packages are).
constexpr auto kListedTypes = wmd::PackageTypes::Main | wmd::PackageTypes::Optional |
                              wmd::PackageTypes::Framework | wmd::PackageTypes::Resource;

// Package properties resolve resources and file paths and can throw for broken or staged
// packages; a bad property must not lose the whole package.
template <typename Getter>
auto try_get(Getter&& getter) -> decltype(getter()) {
    try {
        return getter();
    } catch (...) {
        return {};
    }
}

PackageSignatureKind to_signature_kind(wam::PackageSignatureKind kind) {
    switch (kind) {
        case wam::PackageSignatureKind::Developer:
            return PackageSignatureKind::Developer;
        case wam::PackageSignatureKind::Enterprise:
            return PackageSignatureKind::Enterprise;
        case wam::PackageSignatureKind::Store:
            return PackageSignatureKind::Store;
        case wam::PackageSignatureKind::System:
            return PackageSignatureKind::System;
        default:
            return PackageSignatureKind::None;
    }
}

std::wstring version_text(const wam::PackageVersion& version) {
    return std::to_wstring(version.Major) + L'.' + std::to_wstring(version.Minor) + L'.' +
           std::to_wstring(version.Build) + L'.' + std::to_wstring(version.Revision);
}

// The users that have the package registered. FindUsers also names the users for whom it is only
// staged (S-1-5-18 for a package staged without users), and those don't have it.
std::vector<std::wstring> registered_users(const wmd::PackageManager& manager,
                                           const winrt::hstring& full_name) {
    std::vector<std::wstring> sids;
    for (const auto& user : manager.FindUsers(full_name)) {
        const auto state = user.InstallState();
        if (state == wmd::PackageInstallState::Installed || state == wmd::PackageInstallState::Paused) {
            sids.emplace_back(user.UserSecurityId());
        }
    }
    return sids;
}

// Everything but the users.
PackageInfo to_info(const wam::Package& package) {
    const auto id = package.Id();
    PackageInfo info;
    info.full_name = id.FullName();
    info.family_name = id.FamilyName();
    info.name = id.Name();
    info.version = version_text(id.Version());
    info.publisher = try_get([&] { return std::wstring(package.PublisherDisplayName()); });
    info.display_name = try_get([&] { return std::wstring(package.DisplayName()); });
    if (info.display_name.empty() || istarts_with(info.display_name, L"ms-resource:")) {
        info.display_name = info.name;
    }
    info.install_location = try_get([&] { return std::wstring(package.InstalledPath()); });
    // Windows 10 2004 and later, as the inventory's appx source reads it; empty for a package with
    // no external location.
    info.external_location = try_get([&] { return std::wstring(package.EffectiveExternalPath()); });
    info.signature_kind = try_get([&] { return to_signature_kind(package.SignatureKind()); });
    info.framework = try_get([&] { return package.IsFramework(); });
    info.resource = try_get([&] { return package.IsResourcePackage(); });
    info.bundle = try_get([&] { return package.IsBundle(); });
    info.optional = try_get([&] { return package.IsOptional(); });
    return info;
}

// The whole package with its users: every user's (needs administrator rights) or, with
// `all_users` false, just `current_sid`.
PackageInfo describe(const wmd::PackageManager& manager, const wam::Package& package, bool all_users,
                     const std::wstring& current_sid) {
    PackageInfo info = to_info(package);
    if (all_users) {
        info.user_sids = try_get([&] { return registered_users(manager, package.Id().FullName()); });
    } else if (!current_sid.empty()) {
        info.user_sids.push_back(current_sid);
    }
    return info;
}

std::optional<std::wstring> find_provisioned(const wmd::PackageManager& manager,
                                             const std::wstring& family_name) {
    for (const auto& package : manager.FindProvisionedPackages()) {
        const auto id = package.Id();
        if (iequals(std::wstring_view(id.FamilyName()), family_name)) {
            return std::wstring(id.FullName());
        }
    }
    return std::nullopt;
}

// The full name of the current user's package of the family, if they have one.
std::optional<std::wstring> current_user_package(const wmd::PackageManager& manager,
                                                 const std::wstring& family_name) {
    for (const auto& package : manager.FindPackagesForUser(L"", family_name)) {
        return std::wstring(package.Id().FullName());
    }
    return std::nullopt;
}

bool any_staged(const wmd::PackageManager& manager, const std::wstring& family_name) {
    const auto packages = manager.FindPackages(family_name);
    return packages.begin() != packages.end() || find_provisioned(manager, family_name).has_value();
}

std::wstring family_of(const std::wstring& full_name) {
    std::array<wchar_t, PACKAGE_FAMILY_NAME_MAX_LENGTH + 1> family{};
    UINT32 length = static_cast<UINT32>(family.size());
    if (::PackageFamilyNameFromFullName(full_name.c_str(), &length, family.data()) != ERROR_SUCCESS) {
        return {};
    }
    return family.data();
}

using DeploymentOperation = wf::IAsyncOperationWithProgress<wmd::DeploymentResult, wmd::DeploymentProgress>;

// Starts a deployment operation, waits for it and records its codes in `operation`.
void deploy(PackageOperation& operation, const std::function<DeploymentOperation()>& start) {
    try {
        const DeploymentOperation running = start();
        try {
            running.get();
        } catch (const winrt::hresult_error& error) {
            operation.hresult = error.code().value;
        }
        if (operation.hresult == 0) {
            operation.hresult = running.ErrorCode().value;
        }
        try {
            operation.extended = running.GetResults().ExtendedErrorCode().value;
        } catch (const winrt::hresult_error&) {
        }
    } catch (const winrt::hresult_error& error) {
        operation.hresult = error.code().value;  // Refused at the start
    }
}

// Runs a change and then reads back its effect with `confirm` (false when that read fails), both in
// the MTA. Never trusts the operation's codes alone (ADR 0008).
PackageOperation change(const std::function<DeploymentOperation(const wmd::PackageManager&)>& start,
                        const std::function<bool(const wmd::PackageManager&, PackageOperation&)>& confirm) {
    return run_in_mta([&] {
        PackageOperation operation;
        try {
            const wmd::PackageManager manager;
            deploy(operation, [&] { return start(manager); });
            try {
                operation.confirmed = confirm(manager, operation);
            } catch (const winrt::hresult_error&) {
                operation.confirmed = false;
            }
        } catch (const winrt::hresult_error& error) {
            operation.hresult = error.code().value;
        }
        return operation;
    });
}

}  // namespace

PackageListing list_packages() {
    const bool elevated = is_process_elevated();
    const std::wstring current_sid = current_user_sid();
    return run_in_mta([&] {
        PackageListing listing;
        listing.all_users = elevated;
        try {
            const wmd::PackageManager manager;
            // An empty SID means the current user.
            const auto packages = elevated ? manager.FindPackagesWithPackageTypes(kListedTypes)
                                           : manager.FindPackagesForUserWithPackageTypes(L"", kListedTypes);
            for (const auto& package : packages) {
                try {
                    listing.packages.push_back(describe(manager, package, elevated, current_sid));
                } catch (const winrt::hresult_error&) {
                    // A package removed while listed, or one that can't be inspected: skipped
                }
            }
            if (elevated) {
                for (const auto& package : manager.FindProvisionedPackages()) {
                    const std::wstring full_name(package.Id().FullName());
                    listing.provisioned.push_back(full_name);
                    // A provisioned package is staged, so FindPackages lists it; just in case it
                    // doesn't, it is added here.
                    const bool listed = std::any_of(
                        listing.packages.begin(), listing.packages.end(),
                        [&](const PackageInfo& info) { return iequals(info.full_name, full_name); });
                    if (!listed) {
                        listing.packages.push_back(describe(manager, package, true, current_sid));
                    }
                }
            }
        } catch (const winrt::hresult_error& error) {
            listing.error = error.code().value;
        }
        return listing;
    });
}

std::optional<PackageInfo> find_package(const std::wstring& full_name) {
    const bool elevated = is_process_elevated();
    const std::wstring current_sid = current_user_sid();
    return run_in_mta([&]() -> std::optional<PackageInfo> {
        try {
            const wmd::PackageManager manager;
            const auto package =
                elevated ? manager.FindPackage(full_name) : manager.FindPackageForUser(L"", full_name);
            if (!package) {
                return std::nullopt;
            }
            return describe(manager, package, elevated, current_sid);
        } catch (const winrt::hresult_error&) {
            return std::nullopt;
        }
    });
}

bool family_staged(const std::wstring& family_name) {
    return run_in_mta([&] {
        try {
            return any_staged(wmd::PackageManager(), family_name);
        } catch (const winrt::hresult_error&) {
            return false;
        }
    });
}

std::vector<std::wstring> family_users(const std::wstring& family_name) {
    return run_in_mta([&] {
        std::vector<std::wstring> users;
        try {
            const wmd::PackageManager manager;
            for (const auto& package : manager.FindPackages(family_name)) {
                for (auto& sid : registered_users(manager, package.Id().FullName())) {
                    const bool known = std::any_of(users.begin(), users.end(), [&](const std::wstring& user) {
                        return iequals(user, sid);
                    });
                    if (!known) {
                        users.push_back(std::move(sid));
                    }
                }
            }
        } catch (const winrt::hresult_error&) {
        }
        return users;
    });
}

std::optional<std::wstring> provisioned_package(const std::wstring& family_name) {
    return run_in_mta([&]() -> std::optional<std::wstring> {
        try {
            return find_provisioned(wmd::PackageManager(), family_name);
        } catch (const winrt::hresult_error&) {
            return std::nullopt;
        }
    });
}

PackageOperation remove_for_current_user(const std::wstring& full_name) {
    const std::wstring family_name = family_of(full_name);
    return change(
        [&](const wmd::PackageManager& manager) {
            return manager.RemovePackageAsync(full_name, wmd::RemovalOptions::None);
        },
        [&](const wmd::PackageManager& manager, PackageOperation& operation) {
            operation.full_name = full_name;
            return !family_name.empty() && !current_user_package(manager, family_name);
        });
}

PackageOperation register_for_current_user(const std::wstring& family_name) {
    return change(
        [&](const wmd::PackageManager& manager) {
            return manager.RegisterPackageByFamilyNameAsync(family_name, nullptr,
                                                            wmd::DeploymentOptions::None, nullptr, nullptr);
        },
        [&](const wmd::PackageManager& manager, PackageOperation& operation) {
            // The operation can report success with nothing registered (ADR 0008, finding 2).
            const auto registered = current_user_package(manager, family_name);
            operation.full_name = registered.value_or(L"");
            return registered.has_value();
        });
}

PackageOperation deprovision(const std::wstring& family_name) {
    const std::optional<std::wstring> provisioned = provisioned_package(family_name);
    return change(
        [&](const wmd::PackageManager& manager) {
            return manager.DeprovisionPackageForAllUsersAsync(family_name);
        },
        [&](const wmd::PackageManager& manager, PackageOperation& operation) {
            operation.full_name = provisioned.value_or(L"");
            return !find_provisioned(manager, family_name);
        });
}

PackageOperation provision(const std::wstring& family_name) {
    return change(
        [&](const wmd::PackageManager& manager) {
            return manager.ProvisionPackageForAllUsersAsync(family_name);
        },
        [&](const wmd::PackageManager& manager, PackageOperation& operation) {
            const auto provisioned = find_provisioned(manager, family_name);
            operation.full_name = provisioned.value_or(L"");
            return provisioned.has_value();
        });
}

PackageOperation remove_for_all_users(const std::wstring& full_name) {
    const std::wstring family_name = family_of(full_name);
    return change(
        [&](const wmd::PackageManager& manager) {
            return manager.RemovePackageAsync(full_name, wmd::RemovalOptions::RemoveForAllUsers);
        },
        [&](const wmd::PackageManager& manager, PackageOperation& operation) {
            operation.full_name = full_name;
            return !family_name.empty() && !any_staged(manager, family_name);
        });
}

}  // namespace avel::platform
