// What a program's install trace recorded (Scanner::Trace; docs/leftovers.md, "Install traces"):
// the items the installer created, still there, and the edits that take back its changes to the
// environment and SharedDLLs. Which items are the program's own was settled by the tracker when the
// target was captured (ScanTarget::traced, TracedItem::tied); only those get strong evidence. A
// created folder or key that holds more now than the trace saw in it goes only as what it saw.
#include <windows.h>

#include <wil/resource.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "core/leftovers/product_name.h"
#include "core/leftovers/scanners.h"
#include "core/platform/file_ops.h"
#include "core/platform/nt_registry.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"

namespace avel::leftovers::detail {

namespace {

using platform::RawValue;
using platform::RegistryAddress;

constexpr wchar_t kServices[] = LR"(SYSTEM\CurrentControlSet\Services)";
// The Task Scheduler keeps a key here for every task and task folder.
constexpr wchar_t kTaskTree[] = LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tree)";
constexpr wchar_t kFirewallRules[] =
    LR"(SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\FirewallRules)";
constexpr const wchar_t* kSharedDllKeys[] = {
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)",
    LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\SharedDLLs)"};

RegistryAddress machine_key(std::wstring path) { return {platform::Hive::Machine, L"", std::move(path)}; }

bool same(const std::optional<RawValue>& a, const std::optional<RawValue>& b) {
    return a.has_value() == b.has_value() && (!a || (a->type == b->type && a->data == b->data));
}

bool planned(ValueEditBasis basis) {
    return basis == ValueEditBasis::Undo || basis == ValueEditBasis::Restore;
}

// Gone only when Windows says so: a key that can't be opened may still be there.
bool missing(LSTATUS status) { return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND; }

bool key_exists(const ScanEnvironment& environment, const RegistryAddress& address) {
    platform::RegistryKey key;
    return !missing(open_key(environment, address, key));
}

bool value_exists(const ScanEnvironment& environment, const RegistryAddress& address,
                  const std::wstring& name) {
    platform::RegistryKey key;
    if (const LSTATUS status = open_key(environment, address, key); status != ERROR_SUCCESS) {
        return !missing(status);
    }
    return !missing(::RegQueryValueExW(key.get(), name.c_str(), nullptr, nullptr, nullptr, nullptr));
}

// The item is still where the trace saw it created (a file is still a file, a folder a folder).
bool still_there(const ScanEnvironment& environment, const Location& location) {
    return std::visit(
        [&](const auto& item) -> bool {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, FileLocation>) {
                const std::wstring path = item.path.size() >= MAX_PATH && !item.path.starts_with(L"\\\\")
                                              ? LR"(\\?\)" + item.path
                                              : item.path;
                const DWORD attributes = ::GetFileAttributesW(path.c_str());
                return attributes != INVALID_FILE_ATTRIBUTES &&
                       ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == item.directory;
            } else if constexpr (std::is_same_v<T, RegistryKeyLocation>) {
                return key_exists(environment, item.key);
            } else if constexpr (std::is_same_v<T, RegistryValueLocation>) {
                return value_exists(environment, item.key, item.value_name);
            } else if constexpr (std::is_same_v<T, ServiceLocation>) {
                return key_exists(environment, machine_key(std::wstring(kServices) + L"\\" + item.name));
            } else if constexpr (std::is_same_v<T, TaskLocation>) {
                std::wstring_view path = item.path;
                while (path.starts_with(L'\\')) {
                    path.remove_prefix(1);
                }
                return !path.empty() && key_exists(environment, machine_key(std::wstring(kTaskTree) + L"\\" +
                                                                            std::wstring(path)));
            } else if constexpr (std::is_same_v<T, FirewallRuleLocation>) {
                return value_exists(environment, machine_key(kFirewallRules), item.rule_id);
            } else {
                return true;  // Packages and value edits: a trace records neither as an item
            }
        },
        location);
}

