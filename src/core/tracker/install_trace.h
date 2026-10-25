// "Install and trace" (`avel-cli trace run`, later the GUI): starts a trace, launches the installer,
// waits until the installation is over (installer_watch.h) while the journals are read in the
// background, and stops the trace.
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include "core/tracker/installer_watch.h"
#include "core/tracker/trace_record.h"
#include "core/tracker/trace_session.h"
#include "core/tracker/trace_store.h"
#include "core/uninstall/process_runner.h"

namespace avel::tracker {

struct InstallTraceOptions {
    std::wstring installer;
    std::wstring arguments;
    std::wstring description;
    WatchOptions watch;  // Its `activity` is replaced by the trace session's
    std::chrono::milliseconds drain_every{2000};
    TraceSources sources = live_trace_sources();
};

enum class InstallTracePhase : std::uint8_t {
    Starting,   // The trace starts (`step`): journal positions, known values, registry snapshot
    Watching,   // The installer runs (`watch`, `running`)
    Finishing,  // The trace stops (`step`)
};

struct InstallTraceStatus {
    InstallTracePhase phase = InstallTracePhase::Starting;
    SessionStep step = SessionStep::Volumes;
    WatchPhase watch = WatchPhase::Running;
    std::span<const WatchProcess> running;  // The installer's processes, the installer first
};

using InstallTraceProgress = std::function<void(const InstallTraceStatus& status)>;

struct InstallTraceResult {
    // Why there is no trace: it couldn't start (AlreadyOpen, WriteFailed, ...), Cancelled before
    // the launch, or LaunchFailed (the trace was discarded; see watch->error).
    TraceError error = TraceError::None;
    std::optional<WatchResult> watch;  // Once the installer was launched, or failed to be
    // The trace: stopped (complete or incomplete; still `stopping` if its result couldn't be
    // saved), or open with stop_reason RebootPending when the installer asked for a restart.
    std::optional<TraceInfo> trace;
    TraceResult result;  // What it found, once stopped
};

// The watch ends the trace this way: InstallerDone stops it with InstallerDone (TimedOut and
// Stopped likewise), except that a reboot request leaves it open (RebootPending: restart, then
// stop it). Cancellation::stop_batch before the launch discards the trace and launches nothing;
// during the watch it stops the trace at once and leaves the installer running, and
// terminate_current also ends the installer's processes.
InstallTraceResult install_and_trace(TraceStore& store, const InstallTraceOptions& options,
                                     const uninstall::Cancellation& cancellation,
                                     const InstallTraceProgress& progress = {});

}  // namespace avel::tracker
