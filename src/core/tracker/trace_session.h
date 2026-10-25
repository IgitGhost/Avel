// A trace from its start to its stop (ADR 0002). Starting records each volume's journal position,
// saves the known spots' values and takes the registry snapshot, all on disk before the trace
// counts as open, so it survives a restart and can be stopped by another process. While it is
// open, whoever drives it (`trace run`, the GUI) reads the journals every few seconds, so records
// are collected before the journal can overwrite them. Stopping reads the journals to the end and
// compares everything with the start.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "core/tracker/file_changes.h"
#include "core/tracker/journal_trace.h"
#include "core/tracker/known_values.h"
#include "core/tracker/noise_filter.h"
#include "core/tracker/registry_snapshot.h"
#include "core/tracker/trace_record.h"
#include "core/tracker/trace_store.h"

namespace avel::tracker {

// A folder whose direct children's journal records don't count as activity (see
// TraceSession::activity), by its volume's serial number and its file reference number.
struct QuietFolder {
    std::uint32_t serial = 0;
    std::uint64_t frn = 0;
};

// Where a trace reads the system. live_trace_sources() reads this machine; tests use scratch
// registry keys and made-up journals instead, so they run without administrator rights.
struct TraceSources {
    using Keep = std::function<bool(const platform::UsnRecord&)>;

    // Files: start_volumes, drain_volume and resolve_volume of journal_trace.h.
    std::function<std::vector<VolumeTrace>()> start_volumes;
    std::function<void(VolumeTrace& volume, FileChangeAggregator& changes, const Keep& keep)> drain_volume;
    std::function<std::vector<TraceChange>(const VolumeTrace& volume, const FileChangeAggregator& changes,
                                           std::int64_t trace_start)>
        resolve_volume;
    // The folders where Windows writes all the time (its Temp, Prefetch, logs, ...); the trace's
    // own folder is added to them.
    std::function<std::vector<QuietFolder>()> quiet_folders;
    // The registry snapshot's roots at the start, and the same roots at the stop.
    std::function<std::vector<SnapshotRoot>()> snapshot_roots;
    std::function<std::vector<SnapshotRoot>(const RegistrySnapshot& started, UserHiveCache& hives)>
        snapshot_roots_at_stop;
    // The known spots at the start, and the same keys at the stop.
    std::function<std::vector<KnownRoot>()> known_roots;
    std::function<std::vector<KnownRoot>(const KnownValues& started, UserHiveCache& hives)>
        known_roots_at_stop;
    // The noise rules, given Avel's own folders; users' hives come from the cache the stop shares.
    std::function<NoiseRules(const std::vector<std::wstring>& avel_paths, UserHiveCache& hives)> noise_rules;
    SnapshotOptions snapshot;
};

// This machine: every fixed NTFS volume, the snapshot roots and known spots of every signed-in
// user, and the default noise rules for this machine's folders and users.
TraceSources live_trace_sources();

// The live quiet folders: Windows' Temp, SystemTemp, Prefetch, Logs, LogFiles, winevt\Logs,
// config and sru; Defender's Support and Scans folders and Avel's own data; every user's Temp,
// profile folder and Local\Microsoft\Windows (where their hives are).
std::vector<QuietFolder> live_quiet_folders();

struct StartOptions {
    std::wstring description;
    TraceMode mode = TraceMode::Manual;
    std::wstring command;  // Launch: the installer and its arguments
};

enum class SessionStep : std::uint8_t {
    Volumes,      // Start: recording the journals' positions
    KnownValues,  // Start: saving the known spots' values
    Snapshot,     // Start: the registry snapshot (a few seconds)
    Draining,     // Stop: reading the journals to the end
    Files,        // Stop: finding the file changes' paths
    Registry,     // Stop: comparing the registry and the known values with the start
    Saving,       // Stop: writing the result
};

std::string_view to_string(SessionStep step);

using SessionProgress = std::function<void(SessionStep step)>;

class TraceSession {
public:
    ~TraceSession();
    TraceSession(const TraceSession&) = delete;
    TraceSession& operator=(const TraceSession&) = delete;

