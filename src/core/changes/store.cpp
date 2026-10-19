#include "core/changes/store.h"

#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <format>

#include "core/changes/serialization.h"
#include "core/platform/file_ops.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::changes {

namespace {

constexpr int kSchemaVersion = 1;
constexpr wchar_t kQuarantineFolder[] = L"$Avel.Quarantine";

std::int64_t unix_seconds(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

std::chrono::system_clock::time_point from_unix(std::int64_t seconds) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
}

std::string iso_time(std::chrono::system_clock::time_point time) {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(time));
}

Json plan_to_json(const ChangePlan& plan, const std::wstring& id, std::chrono::system_clock::time_point created) {
    Json json;
    json["schema_version"] = kSchemaVersion;
    json["id"] = platform::to_utf8(id);
    json["created"] = iso_time(created);
    json["created_unix"] = unix_seconds(created);
    put_text(json, "description", plan.description);
    json["items"] = Json::array();
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        const auto& item = plan.items[i];
        Json entry;
        entry["index"] = i;
        entry["location"] = to_json(item.location);
        entry["category"] = leftovers::to_string(item.category);
        entry["programs"] = Json::array();
        for (const auto& program : item.programs) {
            entry["programs"].push_back(platform::to_utf8(program));
        }
        entry["confidence"] = leftovers::to_string(item.confidence);
        entry["score"] = item.score;
        json["items"].push_back(std::move(entry));
    }
    return json;
}

bool plan_from_json(const Json& json, ChangePlan& plan, std::chrono::system_clock::time_point& created) {
    if (!json.is_object() || json.value("schema_version", 0) != kSchemaVersion || !json.contains("items") ||
        !json["items"].is_array()) {
        return false;
    }
    created = from_unix(json.value("created_unix", std::int64_t{0}));
    plan = {};
    plan.description = get_text(json, "description").value_or(L"");
    for (const auto& entry : json["items"]) {
        PlannedItem item;
        if (!entry.contains("location") || !from_json(entry["location"], item.location)) {
            return false;
        }
        const std::string category = entry.value("category", std::string());
        for (const auto candidate :
             {leftovers::Category::Files, leftovers::Category::Shortcuts, leftovers::Category::Registry,
              leftovers::Category::Services, leftovers::Category::Tasks, leftovers::Category::Packages,
              leftovers::Category::Firewall}) {
            if (leftovers::to_string(candidate) == category) {
                item.category = candidate;
            }
        }
        if (entry.contains("programs") && entry["programs"].is_array()) {
            for (const auto& program : entry["programs"]) {
                if (program.is_string()) {
                    item.programs.push_back(platform::to_utf16(program.get_ref<const std::string&>()));
                }
            }
        }
        const std::string confidence = entry.value("confidence", std::string());
        for (const auto candidate : {leftovers::Confidence::Unknown, leftovers::Confidence::Bad,
                                     leftovers::Confidence::Questionable, leftovers::Confidence::Good,
                                     leftovers::Confidence::VeryGood}) {
            if (leftovers::to_string(candidate) == confidence) {
                item.confidence = candidate;
            }
        }
        item.score = entry.value("score", 0);
        plan.items.push_back(std::move(item));
    }
    return true;
}

std::wstring random_suffix() {
    GUID guid{};
    ::CoCreateGuid(&guid);
    return std::format(L"{:08x}", guid.Data1);
}

