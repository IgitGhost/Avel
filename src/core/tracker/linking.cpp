#include "core/tracker/linking.h"

#include <algorithm>
#include <cwctype>
#include <string_view>

#include "core/inventory/msi_codes.h"
#include "core/inventory/name_matching.h"
#include "core/inventory/uninstall_entry.h"
#include "core/platform/command_line.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/uninstall/registration.h"

namespace avel::tracker {

namespace {

using inventory::Program;
using platform::RegistryAddress;

// Where Uninstall keys live, below the machine's SOFTWARE and a user's Software (the snapshot
// reports both with WOW6432Node spelled out).
constexpr std::wstring_view kUninstallRoots[] = {
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)",
    LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall)",
};

std::vector<std::wstring_view> components(std::wstring_view path) {
    std::vector<std::wstring_view> parts;
    while (!path.empty()) {
        const std::size_t separator = path.find(L'\\');
        parts.push_back(path.substr(0, separator));
        if (separator == std::wstring_view::npos) {
            break;
        }
        path.remove_prefix(separator + 1);
    }
    return parts;
}

// `path` is `ancestor` or lies below it, ignoring case.
bool key_path_is_under(std::wstring_view path, std::wstring_view ancestor) {
    return platform::istarts_with(path, ancestor) &&
           (path.size() == ancestor.size() || (path[ancestor.size()] == L'\\' && !ancestor.empty()));
}

bool key_is_under(const RegistryAddress& key, const RegistryAddress& ancestor) {
    return key.hive == ancestor.hive && platform::iequals(key.sid, ancestor.sid) &&
           key_path_is_under(key.path, ancestor.path);
}

// The Uninstall key (an entry: <root>\<name>) that `key` is or lies in.
std::optional<RegistryAddress> entry_of(const RegistryAddress& key) {
    for (const auto root : kUninstallRoots) {
        if (key.path.size() > root.size() + 1 && key_path_is_under(key.path, root)) {
            const std::size_t end = key.path.find(L'\\', root.size() + 1);
            RegistryAddress entry = key;
            entry.path = key.path.substr(0, end);
            return entry;
        }
    }
    return std::nullopt;
}

// `key` is an Uninstall root or holds one.
bool holds_uninstall_root(const RegistryAddress& key) {
    return std::any_of(std::begin(kUninstallRoots), std::end(kUninstallRoots),
                       [&](std::wstring_view root) { return key_path_is_under(root, key.path); });
}

// The product code of Windows Installer's own keys for a product: ...\Installer\Products\<packed>,
// ...\Installer\Features\<packed> and ...\Installer\UserData\<sid>\Products\<packed>.
std::optional<std::wstring> registered_product(const RegistryAddress& key) {
    const auto parts = components(key.path);
    bool installer = false;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        installer = installer || platform::iequals(parts[i], L"Installer");
        if (installer &&
            (platform::iequals(parts[i], L"Products") || platform::iequals(parts[i], L"Features"))) {
            if (auto code = inventory::unpack_guid(parts[i + 1])) {
                return code;
            }
        }
    }
    return std::nullopt;
}

// An Uninstall key named after its product code, as Windows Installer names them.
std::optional<std::wstring> entry_product(const RegistryAddress& entry) {
    const std::wstring_view name = platform::last_path_component(entry.path);
    if (name.size() != 38) {
        return std::nullopt;
    }
    return inventory::extract_guid(name);
}

void add_key(std::vector<RegistryAddress>& keys, const RegistryAddress& key) {
    if (std::none_of(keys.begin(), keys.end(),
                     [&](const RegistryAddress& known) { return platform::same_address(known, key); })) {
        keys.push_back(key);
    }
}

// Product codes are hex digits and punctuation.
std::wstring upper(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return std::towupper(c); });
    return text;
}

void add_code(std::vector<std::wstring>& codes, std::wstring code) {
    code = upper(std::move(code));
    if (std::find(codes.begin(), codes.end(), code) == codes.end()) {
        codes.push_back(std::move(code));
    }
}

std::wstring product_code_of(const Program& program) {
    if (!program.msi_product_code.empty()) {
        return upper(program.msi_product_code);
    }
    if (program.windows_installer && program.registry) {
        return inventory::extract_guid(program.registry->key_name).value_or(L"");
    }
    return {};
}

// Whose program it is: the owner's SID for a per-user entry, empty for a machine-wide one.
std::wstring owner_of(const Program& program) {
    return program.scope == inventory::Scope::User ? program.user_sid : std::wstring();
}

std::wstring owner_of(const TraceLink& link) {
    return link.uninstall_key && link.uninstall_key->hive == platform::Hive::User ? link.uninstall_key->sid
                                                                                  : std::wstring();
}