// The item's own name matches the program's exactly (or nearly, as the leftover scan's name match
// counts it perfect): a file's or folder's, with or without its extension, a key's, a value's, a
// service's or a task's.
bool named_like_program(const ProductNames& names, const Location& location) {
    std::wstring_view name;
    if (const auto* file = std::get_if<FileLocation>(&location)) {
        name = platform::last_path_component(file->path);
    } else if (const auto* key = std::get_if<RegistryKeyLocation>(&location)) {
        name = platform::last_path_component(key->key.path);
    } else if (const auto* value = std::get_if<RegistryValueLocation>(&location)) {
        name = value->value_name;
    } else if (const auto* service = std::get_if<ServiceLocation>(&location)) {
        name = service->name;
    } else if (const auto* task = std::get_if<TaskLocation>(&location)) {
        name = platform::last_path_component(task->path);
    }
    if (name.empty()) {
        return false;
    }
    const auto perfect = [&](std::wstring_view text) {
        const int match = match_product_name(names, text);
        return match == 0 || match == 1;
    };
    return perfect(name) || perfect(name_without_extension(name));
}

// Whose profile or hive the item is in; null for machine-wide items.
const UserScope* user_of(const ScanEnvironment& environment, const Location& location) {
    const std::wstring sid = owner_sid(location);
    const auto* file = std::get_if<FileLocation>(&location);
    for (const auto& user : environment.users) {
        const bool owner = !sid.empty() ? platform::iequals(user.profile.sid, sid)
                                        : file && !user.profile.profile_path.empty() &&
                                              platform::path_is_under(file->path, user.profile.profile_path);
        if (owner) {
            return &user;
        }
    }
    return nullptr;
}

// A DLL that keeps a SharedDLLs count above 0 once the planned edits are made: the file itself, or
// one inside the folder.
const TraceValuePlan::SharedDll* still_shared(const TraceValuePlan& values, const FileLocation& file) {
    const std::wstring path = platform::normalize_path(file.path);
    for (const auto& dll : values.shared_dlls) {
        if (dll.count > 0 && (file.directory ? platform::normalized_path_is_under(dll.path, path)
                                             : platform::iequals(dll.path, path))) {
            return &dll;
        }
    }
    return nullptr;
}

// --- Created containers -------------------------------------------------------------------------
//
// A folder or key the installer created may since hold what other programs or the user put in it
// (another program's shell extension in a DragDropHandlers key the installer made). It goes whole
// only while it holds no more than the trace saw in it; otherwise the items the trace saw in it go
// one by one, and with only a count to go by, it is offered whole below Good.

// One entry of a folder or key as it is now.
struct LiveEntry {
    std::wstring name;
    bool value = false;      // A registry value
    bool container = false;  // A folder or a subkey
    bool link = false;       // A folder that is a reparse point: not looked into
};