// Holds the set's lock file open, exclusively, for as long as the set is in use. It deletes
// itself when closed, also when the process is killed.
wil::unique_hfile take_lock(const std::wstring& folder) {
    return wil::unique_hfile(::CreateFileW(platform::extended(folder + L"\\lock").c_str(), GENERIC_WRITE, 0, nullptr,
                                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
}

wil::unique_hfile open_journal(const std::wstring& folder) {
    wil::unique_hfile journal(::CreateFileW(platform::extended(folder + L"\\journal.jsonl").c_str(),
                                            FILE_APPEND_DATA | FILE_READ_DATA | SYNCHRONIZE, FILE_SHARE_READ, nullptr,
                                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!journal) {
        return journal;
    }
    // A crash can leave a torn last line; end it, so the next record starts on a line of its own.
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(journal.get(), &size) && size.QuadPart > 0) {
        LARGE_INTEGER position{};
        position.QuadPart = size.QuadPart - 1;
        char last = 0;
        DWORD read = 0;
        if (::SetFilePointerEx(journal.get(), position, nullptr, FILE_BEGIN) &&
            ::ReadFile(journal.get(), &last, 1, &read, nullptr) && read == 1 && last != '\n') {
            DWORD written = 0;
            ::WriteFile(journal.get(), "\n", 1, &written, nullptr);
            ::FlushFileBuffers(journal.get());
        }
    }
    return journal;
}

std::optional<SetSummary> read_summary(const std::wstring& folder) {
    const auto text = read_file(folder + L"\\summary.json");
    if (!text) {
        return std::nullopt;
    }
    const Json json = Json::parse(*text, nullptr, false);
    if (!json.is_object() || json.value("schema_version", 0) != kSchemaVersion) {
        return std::nullopt;
    }
    SetSummary summary;
    summary.id = platform::to_utf16(json.value("id", std::string()));
    summary.created = from_unix(json.value("created_unix", std::int64_t{0}));
    summary.description = get_text(json, "description").value_or(L"");
    if (!parse(json.value("state", std::string()), summary.state)) {
        return std::nullopt;
    }
    summary.items = json.value("items", std::size_t{0});
    summary.done = json.value("done", std::size_t{0});
    summary.skipped = json.value("skipped", std::size_t{0});
    summary.restored = json.value("restored", std::size_t{0});
    summary.quarantined_bytes = json.value("quarantined_bytes", std::uint64_t{0});
    return summary;
}

std::vector<std::wstring> set_folders(const std::wstring& root) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW data{};
    wil::unique_hfind find(::FindFirstFileExW(platform::extended(root + L"\\*").c_str(), FindExInfoBasic, &data,
                                              FindExSearchLimitToDirectories, nullptr, 0));
    if (!find) {
        return names;
    }
    do {
        const std::wstring_view name = data.cFileName;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            name != L"." && name != L"..") {
            names.emplace_back(name);
        }
    } while (::FindNextFileW(find.get(), &data));
    return names;
}

// Volume roots ("D:\") of every fixed NTFS volume with a drive letter.
std::vector<std::wstring> fixed_volumes() {
    std::vector<std::wstring> roots;
    wchar_t buffer[512] = {};
    const DWORD length = ::GetLogicalDriveStringsW(511, buffer);
    for (const wchar_t* root = buffer; root < buffer + length && *root; root += wcslen(root) + 1) {
        if (platform::is_fixed_ntfs(root)) {
            roots.emplace_back(root);
        }
    }
    return roots;
}

}  // namespace

StoreOptions default_store_options() {
    StoreOptions options;
    wil::unique_cotaskmem_string program_data;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &program_data))) {
        options.root = std::wstring(program_data.get()) + LR"(\Avel\Changes)";
    }
    return options;
}

ChangeStore::ChangeStore(StoreOptions options) : options_(std::move(options)) {}

std::unique_ptr<ChangeStore> ChangeStore::open(StoreOptions options, platform::FolderStatus& status) {
    if (options.root.empty() || !platform::is_absolute_path(options.root)) {
        status = platform::FolderStatus::Unavailable;
        return nullptr;
    }
    options.root = platform::normalize_path(options.root);
    status = platform::ensure_secure_folder(options.root, options.policy);
    if (status != platform::FolderStatus::Ok) {
        return nullptr;
    }
    std::unique_ptr<ChangeStore> store(new ChangeStore(std::move(options)));
    store->mutex_.reset(::CreateMutexW(nullptr, FALSE, store->options_.mutex_name.c_str()));
    if (!store->mutex_) {
        status = platform::FolderStatus::Unavailable;
        return nullptr;
    }
    return store;
}

std::optional<ChangeStore::Guard> ChangeStore::lock(std::chrono::milliseconds timeout) const {
    DWORD status = 0;
    auto guard = mutex_.acquire(&status, static_cast<DWORD>(timeout.count()));
    // An abandoned mutex (a crashed holder) is ours now; recovery deals with what it left.
    if (status != WAIT_OBJECT_0 && status != WAIT_ABANDONED) {
        return std::nullopt;
    }
    return guard;
}

