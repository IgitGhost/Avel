#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

#include "core/platform/nt_registry.h"
#include "core/tracker/install_trace.h"
#include "core/tracker/installer_watch.h"
#include "core/tracker/trace.h"
#include "core/tracker/trace_export.h"
#include "core/tracker/trace_items.h"
#include "core/tracker/trace_record.h"
#include "core/tracker/trace_session.h"
#include "core/tracker/trace_store.h"

namespace avel::app {

// Translated, user-facing text for Avel Trace's codes.
class TraceText {
    Q_DECLARE_TR_FUNCTIONS(TraceText)

public:
    static QString stateText(tracker::TraceState state);
    static QString modeText(tracker::TraceMode mode);
    static QString stopReasonText(tracker::StopReason reason);
    // Why an operation on a trace failed: "another Avel process is using it", ...
    static QString errorText(tracker::TraceError error);
    // What starting or stopping a trace is doing: "Taking the registry snapshot…".
    static QString stepText(tracker::SessionStep step);
    // What Install and trace is waiting for: "Waiting for the installer…".
    static QString watchText(tracker::WatchPhase phase);
    static QString watchEndText(tracker::WatchEnd end);
    // Why the installer couldn't be started (WatchResult::error).
    static QString launchErrorText(std::uint32_t error);
    static QString importErrorText(tracker::ImportError error, int version);

    static QString changeText(tracker::ChangeKind change);
    static QString noiseText(tracker::NoiseReason reason);
    static QString tieText(tracker::TieReason reason);
    static QString spotText(tracker::KnownSpot spot);
    static QString linkMatchText(tracker::LinkMatch match);
    static QString journalStatusText(tracker::JournalStatus status);
    static QString incompleteText(const tracker::IncompleteReason& reason);

    // A registry value on one line: strings as text (a REG_MULTI_SZ's joined by " | "), numbers in
    // decimal, anything else by its type and size; "(none)" when it didn't exist.
    static QString valueText(const std::optional<platform::RawValue>& value);
    // "7-Zip 24.08 (x64) (24.08, Igor Pavlov)"; the program's id when it has no name.
    static QString linkText(const tracker::TraceLink& link);
    // The program a trace is for: its primary link's name, or else its description.
    static QString traceName(const tracker::TraceInfo& info);
    // A time in the UI locale's short format, in local time.
    static QString timeText(std::chrono::system_clock::time_point time);
    // "14:02", or "Oct 29, 14:02" when it isn't today.
    static QString sinceText(std::chrono::system_clock::time_point time);
    // "setup.exe (1234), msiexec.exe (5678) and 2 more".
    static QString processList(std::span<const tracker::WatchProcess> processes);
    // A trace's summary, one line each: its state and stop reason, times, mode and command, the
    // installer's outcome and what it left running, the volumes and registry, the links, the
    // import, and why it's incomplete.
    static QStringList summary(const tracker::TraceInfo& info);
};

}  // namespace avel::app