const Program* find_program(const inventory::Inventory& inventory, const auto& matches) {
    for (const auto& program : inventory.programs) {
        if (matches(program)) {
            return &program;
        }
    }
    return nullptr;
}

// Step 4 of resolve_link: an entry an upgrade may have moved the program to.
const Program* moved_program(const TraceLink& link, const inventory::Inventory& inventory) {
    const std::wstring name = inventory::strip_version_number(link.name);
    const std::wstring publisher = inventory::trim_publisher(link.publisher);
    if (name.empty()) {
        return nullptr;
    }
    std::vector<const Program*> found;
    for (const auto& program : inventory.programs) {
        if (program.source == inventory::Source::Appx ||
            !platform::iequals(inventory::strip_version_number(program.display_name), name) ||
            !platform::iequals(owner_of(program), owner_of(link))) {
            continue;
        }
        const bool same_publisher =
            !publisher.empty() && platform::iequals(inventory::trim_publisher(program.publisher), publisher);
        const bool same_folder = !link.install_location.empty() && !program.install_location.empty() &&
                                 platform::paths_equal(program.install_location, link.install_location);
        if (same_publisher || same_folder) {
            found.push_back(&program);
        }
    }
    if (found.size() == 1) {
        return found.front();
    }
    std::erase_if(found, [](const Program* program) { return !inventory::is_listed_in_settings(*program); });
    return found.size() == 1 ? found.front() : nullptr;
}

// What the link says about the program, brought up to date; how it was found and whether it's
// primary stay.
void update_link(TraceLink& link, const Program& program) {
    TraceLink updated = make_link(program, link.matched_by);
    updated.primary = link.primary;
    link = std::move(updated);
}

// Where an entry's files are, as far as it says: its install folder, its uninstaller and its icon,
// normalized (only absolute paths). Many entries don't record their folder (Notepad++'s doesn't);
// their uninstaller and icon still tell.
std::vector<std::wstring> files_of(const Program& program) {
    std::vector<std::wstring> places;
    const auto add = [&](std::wstring_view path) {
        path = platform::trim(path);
        if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
            path = path.substr(1, path.size() - 2);
        }
        if (platform::is_absolute_path(path)) {
            places.push_back(platform::normalize_path(path));
        }
    };
    add(program.install_location);
    if (!program.uninstall_string.empty()) {
        add(platform::split_command_line(program.uninstall_string).executable);
    }
    std::wstring_view icon = program.display_icon;
    if (const std::size_t comma = icon.find_last_of(L',');
        comma != std::wstring_view::npos && icon.find_first_of(L"\\/", comma) == std::wstring_view::npos) {
        icon = icon.substr(0, comma);  // "path,index"
    }
    add(icon);
    return places;
}

void mark_primary(std::vector<TraceLink>& links) {
    for (std::size_t i = 0; i < links.size(); ++i) {
        links[i].primary = i == 0;
    }
}

}  // namespace

LinkCandidates link_candidates(const TraceResult& result) {
    LinkCandidates candidates;
    for (const auto& change : result.changes) {
        if (change.noise != NoiseReason::None || change.change == ChangeKind::Renamed) {
            continue;
        }
        // A created key, or the key of a value created, changed or deleted. A deleted key names
        // nothing that is still there.
        const RegistryAddress* key = nullptr;
        bool created_key = false;
        if (const auto* folder = std::get_if<leftovers::FileLocation>(&change.location)) {
            if (folder->directory && change.change == ChangeKind::Created) {
                candidates.created_folders.push_back(platform::normalize_path(folder->path));
            }
            continue;
        }
        if (const auto* item = std::get_if<leftovers::RegistryKeyLocation>(&change.location)) {
            if (change.change == ChangeKind::Deleted) {
                continue;
            }
            key = &item->key;
            created_key = change.change == ChangeKind::Created;
        } else if (const auto* value = std::get_if<leftovers::RegistryValueLocation>(&change.location)) {
            key = &value->key;
        } else {
            continue;
        }
        if (const auto entry = entry_of(*key)) {
            // The entry itself created, or something in it (a value, a subkey) changed.
            if (created_key && entry->path.size() == key->path.size()) {
                add_key(candidates.created_keys, *entry);
                if (auto code = entry_product(*entry)) {
                    add_code(candidates.product_codes, std::move(*code));
                }
            } else {
                add_key(candidates.changed_keys, *entry);
            }
        } else if (created_key && holds_uninstall_root(*key)) {
            add_key(candidates.created_keys, *key);
        }
        if (auto code = registered_product(*key)) {
            add_code(candidates.product_codes, std::move(*code));
        }
    }
    return candidates;
}

