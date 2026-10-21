#include "core/changes/executor.h"

#include <algorithm>
#include <map>
#include <memory>

#include "core/changes/file_quarantine.h"
#include "core/changes/registry_backup.h"
#include "core/changes/serialization.h"
#include "core/changes/system_items.h"
#include "core/leftovers/environment.h"
#include "core/platform/file_ops.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/platform/user_hives.h"

namespace avel::changes {

namespace {

using leftovers::FileLocation;
using leftovers::FirewallRuleLocation;
using leftovers::Location;
using leftovers::PackageLocation;
using leftovers::RegistryKeyLocation;
using leftovers::RegistryValueLocation;
using leftovers::ServiceLocation;
using leftovers::TaskLocation;

constexpr wchar_t kServicesKey[] = LR"(SYSTEM\CurrentControlSet\Services)";

// Opens registry roots, loading signed-out users' hives for writing as needed.
class Roots {
public:
    explicit Roots(const ExecuteOptions& options) : custom_(options.registry_root) {}

    HKEY get(platform::Hive hive, const std::wstring& sid) {
        if (custom_) {
            return custom_(hive, sid);
        }
        if (hive == platform::Hive::Machine) {
            return HKEY_LOCAL_MACHINE;
        }
        const std::wstring key = platform::to_lower(sid);
        if (const auto found = hives_.find(key); found != hives_.end()) {
            return found->second.key.get();
        }
        for (const auto& profile : platform::enumerate_user_profiles()) {
            if (platform::iequals(profile.sid, sid)) {
                platform::UserHive opened;
                if (platform::open_user_hive(profile, KEY_ALL_ACCESS, opened) == ERROR_SUCCESS) {
                    return hives_.emplace(key, std::move(opened)).first->second.key.get();
                }
            }
        }
        return nullptr;
    }

private:
    std::function<HKEY(platform::Hive, const std::wstring&)> custom_;
    std::map<std::wstring, platform::UserHive> hives_;
};

std::function<leftovers::ProtectionReason(const Location&)> protection_for(const ExecuteOptions& options) {
    if (options.protection) {
        return options.protection;
    }
    std::vector<std::wstring> known;
    {
        // Only for the folder lists; its read-only hive loads end with this scope.
        const leftovers::LiveEnvironment live;
        const auto& environment = live.environment();
        known = environment.machine_folders.all;
        for (const auto& user : environment.users) {
            known.insert(known.end(), user.folders.all.begin(), user.folders.all.end());
        }
    }
    auto policy = std::make_shared<leftovers::ProtectedPolicy>(std::move(known));
    return [policy](const Location& location) {
        const auto reason = leftovers::HardFloor::live().check(location);
        return reason != leftovers::ProtectionReason::None ? reason : policy->check(location);
    };
}

// Services and tasks first, so the files they run are free; files last.
int phase(const Location& location) {
    return std::visit(
        [](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, ServiceLocation>) return 0;
            else if constexpr (std::is_same_v<T, TaskLocation>) return 1;
            else if constexpr (std::is_same_v<T, PackageLocation>) return 2;
            else if constexpr (std::is_same_v<T, RegistryValueLocation>) return 3;
            else if constexpr (std::is_same_v<T, RegistryKeyLocation>) return 4;
            else if constexpr (std::is_same_v<T, FileLocation>) return 5;
            else return 6;
        },
        location);
}

std::vector<std::size_t> execution_order(const ChangePlan& plan) {
    std::vector<std::size_t> order(plan.items.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return phase(plan.items[a].location) < phase(plan.items[b].location);
    });
    return order;
}

std::wstring reg_root_name(const platform::RegistryAddress& address) {
    return address.hive == platform::Hive::Machine ? L"HKEY_LOCAL_MACHINE\\" : L"HKEY_USERS\\" + address.sid + L"\\";
}

std::pair<std::wstring, std::wstring> split_key(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        return {L"", path};
    }
    return {path.substr(0, slash), path.substr(slash + 1)};
}

