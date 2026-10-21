#include "core/leftovers/leftover.h"

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
                return L"task:" + platform::to_lower(item.path);
            } else if constexpr (std::is_same_v<T, PackageLocation>) {
                return L"package:" + platform::to_lower(item.user_sid) + L":" + platform::to_lower(item.full_name);
            } else {
                return L"firewall:" + platform::to_lower(item.rule_id);
            }
        },
        location);
}

bool contains(const Location& outer, const Location& inner) {
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
        return package->user_sid;
    }
    return {};
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
    }
    return "unknown";
}

}  // namespace avel::leftovers
