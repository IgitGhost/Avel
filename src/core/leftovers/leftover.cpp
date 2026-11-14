#include "core/leftovers/leftover.h"

#include <algorithm>
#include <type_traits>

#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::leftovers {

namespace {

// Registry paths compare like file paths, but key names may legally contain "." and "..", so
// they are matched literally instead of through normalize_path.
bool key_is_under(const platform::RegistryAddress& key, const platform::RegistryAddress& ancestor) {
    if (key.hive != ancestor.hive || !platform::iequals(key.sid, ancestor.sid)) {
        return false;
    }
    if (ancestor.path.empty()) {
        return true;
    }
    if (!platform::istarts_with(key.path, ancestor.path)) {
        return false;
    }
    return key.path.size() == ancestor.path.size() || key.path[ancestor.path.size()] == L'\\';
}

}  // namespace

std::wstring identity(const Location& location) {
    return std::visit(
        [](const auto& item) -> std::wstring {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, FileLocation>) {
                return L"file:" + platform::to_lower(platform::normalize_path(item.path));
            } else if constexpr (std::is_same_v<T, RegistryKeyLocation>) {
                return L"key:" + platform::to_lower(platform::display_path(item.key));
            } else if constexpr (std::is_same_v<T, RegistryValueLocation>) {
                return L"value:" + platform::to_lower(platform::display_path(item.key)) + L"\\\\" +
                       platform::to_lower(item.value_name);
            } else if constexpr (std::is_same_v<T, ServiceLocation>) {
                return L"service:" + platform::to_lower(item.name);
            } else if constexpr (std::is_same_v<T, TaskLocation>) {
                return (item.folder ? L"task-folder:" : L"task:") + platform::to_lower(item.path);
            } else if constexpr (std::is_same_v<T, PackageLocation>) {
                return L"package:" +
                       (item.all_users ? std::wstring(L"all-users") : platform::to_lower(item.user_sid)) +
                       L":" + platform::to_lower(item.full_name);
            } else if constexpr (std::is_same_v<T, PackageUserLocation>) {
                return L"package-user:" + platform::to_lower(item.user_sid) + L"\\" +
                       platform::to_lower(item.family_name);
            } else if constexpr (std::is_same_v<T, PackageProvisioningLocation>) {
                return L"package-provisioning:" + platform::to_lower(item.family_name);
            } else if constexpr (std::is_same_v<T, FirewallRuleLocation>) {
                return L"firewall:" + platform::to_lower(item.rule_id);
            } else if constexpr (std::is_same_v<T, StartupApprovedLocation>) {
                return L"approved:" + platform::to_lower(platform::display_path(item.key)) + L"\\\\" +
                       platform::to_lower(item.value_name);
            } else if constexpr (std::is_same_v<T, TaskStateLocation>) {
                return L"task-state:" + platform::to_lower(item.path);
            } else if constexpr (std::is_same_v<T, ServiceStartLocation>) {
                return L"service-start:" + platform::to_lower(item.name);
            } else {
                static_assert(std::is_same_v<T, ValueEditLocation>);
                std::wstring text = L"edit:" + platform::to_lower(platform::display_path(item.key)) +
                                    L"\\\\" + platform::to_lower(item.value_name) + L"|" +
                                    platform::to_utf16(to_string(item.kind));
                if (item.kind == ValueEditKind::RemovePathEntries) {
                    for (std::size_t i = 0; i < item.entries.size(); ++i) {
                        text += (i == 0 ? L"|" : L";") + platform::to_lower(item.entries[i]);
                    }
                } else if (item.delta != 0) {
                    text += L"|" + std::to_wstring(item.delta);
                }
                return text;
            }
        },
        location);
}