std::unique_ptr<ChangeSet> ChangeStore::create(const ChangePlan& plan) {
    const auto now = options_.clock();
    std::unique_ptr<ChangeSet> set(new ChangeSet());
    set->store_ = this;
    set->id_ = std::format(L"{:%Y%m%d-%H%M%S}-", std::chrono::floor<std::chrono::seconds>(now)) + random_suffix();
    set->folder_ = options_.root + L"\\" + set->id_;
    set->plan_ = plan;
    set->created_ = now;
    set->statuses_.assign(plan.items.size(), ItemStatus{});
    if (!::CreateDirectoryW(platform::extended(set->folder_).c_str(), nullptr)) {
        return nullptr;
    }
    set->lock_ = take_lock(set->folder_);
    if (!set->lock_ ||
        !write_file_durably(set->folder_ + L"\\plan.json", plan_to_json(plan, set->id_, now).dump(1, '\t'))) {
        return nullptr;
    }
    set->journal_ = open_journal(set->folder_);
    if (!set->journal_ || !set->write_summary(SetState::Running)) {
        return nullptr;
    }
    return set;
}

std::unique_ptr<ChangeSet> ChangeStore::open_set(const std::wstring& id) {
    if (id.empty() || id.find_first_of(L"\\/:") != std::wstring::npos || id == L"." || id == L"..") {
        return nullptr;
    }
    std::unique_ptr<ChangeSet> set(new ChangeSet());
    set->store_ = this;
    set->id_ = id;
    set->folder_ = options_.root + L"\\" + id;
    // A real folder at its own path; ownership was settled by the root's checks.
    if (const auto status = platform::check_secure_folder(set->folder_, {L"", {}, false});
        status != platform::FolderStatus::ForeignOwner && status != platform::FolderStatus::Ok) {
        return nullptr;
    }
    set->lock_ = take_lock(set->folder_);
    if (!set->lock_ || !set->load()) {
        return nullptr;  // A live run holds it, or it's not a readable set
    }
    set->journal_ = open_journal(set->folder_);
    return set->journal_ ? std::move(set) : nullptr;
}

std::vector<SetSummary> ChangeStore::list() const {
    std::vector<SetSummary> sets;
    for (const auto& name : set_folders(options_.root)) {
        const std::wstring folder = options_.root + L"\\" + name;
        if (auto summary = read_summary(folder)) {
            summary->id = name;
            sets.push_back(std::move(*summary));
        } else if (const auto text = read_file(folder + L"\\plan.json")) {
            // Died before its first summary: still list it, so it can be recovered or removed.
            ChangePlan plan;
            SetSummary entry;
            if (plan_from_json(Json::parse(*text, nullptr, false), plan, entry.created)) {
                entry.id = name;
                entry.description = plan.description;
                entry.items = plan.items.size();
                sets.push_back(std::move(entry));
            }
        }
    }
    std::sort(sets.begin(), sets.end(), [](const auto& a, const auto& b) { return a.created > b.created; });
    return sets;
}

bool ChangeStore::remove(const std::wstring& id) {
    {
        const auto set = open_set(id);
        if (!set) {
            return false;
        }
    }  // Releases the lock file before deleting the folder
    bool removed = true;
    for (const auto& volume : fixed_volumes()) {
        const std::wstring quarantine = volume + kQuarantineFolder + L"\\" + id;
        if (::GetFileAttributesW(quarantine.c_str()) != INVALID_FILE_ATTRIBUTES) {
            removed = platform::delete_tree(quarantine) == ERROR_SUCCESS && removed;
        }
    }
    return platform::delete_tree(options_.root + L"\\" + id) == ERROR_SUCCESS && removed;
}

std::size_t ChangeStore::purge(std::chrono::hours max_age) {
    const auto cutoff = options_.clock() - max_age;
    std::size_t count = 0;
    for (const auto& set : list()) {
        if (set.state != SetState::Running && set.created < cutoff && remove(set.id)) {
            ++count;
        }
    }
    return count;
}

ChangeSet::~ChangeSet() = default;