    // Starts a trace: creates its folder (state `starting`), records the time and the journals'
    // positions with a checkpoint per volume, saves the known values, takes and writes the registry
    // snapshot, then marks it open. Fails with AlreadyOpen while another trace is open, and with
    // WriteFailed (its folder removed again) if a file can't be written. The session holds the
    // trace's lock file until it is destroyed.
    static std::unique_ptr<TraceSession> start(TraceStore& store, const StartOptions& options,
                                               TraceError& error, const SessionProgress& progress = {},
                                               TraceSources sources = live_trace_sources());

    // Continues an open trace from another process or after a restart, from its checkpoints. A
    // trace left `stopping` resumes as such, and stop() finishes it with the reason it recorded.
    // Fails with InUse if another process holds it, NotOpen if it's finished or died while
    // starting (only removing it is left), Damaged if trace.json can't be read. A volume whose
    // checkpoint is damaged counts as unreadable from then on.
    static std::unique_ptr<TraceSession> resume(TraceStore& store, const std::wstring& id, TraceError& error,
                                                TraceSources sources = live_trace_sources());

    // Reads what every volume's journal recorded since the last time and writes the checkpoints
    // of the volumes that moved on. Safe to call from another thread than stop().
    void poll();

    // The journal records since the previous call that aren't NTFS metadata and aren't directly
    // inside a quiet folder (or a folder created in one during the trace): Install and trace's
    // sign that the disk has settled. A heuristic only: writes deeper inside a quiet folder that
    // existed before still count. Reads the journals first, without writing checkpoints.
    std::size_t activity();

    // A background thread calling poll() every `every` until stop_draining(), stop() or the
    // session's end.
    void start_draining(std::chrono::milliseconds every = std::chrono::seconds(2));
    void stop_draining();

    // Install and trace: the watch's result, written to trace.json with the next update.
    void set_watch(const WatchResult& watch);

    // Leaves the trace open with `reason` recorded (RebootPending): drains, checkpoints and writes
    // trace.json. False if the trace isn't open or trace.json can't be written.
    bool keep_open(StopReason reason);

    // Stops the trace: the last drain and checkpoints, state `stopping`; then the file changes'
    // paths, the registry diff, the known values diff, the noise filter, changes.json, and the
    // state `complete` or `incomplete`; then the snapshot and checkpoints are deleted. Everything
    // after `stopping` is read from the trace's files, so if the process dies, stopping again
    // repeats it (without draining). If the result couldn't be saved, info().state stays
    // `stopping` and stop() can be called again. Once stopped, returns the same result again.
    TraceResult stop(StopReason reason, const SessionProgress& progress = {});

    // Discards the trace: deletes its folder. The session can't be used afterwards.
    bool cancel();

    // The trace as last written; read it on the thread that drives the session.
    const TraceInfo& info() const { return info_; }
    const std::wstring& folder() const;

private:
    TraceSession(TraceStore& store, TraceSources sources);

    bool write_info();
    bool write_checkpoint(std::size_t volume);
    void drain(bool checkpoint);
    void load_quiet_folders();
    std::vector<std::wstring> avel_paths() const;
    void discard();

    TraceStore& store_;
    TraceSources sources_;
    std::unique_ptr<TraceFolder> folder_;
    TraceInfo info_;
    bool discarded_ = false;
    std::optional<TraceResult> result_;

    // Guarded by mutex_.
    std::mutex mutex_;
    std::vector<VolumeTrace> volumes_;
    std::vector<FileChangeAggregator> changes_;
    std::vector<std::int64_t> saved_usn_;  // Each volume's next_usn in its checkpoint
    std::vector<JournalStatus> saved_status_;
    std::vector<std::unordered_set<std::uint64_t>> quiet_;  // Per volume
    std::atomic<std::size_t> activity_{0};

    std::thread drainer_;
    std::mutex drainer_mutex_;
    std::condition_variable drainer_wake_;
    bool drainer_stop_ = false;
};

}  // namespace avel::tracker