std::wstring package_family_name(std::wstring_view full_name) {
    // Identity names, versions, architectures, resource ids and publisher ids hold no '_'.
    std::vector<std::wstring_view> parts;
    for (std::size_t start = 0;;) {
        const std::size_t end = full_name.find(L'_', start);
        parts.push_back(full_name.substr(start, end == std::wstring_view::npos ? end : end - start));
        if (end == std::wstring_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (parts.size() != 5 || parts[0].empty() || parts[4].empty()) {
        return {};
    }
    return std::wstring(parts[0]) + L"_" + std::wstring(parts[4]);
}

bool contains(const Location& outer, const Location& inner) {
    if (const auto* package = std::get_if<PackageLocation>(&outer); package && package->all_users) {
        // Removing it for every user also unregisters it for each and deprovisions it.
        const std::wstring family = package_family_name(package->full_name);
        if (const auto* user = std::get_if<PackageUserLocation>(&inner)) {
            return !family.empty() && platform::iequals(user->family_name, family);
        }
        if (const auto* provisioning = std::get_if<PackageProvisioningLocation>(&inner)) {
            return !family.empty() && platform::iequals(provisioning->family_name, family);
        }
        return false;
    }
    if (const auto* folder = std::get_if<FileLocation>(&outer)) {
        const auto* file = std::get_if<FileLocation>(&inner);
        return folder->directory && file && platform::path_is_inside(file->path, folder->path);
    }
    if (const auto* key = std::get_if<RegistryKeyLocation>(&outer)) {
        if (const auto* subkey = std::get_if<RegistryKeyLocation>(&inner)) {
            return key_is_under(subkey->key, key->key) && !platform::same_address(subkey->key, key->key);
        }
        if (const auto* value = std::get_if<RegistryValueLocation>(&inner)) {
            return key_is_under(value->key, key->key);
        }
    }
    return false;
}

std::wstring owner_sid(const Location& location) {
    if (const auto* key = std::get_if<RegistryKeyLocation>(&location)) {
        return key->key.sid;
    }
    if (const auto* value = std::get_if<RegistryValueLocation>(&location)) {
        return value->key.sid;
    }
    if (const auto* package = std::get_if<PackageLocation>(&location)) {
        return package->all_users ? std::wstring() : package->user_sid;
    }
    if (const auto* registration = std::get_if<PackageUserLocation>(&location)) {
        return registration->user_sid;
    }
    if (const auto* edit = std::get_if<ValueEditLocation>(&location)) {
        return edit->key.sid;
    }
    if (const auto* approval = std::get_if<StartupApprovedLocation>(&location)) {
        return approval->key.sid;
    }
    return {};
}

Category category_of(const Location& location) {
    return std::visit(
        [](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, FileLocation>) {
                return !item.directory && platform::iends_with(item.path, L".lnk") ? Category::Shortcuts
                                                                                   : Category::Files;
            } else if constexpr (std::is_same_v<T, ServiceLocation> ||
                                 std::is_same_v<T, ServiceStartLocation>) {
                return Category::Services;
            } else if constexpr (std::is_same_v<T, TaskLocation> || std::is_same_v<T, TaskStateLocation>) {
                return Category::Tasks;
            } else if constexpr (std::is_same_v<T, PackageLocation> ||
                                 std::is_same_v<T, PackageUserLocation> ||
                                 std::is_same_v<T, PackageProvisioningLocation>) {
                return Category::Packages;
            } else if constexpr (std::is_same_v<T, FirewallRuleLocation>) {
                return Category::Firewall;
            } else {
                return Category::Registry;
            }
        },
        location);
}

bool excluded_in_trace_review(const Leftover& leftover) {
    return std::any_of(leftover.records.begin(), leftover.records.end(), [](const Record& record) {
        return record.evidence == Evidence::ExcludedInTraceReview;
    });
}

std::vector<bool> held_back(std::span<const Leftover> leftovers) {
    const auto holds_exclusion = [](const Leftover& leftover) {
        return std::any_of(leftover.records.begin(), leftover.records.end(), [](const Record& record) {
            return record.evidence == Evidence::HoldsTraceReviewExclusion;
        });
    };
    std::vector<bool> held(leftovers.size(), false);
    for (std::size_t i = 0; i < leftovers.size(); ++i) {
        if (!excluded_in_trace_review(leftovers[i]) && !holds_exclusion(leftovers[i])) {
            continue;
        }
        held[i] = true;
        // Bounded, in case the links ever loop.
        auto container = leftovers[i].inside;
        for (std::size_t steps = 0; container && *container < leftovers.size() && steps < leftovers.size();
             ++steps) {
            held[*container] = true;
            container = leftovers[*container].inside;
        }
    }
    return held;
}

std::string_view to_string(Category category) {
    switch (category) {
        case Category::Files:
            return "files";
        case Category::Shortcuts:
            return "shortcuts";
        case Category::Registry:
            return "registry";
        case Category::Services:
            return "services";
        case Category::Tasks:
            return "tasks";
        case Category::Packages:
            return "packages";
        case Category::Firewall:
            return "firewall";
    }
    return "unknown";
}

std::string_view to_string(Scanner scanner) {
    switch (scanner) {
        case Scanner::InstallLocation:
            return "install-location";
        case Scanner::CommonDrive:
            return "common-drive";
        case Scanner::SoftwareKey:
            return "software-key";
        case Scanner::UninstallerKey:
            return "uninstaller-key";
        case Scanner::PathReferences:
            return "path-references";
        case Scanner::Shortcuts:
            return "shortcuts";
        case Scanner::RelatedPackages:
            return "related-packages";
        case Scanner::Services:
            return "services";
        case Scanner::Tasks:
            return "tasks";
        case Scanner::ComClasses:
            return "com-classes";
        case Scanner::Firewall:
            return "firewall";
        case Scanner::MsiRegistration:
            return "msi-registration";
        case Scanner::DeepSearch:
            return "deep-search";
        case Scanner::Orphans:
            return "orphans";
        case Scanner::Trace:
            return "trace";
    }
    return "unknown";
}

std::string_view to_string(ValueEditKind kind) {
    switch (kind) {
        case ValueEditKind::RemovePathEntries:
            return "remove-path-entries";
        case ValueEditKind::DecrementSharedDll:
            return "decrement-shared-dll";
        case ValueEditKind::DeleteValue:
            return "delete-value";
        case ValueEditKind::RestoreValue:
            return "restore-value";
    }
    return "unknown";
}

bool parse(std::string_view text, ValueEditKind& kind) {
    for (const auto candidate : {ValueEditKind::RemovePathEntries, ValueEditKind::DecrementSharedDll,
                                 ValueEditKind::DeleteValue, ValueEditKind::RestoreValue}) {
        if (to_string(candidate) == text) {
            kind = candidate;
            return true;
        }
    }
    return false;
}

}  // namespace avel::leftovers