// Opens (or, for undo, creates) `root\path` with `access`.
wil::unique_hkey open_key(HKEY root, const std::wstring& path, REGSAM access, bool create, LSTATUS& status) {
    HKEY raw = nullptr;
    if (path.empty()) {
        status = ::RegOpenKeyExW(root, nullptr, 0, access | KEY_WOW64_64KEY, &raw);
    } else if (create) {
        status = ::RegCreateKeyExW(root, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, access | KEY_WOW64_64KEY,
                                   nullptr, &raw, nullptr);
        if (status == ERROR_CHILD_MUST_BE_VOLATILE) {
            status = ::RegCreateKeyExW(root, path.c_str(), 0, nullptr, REG_OPTION_VOLATILE, access | KEY_WOW64_64KEY,
                                       nullptr, &raw, nullptr);
        }
    } else {
        status = ::RegOpenKeyExW(root, path.c_str(), 0, access | KEY_WOW64_64KEY, &raw);
    }
    return wil::unique_hkey(status == ERROR_SUCCESS ? raw : nullptr);
}

Reason reason_for(LSTATUS status) {
    switch (status) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return Reason::Missing;
        case ERROR_ACCESS_DENIED:
            return Reason::AccessDenied;
        case ERROR_CANT_ACCESS_FILE:
            return Reason::RegistryLink;
        default:
            return Reason::Error;
    }
}

bool write_json(const std::wstring& path, const Json& json) {
    return write_file_durably(path, json.dump(1, '\t'));
}

bool write_utf16(const std::wstring& path, const std::wstring& text) {
    return write_file_durably(path,
                              std::string_view(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t)));
}

std::optional<Json> read_json(const std::wstring& path) {
    const auto text = read_file(path);
    if (!text) {
        return std::nullopt;
    }
    Json json = Json::parse(*text, nullptr, false);
    return json.is_discarded() ? std::nullopt : std::optional<Json>(std::move(json));
}

