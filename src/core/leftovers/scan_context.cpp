// Ported in part from Bulk Crap Uninstaller, Copyright (c) 2017 Marcin Szeniak, Apache License 2.0.
// See scanners.h for the original source files.
#include <algorithm>
#include <numeric>

#include "core/inventory/name_matching.h"
#include "core/leftovers/scanners.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::leftovers::detail {

bool ScanContext::in_scope(const UserScope* user) const {
    if (!user || target.scope == inventory::Scope::Machine) {
        return true;
    }
    return platform::iequals(user->profile.sid, target.user_sid);
}

bool ScanContext::other_user(const UserScope* user) const {
    return user && target.scope == inventory::Scope::Machine &&
           !platform::iequals(user->profile.sid, environment.current_user_sid);
}

bool ScanContext::still_used(std::wstring_view folder) const {
    return std::any_of(other_locations.begin(), other_locations.end(),
                       [&](const auto& location) { return platform::path_is_under(location, folder); });
}

ScanContext make_context(const ScanEnvironment& environment, const Snapshots& snapshots, const ScanTarget& target,
                         std::span<const inventory::Program> installed) {
    ScanContext context{environment, snapshots, target, installed, {}, {}};
    for (const auto& program : installed) {
        if (!program.install_location.empty() && platform::is_absolute_path(program.install_location)) {
            context.other_locations.push_back(platform::normalize_path(program.install_location));
        }
        context.other_names.push_back(inventory::strip_version_number(program.display_name));
    }
    return context;
}

void test_for_similar_names(const ScanContext& context, std::vector<Leftover>& found,
                            const std::vector<std::size_t>& indices, const std::vector<std::wstring>& names) {
    if (names.empty()) {
        return;
    }
    // BCU compares these names case-sensitively.
    const std::wstring& this_name = context.target.names.name;
    std::vector<bool> flagged(names.size(), false);
    for (const auto& other : context.other_names) {
        if (other.find(this_name) != std::wstring::npos) {
            continue;  // Other programs whose names contain this one's don't count
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (other.find(names[i]) != std::wstring::npos) {
                flagged[i] = true;
            }
        }
    }
    if (names.size() >= 2) {
        // Of several names like "AppX" and "AppX Extended", only the closest match keeps a clean slate.
        std::vector<std::size_t> similar;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i].find(this_name) != std::wstring::npos || this_name.find(names[i]) != std::wstring::npos) {
                similar.push_back(i);
            }
        }
        std::stable_sort(similar.begin(), similar.end(), [&](std::size_t a, std::size_t b) {
            return inventory::sift4_distance(names[a], this_name, 100) <
                   inventory::sift4_distance(names[b], this_name, 100);
        });
        for (std::size_t i = 1; i < similar.size(); ++i) {
            flagged[similar[i]] = true;
        }
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (flagged[i]) {
            add_record(found[indices[i]].records, {Evidence::UsedBySimilarNamedApp});
        }
    }
}

}  // namespace avel::leftovers::detail