std::optional<RegistryAddress> uninstall_key_of(const Program& program) {
    if (!program.registry) {
        return std::nullopt;
    }
    const auto address = uninstall::uninstall_key_address(*program.registry, program.user_sid);
    if (!address) {
        return std::nullopt;
    }
    if (address->root == HKEY_USERS) {
        const std::size_t separator = address->path.find(L'\\');
        if (separator == std::wstring::npos) {
            return std::nullopt;
        }
        return RegistryAddress{platform::Hive::User, address->path.substr(0, separator),
                               address->path.substr(separator + 1)};
    }
    // Spelled the way the trace reports HKLM's keys; the 32-bit view is WOW6432Node below SOFTWARE.
    std::wstring path = address->path;
    constexpr std::wstring_view kSoftware = LR"(Software\)";
    if (platform::istarts_with(path, kSoftware)) {
        path = (address->view == KEY_WOW64_32KEY ? LR"(SOFTWARE\WOW6432Node\)" : LR"(SOFTWARE\)") +
               path.substr(kSoftware.size());
    }
    return RegistryAddress{platform::Hive::Machine, L"", std::move(path)};
}

TraceLink make_link(const Program& program, LinkMatch matched_by) {
    TraceLink link;
    link.program_id = program.id;
    link.name = program.display_name;
    link.publisher = program.publisher;
    link.version = program.display_version;
    link.uninstall_key = uninstall_key_of(program);
    link.product_code = product_code_of(program);
    if (platform::is_absolute_path(program.install_location)) {
        link.install_location = platform::normalize_path(program.install_location);
    }
    link.matched_by = matched_by;
    return link;
}

std::vector<TraceLink> find_links(const LinkCandidates& candidates, const inventory::Inventory& inventory) {
    struct Found {
        const Program* program;
        LinkMatch matched_by;
        std::size_t order;
        bool laid_down = false;  // Its folder, uninstaller or icon is in a folder the trace created
    };
    const auto laid_down = [&](const Program& program) {
        const auto places = files_of(program);
        return std::any_of(places.begin(), places.end(), [&](const std::wstring& place) {
            return std::any_of(candidates.created_folders.begin(), candidates.created_folders.end(),
                               [&](const std::wstring& created) {
                                   return platform::normalized_path_is_under(place, created);
                               });
        });
    };
    std::vector<Found> found;
    for (std::size_t i = 0; i < inventory.programs.size(); ++i) {
        const Program& program = inventory.programs[i];
        if (program.source == inventory::Source::Appx) {
            continue;
        }
        std::optional<LinkMatch> matched_by;
        if (const auto key = uninstall_key_of(program)) {
            if (std::any_of(candidates.created_keys.begin(), candidates.created_keys.end(),
                            [&](const RegistryAddress& created) { return key_is_under(*key, created); })) {
                matched_by = LinkMatch::CreatedKey;
            } else if (std::any_of(candidates.changed_keys.begin(), candidates.changed_keys.end(),
                                   [&](const RegistryAddress& changed) {
                                       return platform::same_address(*key, changed);
                                   })) {
                matched_by = LinkMatch::ChangedKey;
            }
        }
        if (!matched_by) {
            const std::wstring code = product_code_of(program);
            if (!code.empty() && std::find(candidates.product_codes.begin(), candidates.product_codes.end(),
                                           code) != candidates.product_codes.end()) {
                matched_by = LinkMatch::ProductCode;
            }
        }
        if (matched_by) {
            found.push_back({&program, *matched_by, i, laid_down(program)});
        }
    }
    // Listed in Settings first; then an entry whose files the installer laid down (not one that
    // only registered alongside, such as a compatibility database Windows installed meanwhile);
    // then by how it was found.
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        const bool listed_a = inventory::is_listed_in_settings(*a.program);
        const bool listed_b = inventory::is_listed_in_settings(*b.program);
        if (listed_a != listed_b) {
            return listed_a;
        }
        if (a.laid_down != b.laid_down) {
            return a.laid_down;
        }
        return a.matched_by < b.matched_by;
    });
    std::vector<TraceLink> links;
    for (const auto& item : found) {
        links.push_back(make_link(*item.program, item.matched_by));
    }
    mark_primary(links);
    return links;
}

const Program* resolve_link(const TraceLink& link, const inventory::Inventory& inventory) {
    if (!link.program_id.empty()) {
        if (const auto* program = find_program(inventory, [&](const Program& candidate) {
                return platform::iequals(candidate.id, link.program_id);
            })) {
            return program;
        }
    }
    if (link.uninstall_key) {
        if (const auto* program = find_program(inventory, [&](const Program& candidate) {
                const auto key = uninstall_key_of(candidate);
                return key && platform::same_address(*key, *link.uninstall_key);
            })) {
            return program;
        }
    }
    if (!link.product_code.empty()) {
        if (const auto* program = find_program(inventory, [&](const Program& candidate) {
                return platform::iequals(product_code_of(candidate), link.product_code);
            })) {
            return program;
        }
    }
    return moved_program(link, inventory);
}

