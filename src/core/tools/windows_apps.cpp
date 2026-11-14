#include "core/tools/windows_apps.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "core/platform/hash.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"

namespace avel::tools {

namespace {

using platform::PackageInfo;

// "a.b.c.d" as four numbers, for ordering versions; missing or bad parts count as 0.
std::array<unsigned long, 4> version_parts(std::wstring_view version) {
    std::array<unsigned long, 4> parts{};
    for (auto& part : parts) {
        const std::size_t dot = version.find(L'.');
        const std::wstring text(version.substr(0, dot));
        part = std::wcstoul(text.c_str(), nullptr, 10);
        if (dot == std::wstring_view::npos) {
            break;
        }
        version.remove_prefix(dot + 1);
    }
    return parts;
}

bool has_user(const std::vector<std::wstring>& sids, const std::wstring& sid) {
    return !sid.empty() && std::any_of(sids.begin(), sids.end(), [&](const std::wstring& user) {
        return platform::iequals(user, sid);
    });
}

// The family of a full name (Name_Version_Architecture_ResourceId_PublisherId): Name_PublisherId.
std::wstring family_of_full_name(std::wstring_view full_name) {
    const std::size_t first = full_name.find(L'_');
    const std::size_t last = full_name.rfind(L'_');
    if (first == std::wstring_view::npos || first == last) {
        return {};
    }
    return std::wstring(full_name.substr(0, first)) + std::wstring(full_name.substr(last));
}

WindowsApp make_app(std::vector<const PackageInfo*> packages, const platform::PackageListing& listing,
                    const WindowsAppsEnvironment& environment) {
    std::stable_sort(packages.begin(), packages.end(), [](const PackageInfo* a, const PackageInfo* b) {
        return version_parts(a->version) > version_parts(b->version);
    });
    const std::wstring& current_sid = environment.current_user_sid;
    const auto current = std::find_if(packages.begin(), packages.end(), [&](const PackageInfo* package) {
        return has_user(package->user_sids, current_sid);
    });
    const PackageInfo& primary = current != packages.end() ? **current : *packages.front();

    WindowsApp app;
    app.family_name = primary.family_name;
    app.id = windows_app_id(app.family_name);
    app.name = primary.name;
    app.display_name = primary.display_name.empty() ? primary.name : primary.display_name;
    app.publisher = primary.publisher;
    app.version = primary.version;
    app.install_location = primary.install_location;
    app.signature_kind = primary.signature_kind;
    if (current != packages.end()) {
        app.current_user_full_name = primary.full_name;
    }

    std::vector<std::wstring> sids;
    bool system_signed = false;
    app.external_location = primary.external_location;
    for (const PackageInfo* package : packages) {
        app.packages.push_back({package->full_name, package->version, package->user_sids});
        system_signed = system_signed || package->signature_kind == platform::PackageSignatureKind::System;
        if (app.external_location.empty()) {
            app.external_location = package->external_location;  // Any version's gives it away
        }
        for (const auto& sid : package->user_sids) {
            if (!has_user(sids, sid)) {
                sids.push_back(sid);
            }
        }
    }
    for (auto& sid : sids) {
        WindowsAppUser user;
        user.current = platform::iequals(sid, current_sid);
        user.account_name = environment.account_name ? environment.account_name(sid) : std::wstring();
        if (user.account_name.empty()) {
            user.account_name = sid;
        }
        user.sid = std::move(sid);
        app.users.push_back(std::move(user));
    }
    std::stable_sort(
        app.users.begin(), app.users.end(), [](const WindowsAppUser& a, const WindowsAppUser& b) {
            if (a.current != b.current) {
                return a.current;
            }
            return ::CompareStringOrdinal(a.account_name.c_str(), static_cast<int>(a.account_name.size()),
                                          b.account_name.c_str(), static_cast<int>(b.account_name.size()),
                                          TRUE) == CSTR_LESS_THAN;
        });

    for (const auto& full_name : listing.provisioned) {
        if (platform::iequals(family_of_full_name(full_name), app.family_name)) {
            app.provisioned = true;
            app.provisioned_full_name = full_name;
            break;
        }
    }

    app.category = app_category(app.family_name);
    app.warning = app_warning(app.family_name);
    if (system_signed) {
        app.protection = AppProtection::System;
    } else if (app.category == AppCategory::Critical) {
        app.protection = AppProtection::Critical;
    } else if (!app.external_location.empty()) {
        app.protection = AppProtection::ExternalLocation;  // It goes with its desktop program
    }

    // Only a complete listing tells who else has it and whether it is provisioned.
    const bool someone_else = std::any_of(app.users.begin(), app.users.end(),
                                          [](const WindowsAppUser& user) { return !user.current; });
    app.soft_undoable = listing.all_users && (app.provisioned || someone_else);
    // Nothing is offered from an incomplete listing either: the planner refuses it all
    // (incomplete-listing), and WindowsAppsList::all_users tells the front ends why.
    if (app.protection == AppProtection::None && listing.all_users) {
        app.can_remove_for_me = !app.current_user_full_name.empty() && app.soft_undoable;
        app.can_deprovision = app.provisioned;
        app.can_remove_for_everyone = true;
    }
    return app;
}

}  // namespace

WindowsAppsEnvironment live_windows_apps_environment() {
    WindowsAppsEnvironment environment;
    environment.current_user_sid = platform::current_user_sid();
    struct Cache {
        std::mutex mutex;
        std::map<std::wstring, std::wstring> names;  // By lowercase SID
    };
    environment.account_name = [cache = std::make_shared<Cache>()](const std::wstring& sid) {
        const std::wstring key = platform::to_lower(sid);
        {
            const std::lock_guard lock(cache->mutex);
            if (const auto found = cache->names.find(key); found != cache->names.end()) {
                return found->second;
            }
        }
        std::wstring name = platform::lookup_account_name(sid);
        const std::lock_guard lock(cache->mutex);
        cache->names.emplace(key, name);
        return name;
    };
    return environment;
}

WindowsAppsList list_windows_apps(const WindowsAppsEnvironment& environment) {
    const platform::PackageListing listing =
        environment.listing ? *environment.listing : platform::list_packages();
    WindowsAppsList list;
    list.all_users = listing.all_users;
    list.error = listing.error;

    std::vector<std::vector<const PackageInfo*>> families;
    std::unordered_map<std::wstring, std::size_t> by_family;  // Lowercase family name -> index
    for (const PackageInfo& package : listing.packages) {
        // Frameworks and resource packages aren't apps of their own, and a bundle only holds packages.
        if (package.framework || package.resource || package.bundle || package.family_name.empty()) {
            continue;
        }
        const auto [found, added] =
            by_family.try_emplace(platform::to_lower(package.family_name), families.size());
        if (added) {
            families.emplace_back();
        }
        families[found->second].push_back(&package);
    }

    for (auto& packages : families) {
        list.apps.push_back(make_app(std::move(packages), listing, environment));
    }
    std::sort(list.apps.begin(), list.apps.end(), [](const WindowsApp& a, const WindowsApp& b) {
        const int order =
            ::CompareStringOrdinal(a.display_name.c_str(), static_cast<int>(a.display_name.size()),
                                   b.display_name.c_str(), static_cast<int>(b.display_name.size()), TRUE);
        if (order != CSTR_EQUAL) {
            return order == CSTR_LESS_THAN;
        }
        return a.family_name < b.family_name;
    });
    return list;
}

std::wstring windows_app_id(std::wstring_view family_name) {
    const std::string key = platform::to_utf8(platform::to_lower(family_name));
    return std::format(L"{:010x}", platform::fnv1a(key.data(), key.size()) >> 24);
}

std::string_view to_string(AppProtection protection) {
    switch (protection) {
        case AppProtection::None:
            return "none";
        case AppProtection::System:
            return "system";
        case AppProtection::Critical:
            return "critical";
        case AppProtection::ExternalLocation:
            return "external-location";
    }
    return "unknown";
}

}  // namespace avel::tools
