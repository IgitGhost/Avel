#include "core/tracker/trace_session.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <string_view>

#include "core/changes/serialization.h"
#include "core/leftovers/environment.h"
#include "core/platform/file_ops.h"
#include "core/platform/known_folders.h"
#include "core/platform/paths.h"
#include "core/platform/user_profiles.h"

namespace avel::tracker {

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ms(Clock::time_point since) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
}

std::int64_t precise_now() {
    FILETIME now{};
    ::GetSystemTimePreciseAsFileTime(&now);
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(now.dwHighDateTime) << 32) |
                                     now.dwLowDateTime);
}

std::optional<QuietFolder> quiet_folder(const std::wstring& path) {
    const auto folder = platform::open_item(path, FILE_READ_ATTRIBUTES);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!folder || !::GetFileInformationByHandle(folder.get(), &information) ||
        !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return std::nullopt;
    }
    return QuietFolder{
        information.dwVolumeSerialNumber,
        (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow};
}

std::string_view bytes_of(const std::vector<std::byte>& data) {
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

std::span<const std::byte> bytes_of(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// The noise rules' environment, with users' hives from the cache the stop shares: a signed-out
// user's hive can be loaded privately only once per process, so LiveEnvironment can't load it a
// second time while the registry diff holds it.
NoiseRules live_noise_rules(const std::vector<std::wstring>& avel_paths, UserHiveCache& hives) {
    leftovers::ScanEnvironment environment;
    environment.machine_folders = platform::machine_folders();
    environment.current_user_sid = platform::current_user_sid();
    for (auto& profile : platform::enumerate_user_profiles()) {
        leftovers::UserScope user;
        const auto hive = hives.open(profile.sid, false);
        user.hive = hive ? hive->key.get() : nullptr;
        user.folders = platform::user_folders(user.hive, profile);
        user.profile = std::move(profile);
        environment.users.push_back(std::move(user));
    }
    return default_noise_rules(environment, avel_paths);
}

void report(const SessionProgress& progress, SessionStep step) {
    if (progress) {
        progress(step);
    }
}

}  // namespace

TraceSources live_trace_sources() {
    TraceSources sources;
    sources.start_volumes = [] { return start_volumes(); };
    sources.drain_volume = [](VolumeTrace& volume, FileChangeAggregator& changes,
                              const TraceSources::Keep& keep) { drain_volume(volume, changes, keep); };
    sources.resolve_volume = [](const VolumeTrace& volume, const FileChangeAggregator& changes,
                                std::int64_t trace_start) {
        return resolve_volume(volume, changes, trace_start);
    };
    sources.quiet_folders = live_quiet_folders;
    sources.snapshot_roots = [] { return live_snapshot_roots(); };
    sources.snapshot_roots_at_stop = [](const RegistrySnapshot& started, UserHiveCache& hives) {
        return live_snapshot_roots(started, hives);
    };
    sources.known_roots = [] { return live_known_roots(); };
    sources.known_roots_at_stop = [](const KnownValues& started, UserHiveCache& hives) {
        return live_known_roots(started, hives);
    };
    sources.noise_rules = live_noise_rules;
    return sources;
}

std::vector<QuietFolder> live_quiet_folders() {
    std::vector<std::wstring> paths;
    const auto machine = platform::machine_folders();
    if (!machine.windows.empty()) {
        for (const std::wstring_view folder :
             {L"Temp", L"SystemTemp", L"Prefetch", L"Logs", LR"(System32\LogFiles)",
              LR"(System32\winevt\Logs)", LR"(System32\config)", LR"(System32\sru)"}) {
            paths.push_back(machine.windows + L"\\" + std::wstring(folder));
        }
    }
    if (!machine.program_data.empty()) {
        for (const std::wstring_view folder :
             {LR"(Microsoft\Windows Defender\Support)", LR"(Microsoft\Windows Defender\Scans)", L"Avel"}) {
            paths.push_back(machine.program_data + L"\\" + std::wstring(folder));
        }
    }
    for (const auto& profile : platform::enumerate_user_profiles()) {
        // The profile's own folder and Local\Microsoft\Windows hold the user's hives, which the
        // registry flushes whenever something writes to HKEY_CURRENT_USER.
        paths.push_back(profile.profile_path);
        paths.push_back(profile.profile_path + LR"(\AppData\Local\Microsoft\Windows)");
        paths.push_back(profile.profile_path + LR"(\AppData\Local\Temp)");
    }
    wchar_t temp[MAX_PATH + 1] = {};
    if (::GetTempPathW(MAX_PATH + 1, temp) > 0) {
        paths.push_back(platform::normalize_path(temp));
    }
    std::vector<QuietFolder> folders;
    for (const auto& path : paths) {
        if (const auto folder = quiet_folder(path)) {
            folders.push_back(*folder);
        }
    }
    return folders;
}

std::string_view to_string(SessionStep step) {
    switch (step) {
        case SessionStep::Volumes:
            return "volumes";
        case SessionStep::KnownValues:
            return "known-values";
        case SessionStep::Snapshot:
            return "snapshot";
        case SessionStep::Draining:
            return "draining";
        case SessionStep::Files:
            return "files";
        case SessionStep::Registry:
            return "registry";
        case SessionStep::Saving:
            return "saving";
    }
    return "unknown";
}

TraceSession::TraceSession(TraceStore& store, TraceSources sources)
    : store_(store), sources_(std::move(sources)) {}

TraceSession::~TraceSession() { stop_draining(); }

std::unique_ptr<TraceSession> TraceSession::start(TraceStore& store, const StartOptions& options,
                                                  TraceError& error, const SessionProgress& progress,
                                                  TraceSources sources) {
    std::unique_ptr<TraceSession> session(new TraceSession(store, std::move(sources)));
    TraceInfo& info = session->info_;
    info.mode = options.mode;
    info.description = options.description;
    info.command = options.command;
    session->folder_ = store.create(info, error);
    if (!session->folder_) {
        return nullptr;
    }
    const auto fail = [&](TraceError reason) {
        error = reason;
        session->discard();
        return nullptr;
    };

    info.trace_start = precise_now();
    report(progress, SessionStep::Volumes);
    session->volumes_ = session->sources_.start_volumes();
    const std::size_t count = session->volumes_.size();
    session->changes_.resize(count);
    session->saved_usn_.assign(count, 0);
    session->saved_status_.assign(count, JournalStatus::Ok);
    for (std::size_t i = 0; i < count; ++i) {
        if (!session->write_checkpoint(i)) {
            return fail(TraceError::WriteFailed);
        }
    }
    info.volumes = session->volumes_;

    report(progress, SessionStep::KnownValues);
    {
        const auto roots = session->sources_.known_roots();
        const KnownValues known = capture_known_values(roots);
        if (!changes::write_file_durably(session->folder_->file(kSavedValuesFile),
                                         to_json(known).dump(1, '\t'))) {
            return fail(TraceError::WriteFailed);
        }
    }

    report(progress, SessionStep::Snapshot);
    {
        const auto started = Clock::now();
        const auto roots = session->sources_.snapshot_roots();
        std::vector<SnapshotDiagnostic> diagnostics;
        const RegistrySnapshot snapshot =
            take_registry_snapshot(roots, session->sources_.snapshot, diagnostics);
        if (!write_snapshot(snapshot, session->folder_->file(kSnapshotFile))) {
            return fail(TraceError::WriteFailed);
        }
        info.registry.keys = snapshot.keys().size();
        info.registry.values = snapshot.values().size();
        info.registry.snapshot_ms = elapsed_ms(started);
        info.registry.diagnostics = std::move(diagnostics);
    }

    info.state = TraceState::Open;
    if (!session->write_info()) {
        return fail(TraceError::WriteFailed);
    }
    session->load_quiet_folders();
    error = TraceError::None;
    return session;
}

std::unique_ptr<TraceSession> TraceSession::resume(TraceStore& store, const std::wstring& id,
                                                   TraceError& error, TraceSources sources) {
    std::unique_ptr<TraceSession> session(new TraceSession(store, std::move(sources)));
    session->folder_ = store.open_folder(id, error);
    if (!session->folder_) {
        return nullptr;
    }
    auto info = session->folder_->read_info();
    if (!info) {
        error = TraceError::Damaged;
        return nullptr;
    }
    if (info->state != TraceState::Open && info->state != TraceState::Stopping) {
        error = TraceError::NotOpen;
        return nullptr;
    }
    info->id = id;
    session->info_ = std::move(*info);

    std::vector<std::size_t> damaged;
    for (std::size_t i = 0; i < session->info_.volumes.size(); ++i) {
        VolumeTrace volume = session->info_.volumes[i];
        FileChangeAggregator changes;
        const auto data = changes::read_file(session->folder_->file(checkpoint_file(i)));
        if (!data || !load_checkpoint(bytes_of(*data), volume, changes)) {
            // What was read from this volume is lost; it can't be followed further either.
            volume = session->info_.volumes[i];
            volume.status = JournalStatus::Unreadable;
            volume.error = ERROR_FILE_CORRUPT;
            changes = {};
            session->info_.diagnostics.push_back({TraceDiagnosticCode::CheckpointDamaged, volume.root});
            damaged.push_back(i);
        }
        session->saved_usn_.push_back(volume.next_usn);
        session->saved_status_.push_back(volume.status);
        session->volumes_.push_back(std::move(volume));
        session->changes_.push_back(std::move(changes));
    }
    if (!damaged.empty()) {
        // Settle it on disk, so the volume stays unreadable whatever happens next.
        for (const std::size_t i : damaged) {
            session->write_checkpoint(i);
        }
        session->write_info();
    }
    session->load_quiet_folders();
    error = TraceError::None;
    return session;
}

const std::wstring& TraceSession::folder() const { return folder_->path(); }

bool TraceSession::write_info() {
    info_.volumes = volumes_;
    return folder_->write_info(info_);
}

bool TraceSession::write_checkpoint(std::size_t volume) {
    const auto data = save_checkpoint(volumes_[volume], changes_[volume]);
    if (!changes::write_file_durably(folder_->file(checkpoint_file(volume)), bytes_of(data), true)) {
        return false;
    }
    saved_usn_[volume] = volumes_[volume].next_usn;
    saved_status_[volume] = volumes_[volume].status;
    return true;
}

void TraceSession::drain(bool checkpoint) {
    for (std::size_t i = 0; i < volumes_.size(); ++i) {
        if (volumes_[i].status == JournalStatus::Ok) {
            std::size_t active = 0;
            auto& quiet = quiet_[i];
            sources_.drain_volume(volumes_[i], changes_[i], [&](const platform::UsnRecord& record) {
                if (!quiet.contains(record.parent_frn)) {
                    ++active;
                } else if ((record.attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                           (record.reason & USN_REASON_FILE_CREATE)) {
                    quiet.insert(record.frn);  // A folder created in a quiet folder is quiet too
                }
                return true;
            });
            activity_ += active;
        }
        if (checkpoint && (volumes_[i].next_usn != saved_usn_[i] || volumes_[i].status != saved_status_[i])) {
            // A failure is tried again at the next poll.
            write_checkpoint(i);
        }
    }
}

void TraceSession::load_quiet_folders() {
    std::vector<QuietFolder> folders =
        sources_.quiet_folders ? sources_.quiet_folders() : std::vector<QuietFolder>();
    // The trace's own checkpoints are written every few seconds.
    for (const auto& path : {folder_->path(), store_.options().root}) {
        if (const auto folder = quiet_folder(path)) {
            folders.push_back(*folder);
        }
    }
    std::lock_guard lock(mutex_);
    quiet_.assign(volumes_.size(), {});
    for (const auto& folder : folders) {
        for (std::size_t i = 0; i < volumes_.size(); ++i) {
            if (volumes_[i].serial == folder.serial) {
                quiet_[i].insert(folder.frn);
            }
        }
    }
}

std::vector<std::wstring> TraceSession::avel_paths() const {
    std::vector<std::wstring> paths;
    // %ProgramData%\Avel, which holds the traces, the change sets and their backups.
    if (std::wstring data = platform::parent_path(store_.options().root); !data.empty()) {
        paths.push_back(std::move(data));
    }
    wchar_t module[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        if (std::wstring folder = platform::parent_path(module); !folder.empty()) {
            paths.push_back(std::move(folder));
        }
    }
    return paths;
}

void TraceSession::discard() {
    discarded_ = true;
    if (folder_) {
        const auto guard = store_.lock();  // Nobody opens the trace between releasing and deleting it
        folder_->release();
        platform::delete_tree(folder_->path());
    }
}

void TraceSession::poll() {
    std::lock_guard lock(mutex_);
    if (!discarded_ && info_.state == TraceState::Open) {
        drain(true);
    }
}

std::size_t TraceSession::activity() {
    {
        std::lock_guard lock(mutex_);
        if (!discarded_ && info_.state == TraceState::Open) {
            drain(false);
        }
    }
    return activity_.exchange(0);
}

void TraceSession::start_draining(std::chrono::milliseconds every) {
    stop_draining();
    {
        std::lock_guard lock(drainer_mutex_);
        drainer_stop_ = false;
    }
    drainer_ = std::thread([this, every] {
        std::unique_lock lock(drainer_mutex_);
        while (!drainer_wake_.wait_for(lock, every, [this] { return drainer_stop_; })) {
            lock.unlock();
            poll();
            lock.lock();
        }
    });
}

void TraceSession::stop_draining() {
    if (!drainer_.joinable()) {
        return;
    }
    {
        std::lock_guard lock(drainer_mutex_);
        drainer_stop_ = true;
    }
    drainer_wake_.notify_all();
    drainer_.join();
}

void TraceSession::set_watch(const WatchResult& watch) {
    std::lock_guard lock(mutex_);
    info_.watch = watch;
}

bool TraceSession::keep_open(StopReason reason) {
    std::lock_guard lock(mutex_);
    if (discarded_ || info_.state != TraceState::Open) {
        return false;
    }
    drain(true);
    info_.stop_reason = reason;
    return write_info();
}

TraceResult TraceSession::stop(StopReason reason, const SessionProgress& progress) {
    stop_draining();
    std::lock_guard lock(mutex_);
    if (result_) {
        return *result_;
    }
    if (discarded_ || (info_.state != TraceState::Open && info_.state != TraceState::Stopping)) {
        return {};
    }
    if (info_.state == TraceState::Open) {
        report(progress, SessionStep::Draining);
        drain(true);
        info_.state = TraceState::Stopping;
        info_.stop_reason = reason == StopReason::None ? StopReason::Manual : reason;
        info_.stopped = store_.options().clock();
        // A failure here isn't fatal: until stopping is on disk, a crash leaves the trace open.
        write_info();
    }

    // From here on, everything comes from the trace's files and the live system.
    const std::vector<std::wstring> avel = avel_paths();
    UserHiveCache hives;  // Shared by the noise rules, the registry diff and the known values
    const NoiseRules rules = sources_.noise_rules ? sources_.noise_rules(avel, hives) : NoiseRules{};
    std::erase_if(info_.diagnostics, [](const TraceDiagnostic& diagnostic) {
        return diagnostic.code != TraceDiagnosticCode::CheckpointDamaged;
    });

    report(progress, SessionStep::Files);
    TraceResult result;
    for (std::size_t i = 0; i < volumes_.size(); ++i) {
        auto changes = sources_.resolve_volume(volumes_[i], changes_[i], info_.trace_start);
        std::move(changes.begin(), changes.end(), std::back_inserter(result.changes));
    }

    report(progress, SessionStep::Registry);
    info_.registry.diagnostics.clear();
    const auto diff_started = Clock::now();
    if (const auto snapshot = read_snapshot(folder_->file(kSnapshotFile))) {
        const auto roots = sources_.snapshot_roots_at_stop(*snapshot, hives);
        auto changes = diff_registry(*snapshot, roots, sources_.snapshot, info_.registry.diagnostics);
        std::move(changes.begin(), changes.end(), std::back_inserter(result.changes));
    } else {
        info_.diagnostics.push_back({TraceDiagnosticCode::SnapshotDamaged, {}});
    }
    info_.registry.diff_ms = elapsed_ms(diff_started);
    KnownValues before;
    if (const auto text = changes::read_file(folder_->file(kSavedValuesFile));
        text && from_json(changes::Json::parse(*text, nullptr, false), before)) {
        const auto roots = sources_.known_roots_at_stop(before, hives);
        result.known_values = diff_known_values(before, capture_known_values(roots));
    } else {
        info_.diagnostics.push_back({TraceDiagnosticCode::SavedValuesDamaged, {}});
    }

    apply_noise(rules, result.changes);
    info_.counts = {};
    for (const auto& change : result.changes) {
        ++(change.noise == NoiseReason::None ? info_.counts.changes : info_.counts.noise);
    }

    report(progress, SessionStep::Saving);
    if (!changes::write_file_durably(folder_->file(kChangesFile), to_json(result).dump(1, '\t'), true)) {
        return result;  // Still stopping: stop() can try again
    }
    info_.state = incomplete_reasons(info_).empty() ? TraceState::Complete : TraceState::Incomplete;
    if (!write_info()) {
        info_.state = TraceState::Stopping;
        return result;
    }
    // Only once the finished state is on disk; purge finishes this if the process dies first.
    tidy_trace_folder(folder_->path(), result.known_values);
    result_ = result;
    return result;
}

bool TraceSession::cancel() {
    if (discarded_) {
        return false;
    }
    stop_draining();
    std::lock_guard lock(mutex_);  // Waits out an activity() on another thread
    discard();
    return ::GetFileAttributesW(platform::extended(folder_->path()).c_str()) == INVALID_FILE_ATTRIBUTES;
}

}  // namespace avel::tracker
