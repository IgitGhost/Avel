// Where traces live: one folder per trace under %ProgramData%\Avel\Traces, the way the
// ChangeStore keeps ChangeSets (ADR 0004). A trace folder holds:
//   trace.json            TraceInfo, atomically replaced on every update
//   registry.snapshot     the registry at the start (deleted once the trace is finished)
//   saved_values.json     the known spots' values at the start (after stop: only those that changed)
//   volume-<n>.checkpoint each volume's journal position and changes (deleted once finished)
//   changes.json          TraceResult, once stopped
//   lock                  held open by the process driving the trace
// Only one trace may be starting, open or stopping at a time.
#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/platform/secure_folder.h"
#include "core/tracker/trace_record.h"

namespace avel::tracker {

inline constexpr std::wstring_view kTraceInfoFile = L"trace.json";
inline constexpr std::wstring_view kSnapshotFile = L"registry.snapshot";
inline constexpr std::wstring_view kSavedValuesFile = L"saved_values.json";
inline constexpr std::wstring_view kChangesFile = L"changes.json";

// volume-<n>.checkpoint, for the volume at index `volume` of TraceInfo::volumes.
std::wstring checkpoint_file(std::size_t volume);

// How long a finished trace is kept once its links lead to no installed program, and an unlinked
// one after it stopped (or was imported).
inline constexpr std::chrono::hours kTraceRetention = std::chrono::days(30);

struct KnownValues;

enum class TraceError : std::uint8_t {
    None,
    AlreadyOpen,   // Another trace is starting, open or stopping
    NotFound,      // No trace has that id
    NotOpen,       // The trace is finished (or damaged while starting)
    NotFinished,   // The trace is still starting, open or stopping
    InUse,         // Another process drives the trace (it holds its lock file)
    Damaged,       // The trace's files can't be read
    WriteFailed,   // A file of the trace couldn't be written
    Busy,          // The store's mutex couldn't be taken in time
    LaunchFailed,  // Install and trace: the installer couldn't be started (see WatchResult::error)
    Cancelled,     // Install and trace: stopped before the installer was launched
};

std::string_view to_string(TraceError error);

struct TraceStoreOptions {
    std::wstring root;  // Holds one folder per trace
    // Serializes creating, opening and removing traces across processes.
    std::wstring mutex_name = LR"(Global\Avel.TraceStore)";
    platform::FolderPolicy policy = platform::admin_only_folder_policy();
    // Tests set the time traces are created at, and the time retention counts from.
    std::function<std::chrono::system_clock::time_point()> clock = [] {
        return std::chrono::system_clock::now();
    };
};

// %ProgramData%\Avel\Traces, SYSTEM and Administrators only.
TraceStoreOptions default_trace_store_options();

// One trace's folder, held exclusively (its lock file is open) for as long as this object lives.
class TraceFolder {
public:
    const std::wstring& id() const { return id_; }
    const std::wstring& path() const { return path_; }
    // <folder>\<name>.
    std::wstring file(std::wstring_view name) const;

    // trace.json, replaced atomically and flushed.
    bool write_info(const TraceInfo& info) const;
    std::optional<TraceInfo> read_info() const;

    // Closes the lock file, so another process can open the trace.
    void release() { lock_.reset(); }

private:
    friend class TraceStore;
    TraceFolder() = default;

    std::wstring id_;
    std::wstring path_;
    wil::unique_hfile lock_;
};

class TraceStore {
public:
    // Creates the root if needed and checks it (see platform::check_secure_folder). Returns null,
    // with the reason in `status`, if it can't be trusted.
    static std::unique_ptr<TraceStore> open(TraceStoreOptions options, platform::FolderStatus& status);

    // The store's mutex. create, open_folder, remove and purge take it themselves for as long as
    // they run; callers can hold it around more (it is recursive within a thread).
    using Guard = wil::mutex_release_scope_exit;
    std::optional<Guard> lock(std::chrono::milliseconds timeout = std::chrono::seconds(30)) const;