std::optional<ResolvedProgram> resolve_trace_program(const TraceInfo& info,
                                                     const inventory::Inventory& inventory) {
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < info.links.size(); ++i) {
        if (info.links[i].primary) {
            order.insert(order.begin(), i);
        } else {
            order.push_back(i);
        }
    }
    for (const std::size_t i : order) {
        if (const auto* program = resolve_link(info.links[i], inventory)) {
            return ResolvedProgram{i, program};
        }
    }
    return std::nullopt;
}

bool refresh_links(TraceInfo& info, const inventory::Inventory& inventory) {
    bool installed = false;
    for (auto& link : info.links) {
        if (const auto* program = resolve_link(link, inventory)) {
            update_link(link, *program);
            installed = true;
        }
    }
    return installed;
}

std::vector<std::wstring> traces_of(const Program& program, std::span<const TraceInfo> traces,
                                    const inventory::Inventory& inventory) {
    std::vector<const TraceInfo*> found;
    for (const auto& info : traces) {
        if (info.state != TraceState::Complete && info.state != TraceState::Incomplete) {
            continue;
        }
        if (std::any_of(info.links.begin(), info.links.end(), [&](const TraceLink& link) {
                const auto* linked = resolve_link(link, inventory);
                return linked && platform::iequals(linked->id, program.id);
            })) {
            found.push_back(&info);
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const TraceInfo* a, const TraceInfo* b) {
        return a->started != b->started ? a->started > b->started : a->id > b->id;
    });
    std::vector<std::wstring> ids;
    for (const auto* info : found) {
        ids.push_back(info->id);
    }
    return ids;
}

std::unordered_map<std::wstring, std::size_t> program_traces(std::span<const TraceInfo> traces,
                                                             const inventory::Inventory& inventory) {
    // Newer as traces_of orders them.
    const auto newer = [&](std::size_t a, std::size_t b) {
        return traces[a].started != traces[b].started ? traces[a].started > traces[b].started
                                                      : traces[a].id > traces[b].id;
    };
    std::unordered_map<std::wstring, std::size_t> found;
    for (std::size_t i = 0; i < traces.size(); ++i) {
        const auto& info = traces[i];
        if (info.state != TraceState::Complete && info.state != TraceState::Incomplete) {
            continue;
        }
        for (const auto& link : info.links) {
            if (const auto* program = resolve_link(link, inventory)) {
                const auto [entry, added] = found.try_emplace(program->id, i);
                if (!added && entry->second != i && newer(i, entry->second)) {
                    entry->second = i;
                }
            }
        }
    }
    return found;
}

void set_links(TraceInfo& info, const TraceResult& result, const inventory::Inventory& inventory) {
    std::vector<TraceLink> links;
    for (const auto& link : info.links) {
        if (link.matched_by == LinkMatch::Manual) {
            links.push_back(link);
        }
    }
    for (auto& link : find_links(link_candidates(result), inventory)) {
        if (std::none_of(links.begin(), links.end(), [&](const TraceLink& kept) {
                return platform::iequals(kept.program_id, link.program_id);
            })) {
            links.push_back(std::move(link));
        }
    }
    // A manual link was primary before; the automatic ones follow it.
    std::stable_sort(links.begin(), links.end(),
                     [](const TraceLink& a, const TraceLink& b) { return a.primary && !b.primary; });
    mark_primary(links);
    info.links = std::move(links);
    if (resolve_trace_program(info, inventory)) {
        info.unresolved_since.reset();
    }
}

void add_manual_link(TraceInfo& info, const Program& program) {
    std::erase_if(info.links,
                  [&](const TraceLink& link) { return platform::iequals(link.program_id, program.id); });
    TraceLink link = make_link(program, LinkMatch::Manual);
    info.links.insert(info.links.begin(), std::move(link));
    mark_primary(info.links);
    info.unresolved_since.reset();
}

void relink(TraceInfo& info, const TraceResult& result, const inventory::Inventory& inventory) {
    refresh_links(info, inventory);
    for (auto& link : find_links(link_candidates(result), inventory)) {
        if (std::none_of(info.links.begin(), info.links.end(), [&](const TraceLink& kept) {
                return platform::iequals(kept.program_id, link.program_id);
            })) {
            link.primary = false;
            info.links.push_back(std::move(link));
        }
    }
    if (std::none_of(info.links.begin(), info.links.end(),
                     [](const TraceLink& link) { return link.primary; })) {
        mark_primary(info.links);
    }
    info.unresolved_since.reset();
}

std::optional<TraceInfo> link_trace(TraceStore& store, const std::wstring& id, const TraceResult& result,
                                    const inventory::Inventory& inventory, TraceError& error) {
    return store.update(
        id,
        [&](TraceInfo& info) {
            set_links(info, result, inventory);
            return true;
        },
        error);
}

}  // namespace avel::tracker
