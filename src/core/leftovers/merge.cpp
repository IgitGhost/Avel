#include "core/leftovers/merge.h"

#include <algorithm>
#include <string>
#include <unordered_map>

#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::leftovers {

namespace {

// Orders paths as a tree: a separator sorts before any other character, so "C:\A\B" follows
// "C:\A" directly instead of coming after "C:\A2".
bool tree_order(const std::wstring& a, const std::wstring& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](wchar_t x, wchar_t y) {
        const wchar_t left = x == L'\\' ? L'\x01' : x;
        const wchar_t right = y == L'\\' ? L'\x01' : y;
        return left < right;
    });
}

}  // namespace

void merge_duplicates(std::vector<Leftover>& leftovers) {
    std::vector<Leftover> merged;
    std::unordered_map<std::wstring, std::size_t> index;
    for (auto& item : leftovers) {
        const std::wstring key = identity(item.location);
        const auto found = index.find(key);
        if (found == index.end()) {
            index.emplace(key, merged.size());
            merged.push_back(std::move(item));
            continue;
        }
        Leftover& target = merged[found->second];
        for (auto& record : item.records) {
            add_record(target.records, std::move(record));
        }
        for (auto& owner : item.owner_ids) {
            const bool known = std::any_of(target.owner_ids.begin(), target.owner_ids.end(),
                                           [&](const auto& id) { return platform::iequals(id, owner); });
            if (!known) {
                target.owner_ids.push_back(std::move(owner));
            }
        }
    }
    leftovers = std::move(merged);
}

void link_containment(std::vector<Leftover>& leftovers) {
    std::vector<std::pair<std::wstring, std::size_t>> order;
    order.reserve(leftovers.size());
    for (std::size_t i = 0; i < leftovers.size(); ++i) {
        order.emplace_back(identity(leftovers[i].location), i);
    }
    std::sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
        const auto ca = leftovers[a.second].category;
        const auto cb = leftovers[b.second].category;
        return ca != cb ? ca < cb : tree_order(a.first, b.first);
    });
    std::vector<Leftover> sorted;
    sorted.reserve(leftovers.size());
    for (const auto& [key, i] : order) {
        sorted.push_back(std::move(leftovers[i]));
    }
    leftovers = std::move(sorted);

    // Containers by identity; each item then looks its ancestors up, nearest first. A scan of
    // every program finds tens of thousands of items, too many to compare pairwise.
    std::unordered_map<std::wstring, std::size_t> containers;
    for (std::size_t i = 0; i < leftovers.size(); ++i) {
        const auto& location = leftovers[i].location;
        const auto* folder = std::get_if<FileLocation>(&location);
        if ((folder && folder->directory) || std::holds_alternative<RegistryKeyLocation>(location)) {
            containers.emplace(identity(location), i);
        }
    }
    const auto find = [&](const Location& location) -> std::optional<std::size_t> {
        const auto found = containers.find(identity(location));
        return found == containers.end() ? std::nullopt : std::optional<std::size_t>(found->second);
    };
    const auto nearest_key = [&](platform::RegistryAddress key, bool include_self) -> std::optional<std::size_t> {
        if (include_self) {
            if (const auto found = find(RegistryKeyLocation{key})) {
                return found;
            }
        }
        for (size_t separator = key.path.rfind(L'\\'); separator != std::wstring::npos;
             separator = key.path.rfind(L'\\')) {
            key.path.resize(separator);
            if (const auto found = find(RegistryKeyLocation{key})) {
                return found;
            }
        }
        return std::nullopt;
    };

    for (auto& item : leftovers) {
        if (const auto* file = std::get_if<FileLocation>(&item.location)) {
            std::wstring path = platform::normalize_path(file->path);
            for (std::wstring parent = platform::parent_path(path); !parent.empty() && parent != path;
                 parent = platform::parent_path(path)) {
                path = std::move(parent);
                if ((item.inside = find(FileLocation{path, true}))) {
                    break;
                }
            }
        } else if (const auto* key = std::get_if<RegistryKeyLocation>(&item.location)) {
            item.inside = nearest_key(key->key, false);
        } else if (const auto* value = std::get_if<RegistryValueLocation>(&item.location)) {
            item.inside = nearest_key(value->key, true);
        }
    }
}

}  // namespace avel::leftovers