bool ChangeSet::load() {
    const auto plan_text = read_file(folder_ + L"\\plan.json");
    if (!plan_text || !plan_from_json(Json::parse(*plan_text, nullptr, false), plan_, created_)) {
        return false;
    }
    statuses_.assign(plan_.items.size(), ItemStatus{});
    const auto journal = read_file(folder_ + L"\\journal.jsonl");
    if (!journal) {
        return true;  // Nothing recorded yet
    }
    std::size_t start = 0;
    while (start < journal->size()) {
        std::size_t end = journal->find('\n', start);
        if (end == std::string::npos) {
            end = journal->size();
        }
        const Json line = Json::parse(std::string_view(*journal).substr(start, end - start), nullptr, false);
        start = end + 1;
        if (!line.is_object()) {
            continue;  // A torn line
        }
        const std::size_t item = line.value("item", std::size_t{SIZE_MAX});
        ItemStatus status;
        if (item >= statuses_.size() || !parse(line.value("state", std::string()), status.state) ||
            !parse(line.value("reason", std::string("none")), status.reason)) {
            continue;
        }
        status.code = line.value("code", std::uint32_t{0});
        statuses_[item] = status;
    }
    return true;
}

bool ChangeSet::record(std::size_t item, ItemStatus status) {
    if (item >= statuses_.size()) {
        return false;
    }
    Json line;
    line["item"] = item;
    line["state"] = to_string(status.state);
    if (status.reason != Reason::None) {
        line["reason"] = to_string(status.reason);
    }
    if (status.code != 0) {
        line["code"] = status.code;
    }
    line["time"] = iso_time(store_->options().clock());
    const std::string text = line.dump() + "\n";
    DWORD written = 0;
    if (!::WriteFile(journal_.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
        written != text.size() || !::FlushFileBuffers(journal_.get())) {
        return false;
    }
    statuses_[item] = status;
    return true;
}

std::wstring ChangeSet::item_folder(std::size_t item) {
    const std::wstring items = folder_ + L"\\items";
    const std::wstring folder = items + std::format(L"\\{:04}", item);
    ::CreateDirectoryW(platform::extended(items).c_str(), nullptr);
    ::CreateDirectoryW(platform::extended(folder).c_str(), nullptr);
    return folder;
}

std::wstring ChangeSet::quarantine_path(std::size_t item, const std::wstring& volume_root) {
    const auto store_volume = platform::volume_root(store_->options().root);
    const std::wstring name = std::format(L"{:04}", item);
    if (store_volume && platform::paths_equal(*store_volume, volume_root)) {
        const std::wstring files = folder_ + L"\\files";
        ::CreateDirectoryW(platform::extended(files).c_str(), nullptr);
        return files + L"\\" + name;
    }
    auto policy = store_->options().policy;
    policy.hidden = true;
    const std::wstring root = platform::normalize_path(volume_root) + L"\\" + kQuarantineFolder;
    if (platform::ensure_secure_folder(root, policy) != platform::FolderStatus::Ok) {
        return {};
    }
    const std::wstring folder = root + L"\\" + id_;
    ::CreateDirectoryW(platform::extended(folder).c_str(), nullptr);
    return folder + L"\\" + name;
}

bool ChangeSet::write_summary(SetState state, std::uint64_t quarantined_bytes) {
    Json json;
    json["schema_version"] = kSchemaVersion;
    json["id"] = platform::to_utf8(id_);
    json["created"] = iso_time(created_);
    json["created_unix"] = unix_seconds(created_);
    put_text(json, "description", plan_.description);
    json["state"] = to_string(state);
    std::size_t done = 0;
    std::size_t skipped = 0;
    std::size_t restored = 0;
    for (const auto& status : statuses_) {
        done += status.state == ItemState::Done || status.state == ItemState::Partial;
        skipped += status.state == ItemState::Skipped || status.state == ItemState::Failed;
        restored += status.state == ItemState::Restored;
    }
    json["items"] = statuses_.size();
    json["done"] = done;
    json["skipped"] = skipped;
    json["restored"] = restored;
    json["quarantined_bytes"] = quarantined_bytes;
    return write_file_durably(folder_ + L"\\summary.json", json.dump(1, '\t'), true);
}

std::optional<SetSummary> ChangeSet::summary() const {
    auto summary = read_summary(folder_);
    if (summary) {
        summary->id = id_;
    }
    return summary;
}

}  // namespace avel::changes
