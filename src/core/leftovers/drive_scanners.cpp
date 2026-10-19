// Ported in part from Bulk Crap Uninstaller, Copyright (c) 2017 Marcin Szeniak, Apache License 2.0.
// See scanners.h for the original source files.
#include <windows.h>

#include <wil/resource.h>

#include <algorithm>
#include <map>
#include <unordered_set>

#include "core/leftovers/product_name.h"
#include "core/leftovers/protection.h"
#include "core/leftovers/scanners.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::leftovers::detail {

namespace {

bool any_positive(const std::vector<Record>& records) {
    return std::any_of(records.begin(), records.end(), [](const Record& record) { return points(record) > 0; });
}

Leftover make_item(const ScanContext& context, Location location, std::vector<Record> records, Scanner scanner,
                   Category category) {
    Leftover item;
    item.location = std::move(location);
    item.records = std::move(records);
    item.owner_ids = {context.target.program_id};
    item.found_by = scanner;
    item.category = category;
    return item;
}

// Whether `folder` holds no files, and the full paths of its subfolders (all of them, as BCU's
// DirectoryInfo.GetDirectories returns them).
struct FolderContents {
    bool has_files = false;
    std::vector<std::wstring> subfolders;
};

FolderContents read_contents(const std::wstring& folder) {
    FolderContents contents;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = (folder.size() > 248 ? L"\\\\?\\" + folder : folder) + L"\\*";
    wil::unique_hfind find(::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                              nullptr, FIND_FIRST_EX_LARGE_FETCH));
    if (!find) {
        return contents;
    }
    do {
        const std::wstring_view name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            contents.subfolders.push_back(folder + L"\\" + data.cFileName);
        } else {
            contents.has_files = true;
        }
    } while (::FindNextFileW(find.get(), &data));
    return contents;
}

}  // namespace

// BCU InstallLocationScanner + JunkCreatorBase.GetJunkNodeFromLocation.
void scan_install_location(const ScanContext& context, std::vector<Leftover>& found) {
    const auto& target = context.target;
    const std::wstring& location = target.install_location;
    if (location.empty()) {
        return;
    }
    const DWORD attributes = ::GetFileAttributesW(location.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return;
    }
    std::vector<Record> records{{Evidence::ExplicitConnection}};
    if (context.still_used(location)) {
        records.push_back({Evidence::DirectoryStillUsed});
    }
    if (target.store_app) {
        records.push_back({Evidence::StoreApp});
    }
    found.push_back(make_item(context, FileLocation{location, true}, std::move(records), Scanner::InstallLocation,
                              Category::Files));
}

// BCU CommonDriveJunkScanner.
void scan_common_drive(const ScanContext& context, std::vector<Leftover>& found) {
    const auto& folders = context.snapshots.folders;
    std::vector<std::pair<std::size_t, std::size_t>> matched;  // (folder node, index in found)
    std::unordered_set<std::wstring> matched_paths;
    for (std::size_t i = 0; i < folders.size(); ++i) {
        const auto& node = folders[i];
        if (node.known_folder || !context.in_scope(node.user)) {
            continue;
        }
        const std::wstring item_name(name_without_extension(node.name));
        auto records =
            name_confidence(context.target.names, item_name, node.parent_name, node.level, node.parent_known_folder);
        if (!any_positive(records)) {
            continue;
        }
        if (is_questionable_folder_name(item_name)) {
            records.push_back({Evidence::QuestionableDirectoryName});
        }
        if (context.still_used(node.path)) {
            records.push_back({Evidence::DirectoryStillUsed});
        }
        if (context.other_user(node.user)) {
            records.push_back({Evidence::OtherUserData});
        }
        matched.emplace_back(i, found.size());
        matched_paths.insert(platform::to_lower(node.path));
        found.push_back(make_item(context, FileLocation{node.path, true}, std::move(records), Scanner::CommonDrive,
                                  Category::Files));
    }

    // A folder with no files whose subfolders are all leftovers too will be empty once they're gone.
    // BCU only checks this for the levels it recurses from.
    for (const auto& [node_index, found_index] : matched) {
        const auto& node = folders[node_index];
        if (node.level > 1) {
            continue;
        }
        const auto contents = read_contents(node.path);
        if (contents.has_files) {
            continue;
        }
        const bool all_matched = std::all_of(contents.subfolders.begin(), contents.subfolders.end(),
                                             [&](const auto& path) { return matched_paths.count(platform::to_lower(path)); });
        if (all_matched) {
            found[found_index].records.push_back({Evidence::AllSubdirsMatched});
        }
    }

    // Similar names are judged among the leftovers found side by side in one folder.
    std::map<std::wstring, std::pair<std::vector<std::size_t>, std::vector<std::wstring>>> siblings;
    for (const auto& [node_index, found_index] : matched) {
        const auto& node = folders[node_index];
        auto& group = siblings[platform::to_lower(platform::parent_path(node.path))];
        group.first.push_back(found_index);
        group.second.emplace_back(name_without_extension(node.name));
    }
    for (const auto& [parent, group] : siblings) {
        test_for_similar_names(context, found, group.first, group.second);
    }
}

// BCU ShortcutJunk.
void scan_shortcuts(const ScanContext& context, std::vector<Leftover>& found) {
    const auto& target = context.target;
    const bool location_shared =
        !target.install_location.empty() &&
        std::any_of(context.installed.begin(), context.installed.end(), [&](const inventory::Program& program) {
            return platform::paths_equal(program.install_location, target.install_location);
        });
    for (const auto& shortcut : context.snapshots.shortcuts) {
        if (!context.in_scope(shortcut.user)) {
            continue;
        }
        std::vector<Record> records;
        if (!target.install_location.empty() && platform::path_is_under(shortcut.target, target.install_location)) {
            records.push_back({Evidence::PathReference, 0, ReferenceKind::ShortcutTarget, shortcut.target});
            if (location_shared) {
                records.push_back({Evidence::DirectoryStillUsed});
            }
        } else if (!target.uninstaller.empty() && platform::paths_equal(shortcut.target, target.uninstaller)) {
            records.push_back({Evidence::PathReference, 0, ReferenceKind::ShortcutTarget, shortcut.target});
        } else if (target.uninstaller_folder.empty() ||
                   !platform::path_is_under(shortcut.target, target.uninstaller_folder)) {
            continue;  // Points somewhere unrelated
        }
        const std::wstring name(name_without_extension(platform::last_path_component(shortcut.file)));
        for (auto& record : name_confidence(target.names, name, L"", 0, false)) {
            add_record(records, std::move(record));
        }
        if (records.empty()) {
            continue;
        }
        if (context.other_user(shortcut.user)) {
            records.push_back({Evidence::OtherUserData});
        }
        found.push_back(make_item(context, FileLocation{shortcut.file, false}, std::move(records), Scanner::Shortcuts,
                                  Category::Shortcuts));
    }
}

}  // namespace avel::leftovers::detail
