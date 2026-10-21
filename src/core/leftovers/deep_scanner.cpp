// The deep search: folders named after the program anywhere on the fixed NTFS volumes, not just
// in the common locations the drive scan walks. Avel's own; BCU has nothing like it. ADR 0005
// explains why the folders come from the master file table.
#include <windows.h>

#include <algorithm>
#include <array>
#include <future>
#include <unordered_map>

#include "core/leftovers/confidence.h"
#include "core/leftovers/product_name.h"
#include "core/leftovers/protection.h"
#include "core/leftovers/scanners.h"
#include "core/platform/file_ops.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::leftovers::detail {

namespace {

// Folders at a volume's root that hold Windows' own data or copies of other folders: the
// recycle bin, Avel's quarantine ($Avel.Quarantine), restore points, a previous Windows.
bool is_skipped_root_folder(std::wstring_view name) {
    constexpr std::array<std::wstring_view, 4> kSkipped = {L"System Volume Information", L"Recovery",
                                                           L"Windows.old", L"Config.Msi"};
    return name.starts_with(L'$') || std::any_of(kSkipped.begin(), kSkipped.end(), [&](auto skipped) {
               return platform::iequals(name, skipped);
           });
}

// The first folder below the volume root: "Tools" for C:\Tools\App.
std::wstring_view root_folder_of(std::wstring_view path) {
    if (path.size() < 4) {
        return {};
    }
    const std::wstring_view rest = path.substr(3);  // After "C:\"
    return rest.substr(0, rest.find(L'\\'));
}

bool any_positive(const std::vector<Record>& records) {
    return std::any_of(records.begin(), records.end(),
                       [](const Record& record) { return points(record) > 0; });
}

const UserScope* user_of(const ScanEnvironment& environment, const std::wstring& path) {
    for (const auto& user : environment.users) {
        if (!user.profile.profile_path.empty() && platform::path_is_under(path, user.profile.profile_path)) {
            return &user;
        }
    }
    return nullptr;
}

}  // namespace

void take_deep_folders(const ScanEnvironment& environment, Snapshots& snapshots,
                       std::vector<ScanDiagnostic>& diagnostics) {
    auto& deep = snapshots.deep;
    if (environment.volumes) {
        deep.volumes = *environment.volumes;
    } else {
        // One thread per volume: each read is one sequential pass over its index.
        const auto roots = platform::fixed_ntfs_volumes();
        std::vector<std::future<std::pair<std::uint32_t, platform::VolumeFolders>>> reads;
        for (const auto& root : roots) {
            reads.push_back(std::async(std::launch::async, [root] {
                platform::VolumeFolders folders;
                const std::uint32_t error = platform::VolumeFolders::read(root, folders);
                return std::pair{error, std::move(folders)};
            }));
        }
        for (std::size_t i = 0; i < reads.size(); ++i) {
            auto [error, folders] = reads[i].get();
            if (error == ERROR_SUCCESS) {
                deep.volumes.push_back(std::move(folders));
            } else {
                diagnostics.push_back(
                    {ScanDiagnosticCode::DeepSearchUnavailable, roots[i], static_cast<std::int32_t>(error)});
            }
        }
    }

    // Many folders share a name (bin, en-US, ...), so each name is matched once per target.
    std::unordered_map<std::wstring, std::size_t> by_name;
    for (std::uint32_t volume = 0; volume < deep.volumes.size(); ++volume) {
        const auto& folders = deep.volumes[volume].folders();
        for (std::uint32_t index = 0; index < folders.size(); ++index) {
            const auto& folder = folders[index];
            // Names of 4 characters or fewer never match (see match_product_name).
            if ((folder.attributes & (FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
                name_without_extension(folder.name).size() <= 4 || is_blacklisted_folder_name(folder.name)) {
                continue;
            }
            const auto [at, added] = by_name.try_emplace(platform::to_lower(folder.name), deep.names.size());
            if (added) {
                deep.names.push_back({folder.name, {}});
            }
            deep.names[at->second].folders.push_back({volume, index});
        }
    }
}

void scan_deep(const ScanContext& context, std::vector<Leftover>& found) {
    const auto& deep = context.snapshots.deep;
    const auto& target = context.target;
    const auto& windows = context.environment.machine_folders.windows;
    // Looked at already: the drive scan's roots, and the program's own folders (the install
    // location scan). What lies in another program's folder is that program's.
    const auto searched_elsewhere = [&](const std::wstring& path) {
        const auto under = [&](const std::wstring& folder) {
            return !folder.empty() && platform::path_is_under(path, folder);
        };
        return under(windows) || under(target.install_location) || under(target.uninstaller_folder) ||
               std::any_of(context.snapshots.folder_roots.begin(), context.snapshots.folder_roots.end(),
                           under) ||
               std::any_of(context.other_locations.begin(), context.other_locations.end(), under);
    };
    const std::wstring publisher = platform::to_lower(target.names.publisher);

    for (const auto& name : deep.names) {
        const std::wstring_view item = name_without_extension(name.name);
        const int match = match_product_name(target.names, item);
        if (match < 0) {
            continue;
        }
        for (const auto& [volume_index, index] : name.folders) {
            const auto& volume = deep.volumes[volume_index];
            const std::wstring path = volume.path_of(index);
            if (path.empty() || is_skipped_root_folder(root_folder_of(path)) || searched_elsewhere(path)) {
                continue;
            }
            const UserScope* user = user_of(context.environment, path);
            if (!context.in_scope(user)) {
                continue;
            }
            // Like the drive scan's name evidence (name_confidence), without depth: there is no
            // search root to measure it from.
            std::vector<Record> records{
                {match < 2 ? Evidence::ProductNamePerfectMatch : Evidence::ProductNameDodgyMatch},
                {Evidence::OutsideCommonLocations}};
            std::wstring parent(volume.parent_name(index));
            std::replace(parent.begin(), parent.end(), L'_', L' ');
            if (!parent.empty() && !publisher.empty() &&
                publisher.find(platform::to_lower(parent)) != std::wstring::npos) {
                records.push_back({Evidence::CompanyNameMatch});
            }
            if (item_name_equals_company_name(target.names, item)) {
                records.push_back({Evidence::ItemNameEqualsCompanyName});
            }
            if (!any_positive(records)) {
                continue;
            }
            if (is_questionable_folder_name(item)) {
                records.push_back({Evidence::QuestionableDirectoryName});
            }
            if (context.still_used(path)) {
                records.push_back({Evidence::DirectoryStillUsed});
            }
            if (context.other_user(user)) {
                records.push_back({Evidence::OtherUserData});
            }
            Leftover leftover;
            leftover.location = FileLocation{path, true};
            leftover.records = std::move(records);
            leftover.owner_ids = {target.program_id};
            leftover.found_by = Scanner::DeepSearch;
            leftover.category = Category::Files;
            found.push_back(std::move(leftover));
        }
    }
}

}  // namespace avel::leftovers::detail
