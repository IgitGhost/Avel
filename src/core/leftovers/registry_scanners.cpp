// Ported in part from Bulk Crap Uninstaller, Copyright (c) 2017 Marcin Szeniak, Apache License 2.0.
// See scanners.h for the original source files.
#include <algorithm>
#include <map>

#include "core/leftovers/product_name.h"
#include "core/leftovers/scanners.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::leftovers::detail {

namespace {

Leftover make_item(const ScanContext& context, Location location, std::vector<Record> records, Scanner scanner) {
    Leftover item;
    item.location = std::move(location);
    item.records = std::move(records);
    item.owner_ids = {context.target.program_id};
    item.found_by = scanner;
    item.category = Category::Registry;
    return item;
}

// BCU SoftwareRegKeyScanner.TestValueForMatches: one of the key's path values names the
// program's folder or something in it.
bool names_install_location(const KeyNode& node, std::wstring_view location) {
    const auto inside = [&](const std::wstring& path) { return platform::path_is_under(path, location); };
    const auto file_inside = [&](const std::wstring& path) {
        const std::wstring folder = platform::parent_path(path);
        return !folder.empty() && platform::path_is_under(folder, location);
    };
    return std::any_of(node.folder_values.begin(), node.folder_values.end(), inside) ||
           std::any_of(node.file_values.begin(), node.file_values.end(), file_inside) ||
           std::any_of(node.file_or_folder_values.begin(), node.file_or_folder_values.end(), inside) ||
           (!node.default_value.empty() && inside(node.default_value));
}

}  // namespace

// BCU SoftwareRegKeyScanner, including ScanRelatedKeys.
void scan_software_keys(const ScanContext& context, std::vector<Leftover>& found) {
    const auto& target = context.target;
    const auto& keys = context.snapshots.keys;
    std::vector<std::pair<std::size_t, std::size_t>> matched;  // (key node, index in found)
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto& node = keys[i];
        if (!context.in_scope(node.user)) {
            continue;
        }
        auto records = name_confidence(target.names, node.name, node.parent_name, node.level, false);
        if (!target.install_location.empty() && names_install_location(node, target.install_location)) {
            records.push_back({Evidence::ExplicitConnection});
        }
        if (records.empty()) {
            continue;
        }
        if (context.other_user(node.user)) {
            records.push_back({Evidence::OtherUserData});
        }
        matched.emplace_back(i, found.size());
        found.push_back(make_item(context, RegistryKeyLocation{node.address}, std::move(records), Scanner::SoftwareKey));
    }

    // Similar names are judged among the keys found side by side under one parent key.
    std::map<std::wstring, std::pair<std::vector<std::size_t>, std::vector<std::wstring>>> siblings;
    for (const auto& [node_index, found_index] : matched) {
        const auto& node = keys[node_index];
        const std::wstring path = platform::display_path(node.address);
        auto& group = siblings[platform::to_lower(path.substr(0, path.find_last_of(L'\\')))];
        group.first.push_back(found_index);
        group.second.push_back(node.name);
    }
    for (const auto& [parent, group] : siblings) {
        test_for_similar_names(context, found, group.first, group.second);
    }

    // The same key under the other roots (the 32-bit view, another user's hive, the virtual store)
    // most likely belongs to the program too; it shares the evidence.
    const auto& roots = context.snapshots.key_roots;
    std::map<std::size_t, std::size_t> found_by_node;
    for (const auto& [node_index, found_index] : matched) {
        found_by_node.emplace(node_index, found_index);
    }
    const std::size_t original_count = matched.size();
    for (std::size_t m = 0; m < original_count; ++m) {
        const auto [node_index, found_index] = matched[m];
        const auto& node = keys[node_index];
        for (std::size_t root = 0; root < roots.size(); ++root) {
            if (root == node.root || !context.in_scope(roots[root].user)) {
                continue;
            }
            const auto mirror = context.snapshots.key_index.find(key_index_name(root, node.relative_path));
            if (mirror == context.snapshots.key_index.end()) {
                continue;
            }
            std::vector<Record> shared;
            for (const auto& record : found[found_index].records) {
                if (record.evidence != Evidence::OtherUserData) {
                    shared.push_back(record);
                }
            }
            if (context.other_user(roots[root].user)) {
                shared.push_back({Evidence::OtherUserData});
            }
            const auto existing = found_by_node.find(mirror->second);
            if (existing != found_by_node.end()) {
                for (auto& record : shared) {
                    add_record(found[existing->second].records, std::move(record));
                }
                continue;
            }
            found_by_node.emplace(mirror->second, found.size());
            found.push_back(make_item(context, RegistryKeyLocation{keys[mirror->second].address}, std::move(shared),
                                      Scanner::SoftwareKey));
        }
    }
}

// The program's own Uninstall key, if its uninstaller left it behind.
void scan_uninstaller_key(const ScanContext& context, std::vector<Leftover>& found) {
    const auto& key = context.target.uninstall_key;
    if (!key) {
        return;
    }
    platform::RegistryKey handle;
    if (open_key(context.environment, *key, handle) != ERROR_SUCCESS) {
        return;
    }
    found.push_back(make_item(context, RegistryKeyLocation{*key}, {{Evidence::UninstallerKey}}, Scanner::UninstallerKey));
}

// Run values, App Paths, SharedDLLs, Windows Installer's folder list and MuiCache entries that
// point into the program's folder.
void scan_references(const ScanContext& context, std::vector<Leftover>& found) {
    const auto& location = context.target.install_location;
    if (location.empty()) {
        return;
    }
    for (const auto& reference : context.snapshots.references) {
        if (!context.in_scope(reference.user)) {
            continue;
        }
        const auto path = std::find_if(reference.paths.begin(), reference.paths.end(),
                                       [&](const auto& item) { return platform::path_is_under(item, location); });
        if (path == reference.paths.end()) {
            continue;
        }
        std::vector<Record> records;
        if (reference.kind == ReferenceKind::RunValue) {
            records.push_back({Evidence::StartupMatched, 0, reference.kind, *path});
            if (reference.run_once) {
                records.push_back({Evidence::StartupIsRunOnce});
            }
        } else {
            records.push_back({Evidence::PathReference, 0, reference.kind, *path});
        }
        if (context.other_user(reference.user)) {
            records.push_back({Evidence::OtherUserData});
        }
        found.push_back(make_item(context, reference.location, std::move(records), Scanner::PathReferences));
    }
}

// Packages registered with the program's folder as their external location, such as the
// sparse package a classic app adds for its Windows 11 context-menu handler.
void scan_related_packages(const ScanContext& context, std::vector<Leftover>& found) {
    const auto& location = context.target.install_location;
    if (location.empty()) {
        return;
    }
    for (const auto& program : context.installed) {
        if (program.source != inventory::Source::Appx || program.external_location.empty() || program.no_remove ||
            !platform::path_is_under(program.external_location, location)) {
            continue;
        }
        const std::wstring sid = program.user_sid.empty() ? context.environment.current_user_sid : program.user_sid;
        Leftover item;
        item.location = PackageLocation{program.package_full_name, sid};
        item.records = {{Evidence::PathReference, 0, ReferenceKind::PackageExternalLocation, program.external_location}};
        item.owner_ids = {context.target.program_id};
        item.found_by = Scanner::RelatedPackages;
        item.category = Category::Packages;
        found.push_back(std::move(item));
    }
}

}  // namespace avel::leftovers::detail
