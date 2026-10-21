#include "core/force/force.h"

#include <windows.h>

#include <algorithm>

#include "core/leftovers/product_name.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/platform/version_info.h"

namespace avel::force {

namespace {

using leftovers::ProtectionReason;

std::vector<std::wstring> known_folders(const leftovers::ScanEnvironment& environment) {
    std::vector<std::wstring> folders = environment.machine_folders.all;
    for (const auto& user : environment.users) {
        folders.insert(folders.end(), user.folders.all.begin(), user.folders.all.end());
    }
    return folders;
}

ProtectionReason check(const leftovers::ProtectedPolicy& policy, std::wstring_view folder) {
    const leftovers::Location location = leftovers::FileLocation{std::wstring(folder), true};
    const ProtectionReason reason = leftovers::HardFloor::live().check(location);
    return reason != ProtectionReason::None ? reason : policy.check(location);
}

}  // namespace

ProtectionReason folder_protection(const leftovers::ScanEnvironment& environment, std::wstring_view folder) {
    return check(leftovers::ProtectedPolicy(known_folders(environment)), folder);
}

FolderTarget folder_target(const leftovers::ScanEnvironment& environment, std::wstring_view folder,
                           std::wstring name, std::wstring publisher) {
    FolderTarget result;
    const std::wstring path = platform::long_path(folder);
    if (!platform::is_absolute_path(path)) {
        result.error = FolderError::NotAbsolute;
        return result;
    }
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        result.error = FolderError::NotFound;
        return result;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        result.error = FolderError::NotAFolder;
        return result;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        result.error = FolderError::ReparsePoint;
        return result;
    }
    result.protection = folder_protection(environment, path);
    if (result.protection != ProtectionReason::None) {
        result.error = FolderError::Protected;
        return result;
    }
    if (name.empty()) {
        name = std::wstring(platform::last_path_component(path));
    }
    auto& target = result.target;
    target.program_id = L"folder:" + path;
    target.display_name = name;
    target.names = leftovers::make_product_names(name, publisher);
    target.install_location = path;
    return result;
}

FileOwner owner_of(const leftovers::ScanEnvironment& environment,
                   std::span<const inventory::Program> programs, std::wstring_view file) {
    FileOwner owner;
    const std::wstring path = platform::long_path(file);
    const leftovers::ProtectedPolicy policy(known_folders(environment));

    std::size_t best_length = 0;
    for (const auto& program : programs) {
        const auto target = leftovers::make_scan_target(program);
        const bool package = program.source == inventory::Source::Appx;
        for (const auto* folder : {&target.install_location, &target.uninstaller_folder}) {
            // A package's folder is protected (WindowsApps), but it is the package's own.
            if (folder->empty() || folder->size() < best_length || !platform::path_is_under(path, *folder) ||
                (!package && check(policy, *folder) != ProtectionReason::None)) {
                continue;
            }
            // On a tie, the entry Settings lists beats a hidden component of the same program.
            const bool better = !owner.program || folder->size() > best_length ||
                                (!inventory::is_listed_in_settings(*owner.program) &&
                                 inventory::is_listed_in_settings(program));
            if (better) {
                owner.program = &program;
                best_length = folder->size();
            }
        }
    }
    if (owner.program) {
        return owner;
    }

    const platform::FileProduct product = platform::file_product(path);
    owner.name = product.product_name;
    owner.publisher = product.company_name;
    owner.folder = platform::parent_path(path);
    if (!owner.name.empty()) {
        // "C:\Program Files\Contoso\Fixture\bin\app.exe" with product "Contoso Fixture": the
        // Fixture folder, but never the publisher's folder above it, which may hold other programs.
        const auto names = leftovers::make_product_names(owner.name, owner.publisher);
        for (std::wstring folder = platform::parent_path(owner.folder);
             !folder.empty() && !platform::is_root_path(folder) &&
             check(policy, folder) == ProtectionReason::None;
             folder = platform::parent_path(folder)) {
            if (leftovers::match_product_name(names, platform::last_path_component(folder)) >= 0) {
                owner.folder = folder;
            }
        }
    }
    if (owner.name.empty()) {
        owner.name = std::wstring(platform::last_path_component(owner.folder));
    }
    return owner;
}

std::vector<platform::ProcessInfo> processes_in(const leftovers::ScanEnvironment& environment,
                                                std::span<const leftovers::ScanTarget> targets,
                                                std::span<const platform::ProcessInfo> processes,
                                                std::span<const std::uint32_t> excluded) {
    const leftovers::ProtectedPolicy policy(known_folders(environment));
    std::vector<std::wstring> folders;
    for (const auto& target : targets) {
        for (const auto* folder : {&target.install_location, &target.uninstaller_folder}) {
            if (!folder->empty() && check(policy, *folder) == ProtectionReason::None) {
                folders.push_back(platform::normalize_path(*folder));
            }
        }
    }
    const DWORD self = ::GetCurrentProcessId();
    std::vector<platform::ProcessInfo> found;
    for (const auto& process : processes) {
        if (process.pid == 0 || process.pid == 4 || process.pid == self || process.critical ||
            process.image.empty() ||
            std::find(excluded.begin(), excluded.end(), process.pid) != excluded.end()) {
            continue;
        }
        if (std::any_of(folders.begin(), folders.end(), [&](const auto& folder) {
                return platform::normalized_path_is_under(process.image, folder);
            })) {
            found.push_back(process);
        }
    }
    return found;
}

std::string_view to_string(FolderError error) {
    switch (error) {
        case FolderError::None:
            return "none";
        case FolderError::NotAbsolute:
            return "not-absolute";
        case FolderError::NotFound:
            return "not-found";
        case FolderError::NotAFolder:
            return "not-a-folder";
        case FolderError::ReparsePoint:
            return "reparse-point";
        case FolderError::Protected:
            return "protected";
    }
    return "unknown";
}

}  // namespace avel::force
