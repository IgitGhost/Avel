#include "core/tracker/trace_store.h"

#include <algorithm>
#include <cwctype>
#include <format>

#include "core/changes/serialization.h"
#include "core/platform/file_ops.h"
#include "core/platform/paths.h"
#include "core/platform/record_folders.h"
#include "core/platform/text.h"
#include "core/tracker/known_values.h"

namespace avel::tracker {

namespace {

using changes::Json;

bool is_active(TraceState state) {
    return state == TraceState::Starting || state == TraceState::Open || state == TraceState::Stopping;
}

bool is_finished(TraceState state) {
    return state == TraceState::Complete || state == TraceState::Incomplete;
}

// YYYYMMDD-HHMMSS-<8 hex digits>: purge only ever deletes folders named like traces.
bool looks_like_trace_id(std::wstring_view name) {
    if (name.size() != 24) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const wchar_t c = name[i];
        if (i == 8 || i == 15) {
            if (c != L'-') {
                return false;
            }
        } else if (i < 15 ? (c < L'0' || c > L'9') : !std::iswxdigit(c)) {
            return false;
        }
    }
    return true;
}

bool folder_exists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(platform::extended(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

struct InfoFile {
    std::optional<TraceInfo> info;
    bool other_version = false;  // Readable JSON of another schema version: left alone
};

InfoFile read_info_file(const std::wstring& folder) {
    InfoFile result;
    const auto text = changes::read_file(folder + L"\\" + std::wstring(kTraceInfoFile));
    if (!text) {
        return result;
    }
    const Json json = Json::parse(*text, nullptr, false);
    if (json.is_object() && json.contains("schema_version") &&
        json.value("schema_version", 0) != kTraceSchemaVersion) {
        result.other_version = true;
        return result;
    }
    TraceInfo info;
    if (from_json(json, info)) {
        result.info = std::move(info);
    }
    return result;
}

std::vector<std::wstring> checkpoint_files(const std::wstring& folder) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW data{};
    wil::unique_hfind find(::FindFirstFileExW(platform::extended(folder + L"\\volume-*.checkpoint").c_str(),
                                              FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0));
    if (!find) {
        return names;
    }
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            names.emplace_back(data.cFileName);
        }
    } while (::FindNextFileW(find.get(), &data));
    return names;
}

bool needs_tidying(const std::wstring& folder) {
    return ::GetFileAttributesW((folder + L"\\" + std::wstring(kSnapshotFile)).c_str()) !=
               INVALID_FILE_ATTRIBUTES ||
           !checkpoint_files(folder).empty();
}

}  // namespace

std::wstring checkpoint_file(std::size_t volume) { return std::format(L"volume-{}.checkpoint", volume); }