bool exists(const std::wstring& path) {
    return ::GetFileAttributesW(platform::extended(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// An item that was only partly removed (files in use, a key deletion that stopped) has a "partial"
// marker in its backup folder: undo then merges into what's left instead of refusing it.
void mark_partial(const std::wstring& folder) {
    write_file_durably(folder + L"\\partial", "");
}

bool is_partial(const std::wstring& folder) {
    return exists(folder + L"\\partial");
}

// One run of execute(): the set, the roots, the journal with its stop point.
class Run {
public:
    Run(ChangeSet& set, const ExecuteOptions& options)
        : set_(set), options_(options), roots_(options), protection_(protection_for(options)) {}

    bool stopped() const { return stopped_; }
    std::uint64_t bytes() const { return bytes_; }

    // Records `status`; false once the stop point is reached (the caller stops at once).
    bool record(std::size_t index, ItemStatus status) {
        set_.record(index, status);
        ++records_;
        if (options_.stop_after && records_ >= *options_.stop_after) {
            stopped_ = true;
            if (options_.on_stop) {
                options_.on_stop();
            }
        }
        return !stopped_;
    }

    ItemResult remove(std::size_t index, const PlannedItem& item, bool contained) {
        ItemResult result;
        const auto finish = [&](ItemStatus status) {
            result.status = status;
            record(index, status);
            return result;
        };
        if (!is_removable(item.location)) {
            return finish({ItemState::Skipped, Reason::ReportOnly, 0});
        }
        if (protection_(item.location) != leftovers::ProtectionReason::None) {
            return finish({ItemState::Skipped, Reason::Protected, 0});
        }
        if (!is_undoable(item.location) && !options_.allow_irreversible) {
            return finish({ItemState::Skipped, Reason::NotUndoable, 0});
        }
        if (contained) {
            return finish({ItemState::Skipped, Reason::Contained, 0});
        }
        return std::visit([&](const auto& location) { return remove_item(index, location, result, finish); },
                          item.location);
    }

    ItemResult restore(std::size_t index, const PlannedItem& item) {
        ItemResult result;
        const std::wstring folder = set_.item_folder(index);
        result.status = std::visit([&](const auto& location) { return restore_item(folder, location, result); },
                                   item.location);
        set_.record(index, result.status);
        return result;
    }

    ItemStatus settle(std::size_t index, const PlannedItem& item) {
        const std::wstring folder = set_.item_folder(index);
        return std::visit([&](const auto& location) { return settle_item(folder, location); }, item.location);
    }

private:
    template <typename Finish>
    ItemResult remove_item(std::size_t index, const FileLocation& location, ItemResult& result, Finish& finish) {
        const auto volume = platform::volume_root(location.path);
        const std::wstring quarantine = volume ? set_.quarantine_path(index, *volume) : std::wstring();
        if (quarantine.empty()) {
            return finish({ItemState::Skipped, Reason::UnsupportedVolume, 0});
        }
        ItemStatus failure;
        const auto move = prepare_move(location.path, location.directory, quarantine, failure);
        if (!move) {
            return finish(failure);
        }
        const std::wstring folder = set_.item_folder(index);
        if (!write_json(folder + L"\\file.json", to_json(*move))) {
            return finish({ItemState::Failed, Reason::Error, ::GetLastError()});
        }
        if (!record(index, {ItemState::BackedUp, Reason::None, 0})) {
            return result;
        }
        auto moved = move_to_quarantine(*move);
        result.left_behind = std::move(moved.left_behind);
        if (moved.status.state == ItemState::Partial) {
            result.holders = processes_using(result.left_behind);
            Json in_use = Json::array();
            for (const auto& file : result.left_behind) {
                Json entry;
                put_text(entry, "path", file);
                in_use.push_back(std::move(entry));
            }
            write_json(folder + L"\\in-use.json", in_use);
            mark_partial(folder);
        }
        if (moved.status.state == ItemState::Done || moved.status.state == ItemState::Partial) {
            bytes_ += move->bytes;
        }
        return finish(moved.status);
    }

    template <typename Finish>
    ItemResult remove_item(std::size_t index, const RegistryKeyLocation& location, ItemResult& result, Finish& finish) {
        const HKEY root = roots_.get(location.key.hive, location.key.sid);
        if (!root) {
            return finish({ItemState::Skipped, Reason::Missing, 0});
        }
        const auto [parent_path, name] = split_key(location.key.path);
        LSTATUS status = ERROR_SUCCESS;
        const auto parent = open_key(root, parent_path, KEY_READ, false, status);
        if (!parent) {
            return finish({ItemState::Skipped, reason_for(status), static_cast<std::uint32_t>(status)});
        }
        KeySnapshot snapshot;
        if (status = capture_key(parent.get(), name, snapshot); status != ERROR_SUCCESS) {
            return finish({ItemState::Skipped, reason_for(status), static_cast<std::uint32_t>(status)});
        }
        const std::wstring folder = set_.item_folder(index);
        if (!write_json(folder + L"\\key.json", to_json(snapshot)) ||
            !write_utf16(folder + L"\\key.reg", to_reg_text(reg_root_name(location.key) + location.key.path, snapshot))) {
            return finish({ItemState::Failed, Reason::Error, ::GetLastError()});
        }
        if (!record(index, {ItemState::BackedUp, Reason::None, 0})) {
            return result;
        }
        status = delete_key(parent.get(), name);
        if (status == ERROR_SUCCESS) {
            return finish({ItemState::Done, Reason::None, 0});
        }
        // Part of the tree may be gone; undo merges the backup back in.
        mark_partial(folder);
        return finish({ItemState::Partial, Reason::Error, static_cast<std::uint32_t>(status)});
    }

    template <typename Finish>
    ItemResult remove_item(std::size_t index, const RegistryValueLocation& location, ItemResult& result,
                           Finish& finish) {
        const HKEY root = roots_.get(location.key.hive, location.key.sid);
        LSTATUS status = ERROR_FILE_NOT_FOUND;
        const auto key = root ? open_key(root, location.key.path, KEY_READ | KEY_SET_VALUE, false, status)
                              : wil::unique_hkey();
        if (!key) {
            return finish({ItemState::Skipped, reason_for(status), static_cast<std::uint32_t>(status)});
        }
        ValueSnapshot value;
        if (status = capture_value(key.get(), location.value_name, value); status != ERROR_SUCCESS) {
            return finish({ItemState::Skipped, reason_for(status), static_cast<std::uint32_t>(status)});
        }
        const std::wstring folder = set_.item_folder(index);
        if (!write_json(folder + L"\\value.json", to_json(value)) ||
            !write_utf16(folder + L"\\value.reg", to_reg_text(reg_root_name(location.key) + location.key.path, value))) {
            return finish({ItemState::Failed, Reason::Error, ::GetLastError()});
        }
        if (!record(index, {ItemState::BackedUp, Reason::None, 0})) {
            return result;
        }
        status = ::RegDeleteValueW(key.get(), location.value_name.c_str());
        return finish(status == ERROR_SUCCESS ? ItemStatus{ItemState::Done, Reason::None, 0}
                                              : ItemStatus{ItemState::Failed, reason_for(status),
                                                           static_cast<std::uint32_t>(status)});
    }

    template <typename Finish>
    ItemResult remove_item(std::size_t index, const ServiceLocation& location, ItemResult& result, Finish& finish) {
        LSTATUS status = ERROR_SUCCESS;
        const HKEY root = roots_.get(platform::Hive::Machine, L"");
        const auto services = open_key(root, kServicesKey, KEY_READ, false, status);
        ItemStatus failure;
        const auto backup = services ? backup_service(services.get(), location.name, failure) : std::nullopt;
        if (!backup) {
            return finish(services ? failure : ItemStatus{ItemState::Skipped, reason_for(status), 0});
        }
        if (!write_json(set_.item_folder(index) + L"\\service.json", to_json(*backup))) {
            return finish({ItemState::Failed, Reason::Error, ::GetLastError()});
        }
        if (!record(index, {ItemState::BackedUp, Reason::None, 0})) {
            return result;
        }
        return finish(remove_service(*backup));
    }

    template <typename Finish>
    ItemResult remove_item(std::size_t index, const TaskLocation& location, ItemResult& result, Finish& finish) {
        ItemStatus failure;
        const auto backup = backup_task(location.path, failure);
        if (!backup) {
            return finish(failure);
        }
        const std::wstring folder = set_.item_folder(index);
        if (!write_json(folder + L"\\task.json", to_json(*backup)) || !write_utf16(folder + L"\\task.xml", backup->xml)) {
            return finish({ItemState::Failed, Reason::Error, ::GetLastError()});
        }
        if (!record(index, {ItemState::BackedUp, Reason::None, 0})) {
            return result;
        }
        return finish(remove_task(*backup));
    }

    template <typename Finish>
    ItemResult remove_item(std::size_t index, const PackageLocation& location, ItemResult& result, Finish& finish) {
        Json json;
        put_text(json, "full_name", location.full_name);
        put_text(json, "user_sid", location.user_sid);
        if (!write_json(set_.item_folder(index) + L"\\package.json", json)) {
            return finish({ItemState::Failed, Reason::Error, ::GetLastError()});
        }
        if (!record(index, {ItemState::BackedUp, Reason::None, 0})) {
            return result;
        }
        return finish(remove_package_item(location.full_name, location.user_sid));
    }

    template <typename Finish>
    ItemResult remove_item(std::size_t, const FirewallRuleLocation&, ItemResult&, Finish& finish) {
        return finish({ItemState::Skipped, Reason::ReportOnly, 0});
    }

    ItemStatus restore_item(const std::wstring& folder, const FileLocation&, ItemResult& result) {
        const auto json = read_json(folder + L"\\file.json");
        FileMove move;
        if (!json || !from_json(*json, move)) {
            return {ItemState::RestoreFailed, Reason::BackupMissing, 0};
        }
        auto restored = restore_from_quarantine(move, is_partial(folder));
        result.left_behind = std::move(restored.left_behind);
        return restored.status;
    }

    ItemStatus restore_item(const std::wstring& folder, const RegistryKeyLocation& location, ItemResult&) {
        const auto json = read_json(folder + L"\\key.json");
        KeySnapshot snapshot;
        const HKEY root = roots_.get(location.key.hive, location.key.sid);
        if (!json || !from_json(*json, snapshot)) {
            return {ItemState::RestoreFailed, Reason::BackupMissing, 0};
        }
        if (!root) {
            return {ItemState::RestoreFailed, Reason::Missing, 0};
        }
        LSTATUS status = ERROR_SUCCESS;
        const auto parent = open_key(root, split_key(location.key.path).first, KEY_ALL_ACCESS, true, status);
        if (!parent) {
            return {ItemState::RestoreFailed, reason_for(status), static_cast<std::uint32_t>(status)};
        }
        status = restore_key(parent.get(), snapshot);
        if (status == ERROR_ALREADY_EXISTS && is_partial(folder)) {
            // A deletion that stopped part way left some of the key: fill in the rest.
            status = merge_key(parent.get(), snapshot);
        }
        if (status == ERROR_ALREADY_EXISTS) {
            return {ItemState::RestoreSkipped, Reason::Exists, 0};
        }
        return status == ERROR_SUCCESS ? ItemStatus{ItemState::Restored, Reason::None, 0}
                                       : ItemStatus{ItemState::RestoreFailed, reason_for(status),
                                                    static_cast<std::uint32_t>(status)};
    }

    ItemStatus restore_item(const std::wstring& folder, const RegistryValueLocation& location, ItemResult&) {
        const auto json = read_json(folder + L"\\value.json");
        ValueSnapshot value;
        const HKEY root = roots_.get(location.key.hive, location.key.sid);
        if (!json || !from_json(*json, value)) {
            return {ItemState::RestoreFailed, Reason::BackupMissing, 0};
        }
        LSTATUS status = ERROR_FILE_NOT_FOUND;
        const auto key = root ? open_key(root, location.key.path, KEY_ALL_ACCESS, true, status) : wil::unique_hkey();
        if (!key) {
            return {ItemState::RestoreFailed, reason_for(status), static_cast<std::uint32_t>(status)};
        }
        status = restore_value(key.get(), value);
        if (status == ERROR_ALREADY_EXISTS) {
            return {ItemState::RestoreSkipped, Reason::Exists, 0};
        }
        return status == ERROR_SUCCESS ? ItemStatus{ItemState::Restored, Reason::None, 0}
                                       : ItemStatus{ItemState::RestoreFailed, reason_for(status),
                                                    static_cast<std::uint32_t>(status)};
    }

    ItemStatus restore_item(const std::wstring& folder, const ServiceLocation&, ItemResult&) {
        const auto json = read_json(folder + L"\\service.json");
        ServiceBackup backup;
        if (!json || !from_json(*json, backup)) {
            return {ItemState::RestoreFailed, Reason::BackupMissing, 0};
        }
        LSTATUS status = ERROR_SUCCESS;
        const auto services =
            open_key(roots_.get(platform::Hive::Machine, L""), kServicesKey, KEY_ALL_ACCESS, false, status);
        if (!services) {
            return {ItemState::RestoreFailed, reason_for(status), static_cast<std::uint32_t>(status)};
        }
        return restore_service(services.get(), backup);
    }

    ItemStatus restore_item(const std::wstring& folder, const TaskLocation&, ItemResult&) {
        const auto json = read_json(folder + L"\\task.json");
        TaskBackup backup;
        if (!json || !from_json(*json, backup)) {
            return {ItemState::RestoreFailed, Reason::BackupMissing, 0};
        }
        return restore_task(backup);
    }

    ItemStatus restore_item(const std::wstring&, const PackageLocation&, ItemResult&) {
        return {ItemState::RestoreSkipped, Reason::NotUndoable, 0};
    }

    ItemStatus restore_item(const std::wstring&, const FirewallRuleLocation&, ItemResult&) {
        return {ItemState::RestoreSkipped, Reason::ReportOnly, 0};
    }

    // After a crash: was the item removed? Its backup is on disk; the system tells the rest.
    ItemStatus settle_item(const std::wstring& folder, const FileLocation&) {
        const auto json = read_json(folder + L"\\file.json");
        FileMove move;
        if (!json || !from_json(*json, move)) {
            return {ItemState::Failed, Reason::Interrupted, 0};
        }
        const bool original = exists(move.original);
        const bool quarantined = exists(move.quarantine);
        if (original && quarantined) {
            mark_partial(folder);  // Some files moved: undo merges them back
            return {ItemState::Partial, Reason::Interrupted, 0};
        }
        return quarantined ? ItemStatus{ItemState::Done, Reason::None, 0}
                           : ItemStatus{ItemState::Failed, Reason::Interrupted, 0};
    }

    ItemStatus settle_item(const std::wstring& folder, const RegistryKeyLocation& location) {
        const auto json = read_json(folder + L"\\key.json");
        KeySnapshot backup;
        const HKEY root = roots_.get(location.key.hive, location.key.sid);
        if (!json || !from_json(*json, backup) || !root) {
            return {ItemState::Failed, Reason::Interrupted, 0};
        }
        const auto [parent_path, name] = split_key(location.key.path);
        LSTATUS status = ERROR_SUCCESS;
        const auto parent = open_key(root, parent_path, KEY_READ, false, status);
        KeySnapshot live;
        if (!parent || capture_key_readonly(parent.get(), name, {}, live) != ERROR_SUCCESS) {
            return {ItemState::Done, Reason::None, 0};  // Gone
        }
        if (to_json(live) == to_json(backup)) {
            return {ItemState::Failed, Reason::Interrupted, 0};  // Untouched
        }
        mark_partial(folder);
        return {ItemState::Partial, Reason::Interrupted, 0};
    }

    ItemStatus settle_item(const std::wstring&, const RegistryValueLocation& location) {
        const HKEY root = roots_.get(location.key.hive, location.key.sid);
        LSTATUS status = ERROR_SUCCESS;
        const auto key = root ? open_key(root, location.key.path, KEY_READ, false, status) : wil::unique_hkey();
        const bool present =
            key && ::RegQueryValueExW(key.get(), location.value_name.c_str(), nullptr, nullptr, nullptr, nullptr) ==
                       ERROR_SUCCESS;
        return present ? ItemStatus{ItemState::Failed, Reason::Interrupted, 0} : ItemStatus{ItemState::Done, Reason::None, 0};
    }

    ItemStatus settle_item(const std::wstring&, const ServiceLocation& location) {
        return service_exists(location.name) ? ItemStatus{ItemState::Failed, Reason::Interrupted, 0}
                                             : ItemStatus{ItemState::Done, Reason::None, 0};
    }

    ItemStatus settle_item(const std::wstring&, const TaskLocation& location) {
        return task_exists(location.path) ? ItemStatus{ItemState::Failed, Reason::Interrupted, 0}
                                          : ItemStatus{ItemState::Done, Reason::None, 0};
    }

    ItemStatus settle_item(const std::wstring&, const PackageLocation& location) {
        return package_exists(location.full_name) ? ItemStatus{ItemState::Failed, Reason::Interrupted, 0}
                                                  : ItemStatus{ItemState::Done, Reason::None, 0};
    }

    ItemStatus settle_item(const std::wstring&, const FirewallRuleLocation&) {
        return {ItemState::Skipped, Reason::ReportOnly, 0};
    }

    ChangeSet& set_;
    const ExecuteOptions& options_;
    Roots roots_;
    std::function<leftovers::ProtectionReason(const Location&)> protection_;
    std::size_t records_ = 0;
    bool stopped_ = false;
    std::uint64_t bytes_ = 0;
};

bool is_contained(const ChangePlan& plan, std::size_t index) {
    for (std::size_t other = 0; other < plan.items.size(); ++other) {
        if (other != index && is_removable(plan.items[other].location) &&
            leftovers::contains(plan.items[other].location, plan.items[index].location)) {
            return true;
        }
    }
    return false;
}

}  // namespace

ExecuteResult execute(ChangeStore& store, const ChangePlan& plan, const ExecuteOptions& options,
                      const ChangeEvents& events) {
    ExecuteResult result;
    auto set = store.create(plan);
    if (!set) {
        return result;
    }
    result.set_id = set->id();
    result.items.resize(plan.items.size());
    Run run(*set, options);
    for (const std::size_t index : execution_order(plan)) {
        result.items[index] = run.remove(index, plan.items[index], is_contained(plan, index));
        if (run.stopped()) {
            result.stopped = true;
            return result;  // As if the process died here: no summary
        }
        if (events.item_finished) {
            events.item_finished(index, result.items[index]);
        }
    }
    result.quarantined_bytes = run.bytes();
    set->write_summary(SetState::Complete, result.quarantined_bytes);
    return result;
}

RestoreResult restore(ChangeStore& store, const std::wstring& id, const std::vector<std::size_t>& only,
                      const ExecuteOptions& options, const ChangeEvents& events) {
    RestoreResult result;
    auto set = store.open_set(id);
    if (!set) {
        return result;
    }
    result.found = true;
    Run run(*set, options);
    auto order = execution_order(set->plan());
    std::reverse(order.begin(), order.end());
    // Removed items, and items an earlier undo couldn't put back.
    const auto removed = [](ItemState state) {
        return state == ItemState::Done || state == ItemState::Partial || state == ItemState::RestoreSkipped ||
               state == ItemState::RestoreFailed;
    };
    for (const std::size_t index : order) {
        const auto& location = set->plan().items[index].location;
        if (removed(set->statuses()[index].state) && is_undoable(location) &&
            (only.empty() || std::find(only.begin(), only.end(), index) != only.end())) {
            result.items.emplace_back(index, run.restore(index, set->plan().items[index]));
            if (events.item_finished) {
                events.item_finished(index, result.items.back().second);
            }
        }
    }
    std::size_t undoable = 0;
    std::size_t restored = 0;
    for (std::size_t index = 0; index < set->statuses().size(); ++index) {
        const ItemState state = set->statuses()[index].state;
        if (is_undoable(set->plan().items[index].location) && (removed(state) || state == ItemState::Restored)) {
            ++undoable;
            restored += state == ItemState::Restored;
        }
    }
    result.state = undoable > 0 && restored == undoable ? SetState::Restored
                   : restored > 0                      ? SetState::PartlyRestored
                                                       : SetState::Complete;
    set->write_summary(result.state, set->summary().value_or(SetSummary{}).quarantined_bytes);
    return result;
}

std::vector<std::wstring> recover(ChangeStore& store, const ExecuteOptions& options) {
    std::vector<std::wstring> recovered;
    for (const auto& summary : store.list()) {
        if (summary.state != SetState::Running) {
            continue;
        }
        auto set = store.open_set(summary.id);
        if (!set) {
            continue;  // A live run
        }
        Run run(*set, options);
        for (std::size_t index = 0; index < set->plan().items.size(); ++index) {
            const ItemState state = set->statuses()[index].state;
            if (state == ItemState::BackedUp) {
                set->record(index, run.settle(index, set->plan().items[index]));
            } else if (state == ItemState::Pending) {
                set->record(index, {ItemState::Skipped, Reason::Interrupted, 0});
            }
        }
        set->write_summary(SetState::Complete);
        recovered.push_back(summary.id);
    }
    return recovered;
}

}  // namespace avel::changes