    // Every readable trace, newest first.
    std::vector<TraceInfo> list() const;
    std::optional<TraceInfo> read(const std::wstring& id) const;
    // changes.json; nullopt before the trace is stopped or if it's damaged.
    std::optional<TraceResult> read_result(const std::wstring& id) const;
    // The trace that is starting, open or stopping, if there is one. A `starting` trace that no
    // process holds is damaged: it can only be removed.
    std::optional<TraceInfo> open_trace() const;

    // Creates a trace folder with trace.json in state `starting` and holds it. Fills in the id,
    // `started` (the store's clock) and the state of `info`. Fails with AlreadyOpen while another
    // trace is starting, open or stopping (a damaged `starting` one is removed first).
    std::unique_ptr<TraceFolder> create(TraceInfo& info, TraceError& error);

    // Adds a finished trace recorded elsewhere (`trace import`): a new folder and id (from the
    // store's clock, filled into `info`) with saved_values.json, changes.json, and trace.json last,
    // as `info` says otherwise. NotFinished if `info` isn't finished; WriteFailed (the folder
    // removed again) if a file can't be written. Allowed while another trace is open.
    std::unique_ptr<TraceFolder> add_finished(TraceInfo& info, const TraceResult& result,
                                              const KnownValues& saved, TraceError& error);

    // Holds an existing trace's folder. InUse if another process holds it, NotFound if there is
    // no such trace.
    std::unique_ptr<TraceFolder> open_folder(const std::wstring& id, TraceError& error);

    // Reads a finished trace's trace.json, lets `change` edit it, and writes it back if `change`
    // returns true; returns the trace as it is now. Fails with NotFound, InUse, Damaged,
    // NotFinished or WriteFailed.
    std::optional<TraceInfo> update(const std::wstring& id,
                                    const std::function<bool(TraceInfo& info)>& change, TraceError& error);

    // Whether a process holds the trace's lock file.
    bool held(const std::wstring& id) const;

    // Deletes a trace now, in whatever state, unless a process holds it (InUse).
    bool remove(const std::wstring& id, TraceError& error);

    // Removes traces that died while starting (no process holds them) and trace folders whose
    // trace.json is gone or unreadable (folders not named like a trace, and trace.json files of
    // another schema version, are left alone), and finishes tidying finished traces whose process
    // died before deleting their snapshot and checkpoints. Open, stopping and held traces are
    // never touched. Returns how many traces were deleted.
    //
    // With `installed`, it also applies retention to every finished trace. `installed` says
    // whether the trace's links lead to an installed program now, and may bring the links up to
    // date (refresh_links in linking.h); trace.json is rewritten when the trace changed.
    //   - Linked: the first time its links lead nowhere, `unresolved_since` is set to now (the
    //     store's clock); once they lead somewhere again, it's cleared. kTraceRetention after
    //     `unresolved_since`, the trace is deleted.
    //   - Unlinked: deleted kTraceRetention after it stopped, or after it was imported.
    std::size_t purge(const std::function<bool(TraceInfo& info)>& installed = {});

    std::wstring folder_of(const std::wstring& id) const { return options_.root + L"\\" + id; }
    const TraceStoreOptions& options() const { return options_; }

private:
    explicit TraceStore(TraceStoreOptions options);

    // A new, empty trace folder with a fresh id, its lock file held. Under the mutex.
    std::unique_ptr<TraceFolder> new_folder(std::chrono::system_clock::time_point now, TraceError& error);
    // Retention for one finished trace whose folder is held: true if it's to be deleted.
    bool expired(TraceFolder& folder, TraceInfo info, const std::function<bool(TraceInfo& info)>& installed);

    TraceStoreOptions options_;
    wil::unique_mutex_nothrow mutex_;
};

// What a finished trace no longer needs: the registry snapshot and the checkpoints are deleted, and
// saved_values.json keeps only the values `known` says changed. Safe to repeat.
void tidy_trace_folder(const std::wstring& folder, const std::vector<KnownValueChange>& known);

}  // namespace avel::tracker
