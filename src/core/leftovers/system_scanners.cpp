// Services, scheduled tasks, COM classes and firewall rules that point into a program's folder.
#include <algorithm>
#include <unordered_set>

#include "core/leftovers/scanners.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::leftovers::detail {

namespace {

// The first of `paths` inside the program's folder, if any. Snapshot paths and the target's
// folder are normalized already.
const std::wstring* path_inside(const std::vector<std::wstring>& paths, std::wstring_view location) {
    const auto found = std::find_if(paths.begin(), paths.end(), [&](const auto& path) {
        return platform::normalized_path_is_under(path, location);
    });
    return found == paths.end() ? nullptr : &*found;
}

Leftover make_item(const ScanContext& context, Location location, Record record, Scanner scanner, Category category,
                   const UserScope* user = nullptr) {
    Leftover item;
    item.location = std::move(location);
    item.records = {std::move(record)};
    if (context.other_user(user)) {
        item.records.push_back({Evidence::OtherUserData});
    }
    item.owner_ids = {context.target.program_id};
    item.found_by = scanner;
    item.category = category;
    return item;
}

}  // namespace

void scan_services(const ScanContext& context, std::vector<Leftover>& found) {
    const auto& location = context.target.install_location;
    if (location.empty()) {
        return;
    }
    for (const auto& service : context.snapshots.services) {
        if (const auto* path = path_inside(service.paths, location)) {
            found.push_back(make_item(context, ServiceLocation{service.name},
                                      {Evidence::StartupMatched, 0, ReferenceKind::ServiceImage, *path},
                                      Scanner::Services, Category::Services));
        }
    }
}

void scan_tasks(const ScanContext& context, std::vector<Leftover>& found) {
    const auto& location = context.target.install_location;
    if (location.empty()) {
        return;
    }
    for (const auto& task : context.snapshots.tasks) {
        if (const auto* path = path_inside(task.paths, location)) {
            found.push_back(make_item(context, TaskLocation{task.path},
                                      {Evidence::StartupMatched, 0, ReferenceKind::TaskAction, *path}, Scanner::Tasks,
                                      Category::Tasks));
        }
    }
}

// A class whose server is in the program's folder, and the shell extension registrations that
// name it (what keeps a removed program's entries in Explorer's context menu).
void scan_com_classes(const ScanContext& context, std::vector<Leftover>& found) {
    const auto& location = context.target.install_location;
    if (location.empty()) {
        return;
    }
    std::unordered_set<std::wstring> classes;
    for (const auto& com_class : context.snapshots.com_classes) {
        if (!context.in_scope(com_class.user)) {
            continue;
        }
        if (const auto* path = path_inside(com_class.paths, location)) {
            classes.insert(platform::to_lower(com_class.clsid));
            found.push_back(make_item(context, RegistryKeyLocation{com_class.key},
                                      {Evidence::PathReference, 0, ReferenceKind::ComServer, *path},
                                      Scanner::ComClasses, Category::Registry, com_class.user));
        }
    }
    if (classes.empty()) {
        return;
    }
    for (const auto& reference : context.snapshots.class_references) {
        if (context.in_scope(reference.user) && classes.count(platform::to_lower(reference.clsid))) {
            found.push_back(make_item(context, reference.location,
                                      {Evidence::PathReference, 0, ReferenceKind::ComServer, reference.clsid},
                                      Scanner::ComClasses, Category::Registry, reference.user));
        }
    }
}

void scan_firewall_rules(const ScanContext& context, std::vector<Leftover>& found) {
    const auto& location = context.target.install_location;
    if (location.empty()) {
        return;
    }
    for (const auto& rule : context.snapshots.firewall_rules) {
        if (platform::normalized_path_is_under(rule.program, location)) {
            found.push_back(make_item(context, FirewallRuleLocation{rule.id},
                                      {Evidence::PathReference, 0, ReferenceKind::FirewallApp, rule.program},
                                      Scanner::Firewall, Category::Firewall));
        }
    }
}

}  // namespace avel::leftovers::detail