// A folder's entries; nullopt when it can't be listed.
std::optional<std::vector<LiveEntry>> list_folder(const std::wstring& folder) {
    WIN32_FIND_DATAW data;
    wil::unique_hfind find(::FindFirstFileExW(platform::extended(folder + L"\\*").c_str(), FindExInfoBasic, &data,
                                              FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
    if (!find) {
        return std::nullopt;  // An existing folder always lists "." at least
    }
    std::vector<LiveEntry> entries;
    do {
        const std::wstring_view name = data.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entries.push_back({std::wstring(name), false, directory,
                           directory && (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
    } while (::FindNextFileW(find.get(), &data));
    if (::GetLastError() != ERROR_NO_MORE_FILES) {
        return std::nullopt;
    }
    return entries;
}

// A key's values and subkeys; nullopt when it can't be read.
std::optional<std::vector<LiveEntry>> list_key(const ScanEnvironment& environment, const RegistryAddress& address) {
    platform::RegistryKey key;
    if (open_key(environment, address, key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::vector<RawValue> values;
    std::vector<platform::SubkeyInfo> subkeys;
    if (platform::read_raw_values(key.get(), values) != ERROR_SUCCESS ||
        platform::enum_subkeys(key.get(), subkeys) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::vector<LiveEntry> entries;
    for (auto& value : values) {
        entries.push_back({std::move(value.name), true, false, false});
    }
    for (auto& subkey : subkeys) {
        entries.push_back({std::move(subkey.name), false, true, false});
    }
    return entries;
}

std::wstring join(const std::wstring& path, std::wstring_view name) {
    return path.empty() ? std::wstring(name) : path + L"\\" + std::wstring(name);
}

// The trace's item that a live entry is, as a key into Inventory::children.
std::wstring entry_key(wchar_t kind, std::wstring_view name) { return kind + platform::to_lower(name); }
wchar_t kind_of(const LiveEntry& entry) { return entry.value ? L'v' : entry.container ? L'c' : L'f'; }

// An item to offer for a created container.
struct ContainerOffer {
    Location location;
    bool holds_more = false;  // Offered whole though it can't be told what it holds
};

// Compares a created folder or key with what the trace saw in it.
class ContainerCheck {
public:
    ContainerCheck(const ScanEnvironment& environment, const Location& container,
                   const std::vector<ContainedItem>& inside)
        : environment_(environment), container_(container) {
        for (const auto& item : inside) {
            if (item.value) {
                children_[platform::to_lower(item.path)].insert(entry_key(L'v', *item.value));
                continue;
            }
            const std::size_t separator = item.path.find_last_of(L'\\');
            const std::wstring parent = separator == std::wstring::npos ? L"" : item.path.substr(0, separator);
            const std::wstring_view name =
                separator == std::wstring::npos ? std::wstring_view(item.path)
                                                : std::wstring_view(item.path).substr(separator + 1);
            children_[platform::to_lower(parent)].insert(entry_key(item.container ? L'c' : L'f', name));
        }
    }

    struct Fit {
        bool exact = true;     // Holds nothing the trace didn't see: may go whole
        bool readable = true;  // Could be listed
        std::vector<ContainerOffer> offers;  // When not exact: what goes instead
    };

    // The container at `relative` (empty: the container itself).
    Fit fit(const std::wstring& relative) const {
        Fit result;
        const auto live = list(relative);
        if (!live) {
            result.exact = false;
            result.readable = false;
            return result;
        }
        const auto found = children_.find(platform::to_lower(relative));
        for (const auto& entry : *live) {
            if (found == children_.end() || !found->second.contains(entry_key(kind_of(entry), entry.name))) {
                result.exact = false;  // Someone else's
                continue;
            }
            if (entry.value) {
                result.offers.push_back({value_at(relative, entry.name)});
                continue;
            }
            const std::wstring path = join(relative, entry.name);
            if (!entry.container || entry.link) {
                result.offers.push_back({item_at(path, entry.container)});
                continue;
            }
            Fit inner = fit(path);
            if (inner.exact) {
                result.offers.push_back({item_at(path, true)});
            } else if (!inner.readable) {
                result.exact = false;
                result.offers.push_back({item_at(path, true), true});
            } else {
                result.exact = false;
                std::move(inner.offers.begin(), inner.offers.end(), std::back_inserter(result.offers));
            }
        }
        return result;
    }

private:
    std::optional<std::vector<LiveEntry>> list(const std::wstring& relative) const {
        if (const auto* folder = std::get_if<FileLocation>(&container_)) {
            return list_folder(relative.empty() ? folder->path : folder->path + L"\\" + relative);
        }
        const auto& key = std::get<RegistryKeyLocation>(container_).key;
        RegistryAddress address = key;
        address.path = join(key.path, relative);
        return list_key(environment_, address);
    }

    Location item_at(const std::wstring& relative, bool container) const {
        if (const auto* folder = std::get_if<FileLocation>(&container_)) {
            return FileLocation{folder->path + L"\\" + relative, container};
        }
        RegistryAddress address = std::get<RegistryKeyLocation>(container_).key;
        address.path += L"\\" + relative;
        return RegistryKeyLocation{std::move(address)};
    }

    Location value_at(const std::wstring& relative, const std::wstring& name) const {
        RegistryAddress address = std::get<RegistryKeyLocation>(container_).key;
        address.path = join(address.path, relative);
        return RegistryValueLocation{std::move(address), name};
    }

    const ScanEnvironment& environment_;
    const Location& container_;
    // By the folder or key they are directly in (lowercase, relative; "" for the container
    // itself): the items the trace saw, as entry_key names them.
    std::unordered_map<std::wstring, std::unordered_set<std::wstring>> children_;
};

// How many items a folder or key holds, all the way down, counting no further than `limit`;
// nullopt when part of it can't be read.
std::optional<std::uint64_t> count_live(const ScanEnvironment& environment, const Location& container,
                                        std::uint64_t limit) {
    std::uint64_t count = 0;
    std::vector<std::wstring> pending{L""};
    while (!pending.empty() && count <= limit) {
        const std::wstring relative = std::move(pending.back());
        pending.pop_back();
        std::optional<std::vector<LiveEntry>> live;
        if (const auto* folder = std::get_if<FileLocation>(&container)) {
            live = list_folder(relative.empty() ? folder->path : folder->path + L"\\" + relative);
        } else {
            RegistryAddress address = std::get<RegistryKeyLocation>(container).key;
            address.path = join(address.path, relative);
            live = list_key(environment, address);
        }
        if (!live) {
            return std::nullopt;
        }
        count += live->size();
        for (const auto& entry : *live) {
            if (entry.container && !entry.link) {
                pending.push_back(join(relative, entry.name));
            }
        }
    }
    return count;
}

bool holds_items(const Location& location) {
    const auto* folder = std::get_if<FileLocation>(&location);
    return (folder && folder->directory) || std::holds_alternative<RegistryKeyLocation>(location);
}

// The value exactly as it is now; `read` false when it can't be read.
CapturedValue read_value(const ScanEnvironment& environment, const RegistryAddress& key,
                         const std::wstring& name) {
    platform::RegistryKey opened;
    if (const LSTATUS status = open_key(environment, key, opened); status != ERROR_SUCCESS) {
        return missing(status) ? CapturedValue{true, std::nullopt} : CapturedValue{};
    }
    RawValue value;
    const LSTATUS status = platform::query_raw_value(opened.get(), name, value);
    if (status == ERROR_SUCCESS) {
        return {true, std::move(value)};
    }
    return missing(status) ? CapturedValue{true, std::nullopt} : CapturedValue{};
}

// One edit for several traces' edits of a value, each planned on what the one before it left, so
// it expects the value as it is now and writes what the last one would.
ValueEditLocation combine(std::vector<ValueEditLocation> steps) {
    if (steps.size() == 1) {
        return std::move(steps.front());
    }
    ValueEditLocation edit = steps.front();
    edit.desired = steps.back().desired;
    edit.entries.clear();
    edit.delta = 0;
    const bool shared_dll = value_spot(edit.key) == ValueSpot::SharedDlls;
    if (!edit.desired) {
        edit.kind = ValueEditKind::DeleteValue;
    } else if (shared_dll) {
        edit.kind = ValueEditKind::DecrementSharedDll;
    } else {
        const bool restores = std::any_of(steps.begin(), steps.end(), [](const ValueEditLocation& step) {
            return step.kind == ValueEditKind::RestoreValue;
        });
        edit.kind = restores ? ValueEditKind::RestoreValue : ValueEditKind::RemovePathEntries;
    }
    if (edit.kind == ValueEditKind::RemovePathEntries) {
        for (const auto& step : steps) {
            edit.entries.insert(edit.entries.end(), step.entries.begin(), step.entries.end());
        }
    }
    if (shared_dll && edit.expected) {
        const auto before = shared_dll_count(*edit.expected);
        const auto after = edit.desired ? shared_dll_count(*edit.desired) : std::optional<std::uint32_t>(0);
        if (before && after) {
            edit.delta = static_cast<std::int64_t>(*before) - *after;
        }
    }
    return edit;
}

// A path the edit names inside the program's folder or an item tied to it: an entry it takes out
// of a list, the variable it deletes, the DLL whose count it lowers. As the edit spells it.
std::optional<std::wstring> tied_reference(const TraceValuePlan::Edit& edit,
                                           std::span<const ScanTarget> targets) {
    const ValueEditLocation& location = edit.location;
    std::vector<std::wstring> paths;
    if (value_spot(location.key) == ValueSpot::SharedDlls) {
        paths.push_back(location.value_name);
    } else if (location.kind == ValueEditKind::RemovePathEntries) {
        paths = location.entries;
    } else if (location.expected) {
        const std::wstring text = string_value_text(*location.expected).value_or(L"");
        for (std::size_t start = 0; start <= text.size();) {
            const std::size_t end = std::min(text.find(L';', start), text.size());
            paths.push_back(text.substr(start, end - start));
            start = end + 1;
        }
    }
    for (const auto& path : paths) {
        std::wstring text(platform::trim(platform::expand_environment(path)));
        if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') {
            text = text.substr(1, text.size() - 2);
        }
        if (!platform::is_absolute_path(text)) {
            continue;
        }
        for (const std::size_t index : edit.targets) {
            const auto& target = targets[index];
            if (!target.install_location.empty() && platform::path_is_under(text, target.install_location)) {
                return path;
            }
            for (const auto& item : target.traced) {
                const auto* file = std::get_if<FileLocation>(&item.location);
                if (file && item.tied && item.change == TracedChange::Created &&
                    (file->directory ? platform::path_is_under(text, file->path)
                                     : platform::paths_equal(text, file->path))) {
                    return path;
                }
            }
        }
    }
    return std::nullopt;
}

std::wstring value_identity(const RegistryAddress& key, const std::wstring& name) {
    return identity(RegistryValueLocation{key, name});
}

}  // namespace

TraceValuePlan plan_trace_values(const ScanEnvironment& environment, std::span<const ScanTarget> targets) {
    TraceValuePlan plan;
    plan.bases.resize(targets.size());
    // The traced values by the value they change, in the order they come.
    struct Change {
        std::size_t target;
        std::size_t value;
    };
    std::vector<std::vector<Change>> groups;
    std::unordered_map<std::wstring, std::size_t> group_of;
    for (std::size_t t = 0; t < targets.size(); ++t) {
        const auto& values = targets[t].traced_values;
        plan.bases[t].assign(values.size(), ValueEditBasis::Unsupported);
        for (std::size_t i = 0; i < values.size(); ++i) {
            const auto [found, added] =
                group_of.try_emplace(value_identity(values[i].key, values[i].name), groups.size());
            if (added) {
                groups.emplace_back();
            }
            groups[found->second].push_back({t, i});
        }
    }
    const bool traced = std::any_of(targets.begin(), targets.end(),
                                    [](const ScanTarget& target) { return !target.trace_id.empty(); });
    if (!traced) {
        return plan;
    }

    for (const auto& group : groups) {
        const TracedValue& first = targets[group.front().target].traced_values[group.front().value];
        const CapturedValue live = read_value(environment, first.key, first.name);
        if (!live.read) {
            continue;  // Unsupported: nothing can be planned without the value
        }
        // Each trace's change in turn, planned on what the one before it leaves. One trace that two
        // targets share (a bundle and its packages) takes the value back once.
        std::optional<RawValue> current = live.value;
        std::vector<ValueEditLocation> steps;
        bool restore = false;
        std::vector<std::size_t> owners;
        std::vector<std::pair<std::wstring, ValueEditBasis>> traces;
        for (const auto& [t, i] : group) {
            const ScanTarget& target = targets[t];
            const TracedValue& value = target.traced_values[i];
            ValueEditBasis& basis = plan.bases[t][i];
            const auto seen = std::find_if(traces.begin(), traces.end(),
                                           [&](const auto& trace) { return trace.first == target.trace_id; });
            if (seen != traces.end()) {
                basis = seen->second;
                if (planned(basis) && std::find(owners.begin(), owners.end(), t) == owners.end()) {
                    owners.push_back(t);
                }
                continue;
            }
            const CapturedValue* captured =
                i < target.captured_values.size() ? &target.captured_values[i] : nullptr;
            if (value.spot == ValueSpot::SharedDlls && captured && captured->read &&
                !same(captured->value, live.value)) {
                // The count changed since the target was captured: the uninstaller took its reference
                // back itself (or another installer counted the DLL in the meantime).
                basis = ValueEditBasis::AlreadyUndone;
            } else {
                auto step = plan_value_edit(value, current);
                basis = step.basis;
                if (step.edit) {
                    current = step.edit->desired;
                    restore = restore || step.basis == ValueEditBasis::Restore;
                    steps.push_back(std::move(*step.edit));
                    owners.push_back(t);
                }
            }
            traces.emplace_back(target.trace_id, basis);
        }
        if (!steps.empty()) {
            plan.edits.push_back({combine(std::move(steps)),
                                  restore ? ValueEditBasis::Restore : ValueEditBasis::Undo,
                                  std::move(owners)});
        }
    }

    // Every SharedDLLs count, lowered by the planned edits. A key both registry views share is read once.
    std::unordered_map<std::wstring, const ValueEditLocation*> edited;
    for (const auto& edit : plan.edits) {
        edited.emplace(value_identity(edit.location.key, edit.location.value_name), &edit.location);
    }
    std::set<std::wstring> seen;
    for (const wchar_t* path : kSharedDllKeys) {
        const RegistryAddress address = machine_key(path);
        platform::RegistryKey key;
        if (open_key(environment, address, key) != ERROR_SUCCESS) {
            continue;
        }
        const std::wstring kernel = platform::to_lower(key.kernel_name());
        if (!kernel.empty() && !seen.insert(kernel).second) {
            continue;
        }
        for (const auto& value : key.values()) {
            if (!platform::is_absolute_path(value.name)) {
                continue;
            }
            std::uint64_t count = value.as_integer().value_or(0);
            if (const auto found = edited.find(value_identity(address, value.name)); found != edited.end()) {
                const auto& desired = found->second->desired;
                count = desired ? shared_dll_count(*desired).value_or(0) : 0;
            }
            plan.shared_dlls.push_back({platform::normalize_path(value.name), count});
        }
    }
    return plan;
}

// Offers `location`, the traced item or one the trace saw inside it, with the traced item's tie.
void offer_traced(const ScanContext& context, const TraceValuePlan& values, const TracedItem& traced,
                  Location location, bool holds_more, std::vector<Leftover>& found) {
    const auto& target = context.target;
    std::vector<Record> records;
    if (traced.tied) {
        records.push_back({Evidence::TracedInstallRoot, 0, ReferenceKind::None, traced.tie});
    } else {
        records.push_back({Evidence::TracedCreated});
        if (named_like_program(target.names, location)) {
            records.push_back({Evidence::ProductNamePerfectMatch});
        }
    }
    if (holds_more) {
        records.push_back({Evidence::ContainerHoldsMore});
    }
    if (const auto* file = std::get_if<FileLocation>(&location)) {
        if (file->directory && context.still_used(file->path)) {
            records.push_back({Evidence::DirectoryStillUsed});
        }
        if (const auto* dll = still_shared(values, *file)) {
            records.push_back({Evidence::SharedDllStillUsed, 0, ReferenceKind::None, dll->path});
        }
    }
    if (context.other_user(user_of(context.environment, location))) {
        records.push_back({Evidence::OtherUserData});
    }
    Leftover item;
    item.category = category_of(location);
    item.location = std::move(location);
    item.records = std::move(records);
    item.owner_ids = {target.program_id};
    item.found_by = Scanner::Trace;
    found.push_back(std::move(item));
}

void scan_trace(const ScanContext& context, const TraceValuePlan& values, std::vector<Leftover>& found,
                TraceUse& use) {
    const auto& target = context.target;
    for (const auto& traced : target.traced) {
        // What existed before the install belongs to the system or the user: an installer that
        // changed it doesn't make it the program's.
        if (traced.change != TracedChange::Created) {
            ++use.shown_only;
            continue;
        }
        if (!still_there(context.environment, traced.location)) {
            ++use.gone;  // The uninstaller removed it, or someone did since
            continue;
        }
        bool holds_more = false;
        if (holds_items(traced.location)) {
            if (traced.inside) {
                const ContainerCheck check(context.environment, traced.location, *traced.inside);
                auto fit = check.fit(L"");
                if (!fit.exact && fit.readable) {
                    // Someone else's items are in it: only what the trace saw goes.
                    ++use.split;
                    use.split_into += fit.offers.size();
                    for (auto& inner : fit.offers) {
                        offer_traced(context, values, traced, std::move(inner.location), inner.holds_more, found);
                    }
                    continue;
                }
                holds_more = !fit.exact;
            } else {
                const auto count = count_live(context.environment, traced.location, traced.contents);
                holds_more = !count || *count > traced.contents;
            }
        }
        offer_traced(context, values, traced, traced.location, holds_more, found);
    }
}

void add_trace_value_edits(const TraceValuePlan& plan, std::span<const ScanTarget> targets,
                           std::vector<Leftover>& found) {
    // Each value a trace saved, with the index in `found` of its edit when one is planned.
    std::unordered_map<std::wstring, std::optional<std::size_t>> traced;
    for (const auto& target : targets) {
        for (const auto& value : target.traced_values) {
            traced.try_emplace(value_identity(value.key, value.name));
        }
    }
    if (traced.empty()) {
        return;
    }
    for (const auto& edit : plan.edits) {
        Leftover item;
        item.location = edit.location;
        item.category = Category::Registry;
        item.found_by = Scanner::Trace;
        for (const std::size_t index : edit.targets) {
            const auto& id = targets[index].program_id;
            if (std::find(item.owner_ids.begin(), item.owner_ids.end(), id) == item.owner_ids.end()) {
                item.owner_ids.push_back(id);
            }
        }
        if (edit.basis == ValueEditBasis::Restore) {
            // The value the installer replaced may be what the user wants now: never checked by default.
            item.records.push_back({Evidence::TracedValueRestore});
        } else {
            item.records.push_back({Evidence::TracedValueUndo});
            if (const auto path = tied_reference(edit, targets)) {
                const bool shared_dll = value_spot(edit.location.key) == ValueSpot::SharedDlls;
                item.records.push_back({Evidence::PathReference, 0,
                                        shared_dll ? ReferenceKind::SharedDll : ReferenceKind::None, *path});
            }
        }
        traced[value_identity(edit.location.key, edit.location.value_name)] = found.size();
        found.push_back(std::move(item));
    }

    // A raw value the traces know (a SharedDLLs value pointing into the program's folder): deleting
    // it would take more than the installer's part, so its edit stands for it.
    std::vector<bool> drop(found.size(), false);
    for (std::size_t i = 0; i < found.size(); ++i) {
        if (!std::holds_alternative<RegistryValueLocation>(found[i].location)) {
            continue;
        }
        const auto entry = traced.find(identity(found[i].location));
        if (entry == traced.end()) {
            continue;
        }
        if (entry->second) {
            Leftover& edit = found[*entry->second];
            for (auto& record : found[i].records) {
                add_record(edit.records, std::move(record));
            }
            for (auto& owner : found[i].owner_ids) {
                if (std::none_of(edit.owner_ids.begin(), edit.owner_ids.end(),
                                 [&](const std::wstring& id) { return platform::iequals(id, owner); })) {
                    edit.owner_ids.push_back(std::move(owner));
                }
            }
        }
        drop[i] = true;
    }
    std::vector<Leftover> kept;
    kept.reserve(found.size());
    for (std::size_t i = 0; i < found.size(); ++i) {
        if (!drop[i]) {
            kept.push_back(std::move(found[i]));
        }
    }
    found = std::move(kept);
}

}  // namespace avel::leftovers::detail
