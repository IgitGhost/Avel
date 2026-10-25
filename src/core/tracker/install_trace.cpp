#include "core/tracker/install_trace.h"

namespace avel::tracker {

InstallTraceResult install_and_trace(TraceStore& store, const InstallTraceOptions& options,
                                     const uninstall::Cancellation& cancellation,
                                     const InstallTraceProgress& progress) {
    const auto report_step = [&](InstallTracePhase phase) {
        return [&progress, phase](SessionStep step) {
            if (progress) {
                InstallTraceStatus status;
                status.phase = phase;
                status.step = step;
                progress(status);
            }
        };
    };

    InstallTraceResult result;
    StartOptions start;
    start.description = options.description;
    start.mode = TraceMode::Launch;
    start.command =
        options.arguments.empty() ? options.installer : options.installer + L" " + options.arguments;
    const auto session = TraceSession::start(store, start, result.error,
                                             report_step(InstallTracePhase::Starting), options.sources);
    if (!session) {
        return result;
    }
    if (cancellation.stop_batch) {
        session->cancel();
        result.error = TraceError::Cancelled;
        return result;
    }

    session->start_draining(options.drain_every);
    WatchOptions watch = options.watch;
    watch.activity = [&session] { return session->activity(); };
    result.watch = launch_and_watch(options.installer, options.arguments, watch, cancellation,
                                    [&progress](WatchPhase phase, std::span<const WatchProcess> running) {
                                        if (progress) {
                                            InstallTraceStatus status;
                                            status.phase = InstallTracePhase::Watching;
                                            status.watch = phase;
                                            status.running = running;
                                            progress(status);
                                        }
                                    });
    session->stop_draining();

    if (result.watch->end == WatchEnd::LaunchFailed) {
        session->cancel();
        result.error = TraceError::LaunchFailed;
        return result;
    }
    session->set_watch(*result.watch);
    StopReason reason = StopReason::InstallerDone;
    switch (result.watch->end) {
        case WatchEnd::Stopped:
            reason = StopReason::Stopped;  // The user asked; a restart request doesn't change that
            break;
        case WatchEnd::TimedOut:
            reason = result.watch->reboot_requested ? StopReason::RebootPending : StopReason::TimedOut;
            break;
        default:
            reason = result.watch->reboot_requested ? StopReason::RebootPending : StopReason::InstallerDone;
            break;
    }
    if (reason == StopReason::RebootPending) {
        // What the installer still does at the restart belongs to the trace: it stays open.
        if (!session->keep_open(reason)) {
            result.error = TraceError::WriteFailed;
        }
        result.trace = session->info();
        return result;
    }
    result.result = session->stop(reason, report_step(InstallTracePhase::Finishing));
    result.trace = session->info();
    return result;
}

}  // namespace avel::tracker