std::string_view to_string(TraceError error) {
    switch (error) {
        case TraceError::None:
            return "none";
        case TraceError::AlreadyOpen:
            return "already-open";
        case TraceError::NotFound:
            return "not-found";
        case TraceError::NotOpen:
            return "not-open";
        case TraceError::NotFinished:
            return "not-finished";
        case TraceError::InUse:
            return "in-use";
        case TraceError::Damaged:
            return "damaged";
        case TraceError::WriteFailed:
            return "write-failed";
        case TraceError::Busy:
            return "busy";
        case TraceError::LaunchFailed:
            return "launch-failed";
        case TraceError::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

TraceStoreOptions default_trace_store_options() {
    TraceStoreOptions options;
    options.root = platform::program_data_folder(L"Traces");
    return options;
}

std::wstring TraceFolder::file(std::wstring_view name) const { return path_ + L"\\" + std::wstring(name); }

bool TraceFolder::write_info(const TraceInfo& info) const {
    const std::string text = to_json(info).dump(1, '\t');
    // A reader in another process (`trace list`) can briefly keep the file from being replaced.
    for (int attempt = 0;; ++attempt) {
        if (changes::write_file_durably(file(kTraceInfoFile), text, true)) {
            return true;
        }
        if (attempt == 4) {
            return false;
        }
        ::Sleep(50);
    }
}

std::optional<TraceInfo> TraceFolder::read_info() const { return read_info_file(path_).info; }

TraceStore::TraceStore(TraceStoreOptions options) : options_(std::move(options)) {}

std::unique_ptr<TraceStore> TraceStore::open(TraceStoreOptions options, platform::FolderStatus& status) {
    if (options.root.empty() || !platform::is_absolute_path(options.root)) {
        status = platform::FolderStatus::Unavailable;
        return nullptr;
    }
    options.root = platform::normalize_path(options.root);
    status = platform::ensure_secure_folder(options.root, options.policy);
    if (status != platform::FolderStatus::Ok) {
        return nullptr;
    }
    std::unique_ptr<TraceStore> store(new TraceStore(std::move(options)));
    store->mutex_.reset(::CreateMutexW(nullptr, FALSE, store->options_.mutex_name.c_str()));
    if (!store->mutex_) {
        status = platform::FolderStatus::Unavailable;
        return nullptr;
    }
    return store;
}

std::optional<TraceStore::Guard> TraceStore::lock(std::chrono::milliseconds timeout) const {
    DWORD status = 0;
    auto guard = mutex_.acquire(&status, static_cast<DWORD>(timeout.count()));
    // An abandoned mutex (a crashed holder) is ours now; the traces' own states say what it left.
    if (status != WAIT_OBJECT_0 && status != WAIT_ABANDONED) {
        return std::nullopt;
    }
    return guard;
}

std::vector<TraceInfo> TraceStore::list() const {
    std::vector<TraceInfo> traces;
    for (const auto& name : platform::record_folders(options_.root)) {
        if (auto info = read_info_file(folder_of(name)).info) {
            info->id = name;
            traces.push_back(std::move(*info));
        }
    }
    std::sort(traces.begin(), traces.end(), [](const TraceInfo& a, const TraceInfo& b) {
        return a.started != b.started ? a.started > b.started : a.id > b.id;
    });
    return traces;
}

std::optional<TraceInfo> TraceStore::read(const std::wstring& id) const {
    if (!platform::is_safe_record_id(id)) {
        return std::nullopt;
    }
    auto info = read_info_file(folder_of(id)).info;
    if (info) {
        info->id = id;
    }
    return info;
}

std::optional<TraceResult> TraceStore::read_result(const std::wstring& id) const {
    if (!platform::is_safe_record_id(id)) {
        return std::nullopt;
    }
    const auto text = changes::read_file(folder_of(id) + L"\\" + std::wstring(kChangesFile));
    TraceResult result;
    if (!text || !from_json(Json::parse(*text, nullptr, false), result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<TraceInfo> TraceStore::open_trace() const {
    for (auto& info : list()) {
        if (is_active(info.state)) {
            return std::move(info);
        }
    }
    return std::nullopt;
}

std::unique_ptr<TraceFolder> TraceStore::create(TraceInfo& info, TraceError& error) {
    const auto guard = lock();
    if (!guard) {
        error = TraceError::Busy;
        return nullptr;
    }
    for (const auto& name : platform::record_folders(options_.root)) {
        const std::wstring path = folder_of(name);
        const auto existing = read_info_file(path).info;
        if (!existing || !is_active(existing->state)) {
            continue;
        }
        if (existing->state == TraceState::Starting) {
            // Died while starting: nothing in it can be stopped.
            if (auto stale = platform::take_folder_lock(path)) {
                stale.reset();
                platform::delete_tree(path);
                continue;
            }
        }
        error = TraceError::AlreadyOpen;
        return nullptr;
    }

    const auto now = options_.clock();
    auto folder = new_folder(now, error);
    if (!folder) {
        return nullptr;
    }
    info.id = folder->id_;
    info.started = now;
    info.state = TraceState::Starting;
    if (!folder->write_info(info)) {
        folder->lock_.reset();
        platform::delete_tree(folder->path_);
        error = TraceError::WriteFailed;
        return nullptr;
    }
    error = TraceError::None;
    return folder;
}

std::unique_ptr<TraceFolder> TraceStore::new_folder(std::chrono::system_clock::time_point now,
                                                    TraceError& error) {
    std::unique_ptr<TraceFolder> folder(new TraceFolder());
    for (int attempt = 0; attempt < 3 && folder->path_.empty(); ++attempt) {
        const std::wstring id = platform::new_record_id(now);
        const std::wstring path = folder_of(id);
        if (::CreateDirectoryW(platform::extended(path).c_str(), nullptr)) {
            folder->id_ = id;
            folder->path_ = path;
        } else if (::GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    if (folder->path_.empty()) {
        error = TraceError::WriteFailed;
        return nullptr;
    }
    folder->lock_ = platform::take_folder_lock(folder->path_);
    if (!folder->lock_) {
        platform::delete_tree(folder->path_);
        error = TraceError::WriteFailed;
        return nullptr;
    }
    return folder;
}

std::unique_ptr<TraceFolder> TraceStore::add_finished(TraceInfo& info, const TraceResult& result,
                                                      const KnownValues& saved, TraceError& error) {
    if (!is_finished(info.state)) {
        error = TraceError::NotFinished;
        return nullptr;
    }
    const auto guard = lock();
    if (!guard) {
        error = TraceError::Busy;
        return nullptr;
    }
    auto folder = new_folder(options_.clock(), error);
    if (!folder) {
        return nullptr;
    }
    info.id = folder->id_;
    // trace.json last: until it's there, purge takes the folder for damaged.
    if (!changes::write_file_durably(folder->file(kSavedValuesFile), to_json(saved).dump(1, '\t')) ||
        !changes::write_file_durably(folder->file(kChangesFile), to_json(result).dump(1, '\t')) ||
        !folder->write_info(info)) {
        folder->lock_.reset();
        platform::delete_tree(folder->path_);
        error = TraceError::WriteFailed;
        return nullptr;
    }
    error = TraceError::None;
    return folder;
}

std::unique_ptr<TraceFolder> TraceStore::open_folder(const std::wstring& id, TraceError& error) {
    const auto guard = lock();
    if (!guard) {
        error = TraceError::Busy;
        return nullptr;
    }
    const std::wstring path = folder_of(id);
    if (!platform::is_safe_record_id(id) || !folder_exists(path)) {
        error = TraceError::NotFound;
        return nullptr;
    }
    // A real folder at its own path; ownership was settled by the root's checks.
    if (const auto status = platform::check_secure_folder(path, {L"", {}, false});
        status != platform::FolderStatus::ForeignOwner && status != platform::FolderStatus::Ok) {
        error = TraceError::Damaged;
        return nullptr;
    }
    std::unique_ptr<TraceFolder> folder(new TraceFolder());
    folder->id_ = id;
    folder->path_ = path;
    folder->lock_ = platform::take_folder_lock(path);
    if (!folder->lock_) {
        error = TraceError::InUse;
        return nullptr;
    }
    error = TraceError::None;
    return folder;
}

std::optional<TraceInfo> TraceStore::update(const std::wstring& id,
                                            const std::function<bool(TraceInfo& info)>& change,
                                            TraceError& error) {
    const auto guard = lock();
    if (!guard) {
        error = TraceError::Busy;
        return std::nullopt;
    }
    const auto folder = open_folder(id, error);
    if (!folder) {
        return std::nullopt;
    }
    auto info = folder->read_info();
    if (!info) {
        error = TraceError::Damaged;
        return std::nullopt;
    }
    if (!is_finished(info->state)) {
        error = TraceError::NotFinished;
        return std::nullopt;
    }
    info->id = id;
    if (change(*info) && !folder->write_info(*info)) {
        error = TraceError::WriteFailed;
        return std::nullopt;
    }
    error = TraceError::None;
    return info;
}

bool TraceStore::held(const std::wstring& id) const {
    const std::wstring path = folder_of(id);
    return platform::is_safe_record_id(id) && folder_exists(path) && !platform::take_folder_lock(path);
}

bool TraceStore::remove(const std::wstring& id, TraceError& error) {
    const auto guard = lock();
    if (!guard) {
        error = TraceError::Busy;
        return false;
    }
    {
        const auto folder = open_folder(id, error);
        if (!folder) {
            return false;
        }
    }  // Releases the lock file before deleting the folder; the mutex keeps others out meanwhile
    if (platform::delete_tree(folder_of(id)) != ERROR_SUCCESS) {
        error = TraceError::WriteFailed;
        return false;
    }
    error = TraceError::None;
    return true;
}

std::size_t TraceStore::purge(const std::function<bool(TraceInfo& info)>& installed) {
    const auto guard = lock();
    if (!guard) {
        return 0;
    }
    std::size_t count = 0;
    for (const auto& name : platform::record_folders(options_.root)) {
        if (!looks_like_trace_id(name)) {
            continue;
        }
        const std::wstring path = folder_of(name);
        const InfoFile file = read_info_file(path);
        if (file.other_version) {
            continue;
        }
        TraceFolder folder;
        folder.id_ = name;
        folder.path_ = path;
        folder.lock_ = platform::take_folder_lock(path);
        if (!folder.lock_) {
            continue;  // A process drives it
        }
        const bool damaged = !file.info || file.info->state == TraceState::Starting;
        const bool finished = file.info && is_finished(file.info->state);
        if (damaged || (finished && installed && expired(folder, *file.info, installed))) {
            folder.release();
            if (platform::delete_tree(path) == ERROR_SUCCESS) {
                ++count;
            }
            continue;
        }
        if (finished && needs_tidying(path)) {
            if (const auto result = read_result(name)) {
                tidy_trace_folder(path, result->known_values);
            }
        }
    }
    return count;
}

bool TraceStore::expired(TraceFolder& folder, TraceInfo info,
                         const std::function<bool(TraceInfo& info)>& installed) {
    info.id = folder.id_;
    const auto now = options_.clock();
    if (info.links.empty()) {
        // Nothing says whether its program is still there: kept for a while after it was recorded
        // here.
        const auto since = info.imported ? info.imported->time : info.stopped.value_or(info.started);
        return now - since >= kTraceRetention;
    }
    const Json before = to_json(info);
    if (installed(info)) {
        info.unresolved_since.reset();
    } else if (!info.unresolved_since) {
        info.unresolved_since = now;
    } else if (now - *info.unresolved_since >= kTraceRetention) {
        return true;
    }
    if (to_json(info) != before) {
        folder.write_info(info);  // A failure is tried again at the next purge
    }
    return false;
}

void tidy_trace_folder(const std::wstring& folder, const std::vector<KnownValueChange>& known) {
    // saved_values.json first, the snapshot last: while the snapshot exists, tidying isn't done.
    const std::wstring saved_path = folder + L"\\" + std::wstring(kSavedValuesFile);
    if (const auto text = changes::read_file(saved_path)) {
        KnownValues saved;
        if (from_json(Json::parse(*text, nullptr, false), saved)) {
            KnownValues kept;
            for (const KnownKey& key : saved.keys) {
                KnownKey reduced{key.spot, key.key, {}, key.present};
                bool changed = false;
                for (const auto& change : known) {
                    if (change.spot != key.spot || !platform::same_address(change.key, key.key)) {
                        continue;
                    }
                    changed = true;
                    for (const auto& value : key.values) {
                        if (platform::iequals(value.name, change.name) &&
                            std::none_of(reduced.values.begin(), reduced.values.end(),
                                         [&](const auto& added) {
                                             return platform::iequals(added.name, value.name);
                                         })) {
                            reduced.values.push_back(value);
                        }
                    }
                }
                if (changed) {
                    kept.keys.push_back(std::move(reduced));
                }
            }
            changes::write_file_durably(saved_path, to_json(kept).dump(1, '\t'), true);
        }
    }
    for (const auto& name : checkpoint_files(folder)) {
        ::DeleteFileW(platform::extended(folder + L"\\" + name).c_str());
    }
    ::DeleteFileW(platform::extended(folder + L"\\" + std::wstring(kSnapshotFile)).c_str());
}

}  // namespace avel::tracker
