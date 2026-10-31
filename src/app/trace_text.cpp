#include "trace_text.h"

#include <QDateTime>
#include <QDir>
#include <QLocale>

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/platform/paths.h"
#include "leftover_text.h"
#include "program_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using tracker::TraceState;

namespace {

QDateTime toDateTime(std::chrono::system_clock::time_point time) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(ms);
}

bool isFinished(TraceState state) { return state == TraceState::Complete || state == TraceState::Incomplete; }

QString nativePath(const std::wstring& path) { return QDir::toNativeSeparators(toQString(path)); }

}  // namespace

QString TraceText::stateText(TraceState state) {
    switch (state) {
        case TraceState::Starting:
            return tr("Starting");
        case TraceState::Open:
            return tr("Open");
        case TraceState::Stopping:
            return tr("Stopping");
        case TraceState::Complete:
            return tr("Complete");
        case TraceState::Incomplete:
            return tr("Incomplete");
    }
    return {};
}

QString TraceText::modeText(tracker::TraceMode mode) {
    switch (mode) {
        case tracker::TraceMode::Manual:
            return tr("Manual");
        case tracker::TraceMode::Launch:
            return tr("Install and trace");
    }
    return {};
}

QString TraceText::stopReasonText(tracker::StopReason reason) {
    using tracker::StopReason;
    switch (reason) {
        case StopReason::None:
            return {};
        case StopReason::Manual:
            return tr("Stopped by hand");
        case StopReason::InstallerDone:
            return tr("The installation was over");
        case StopReason::Stopped:
            return tr("Stopped early; the installer may have kept running");
        case StopReason::TimedOut:
            return tr("Avel gave up waiting for the installer");
        case StopReason::RebootPending:
            return tr("The installer asked for a restart");
    }
    return {};
}

QString TraceText::errorText(tracker::TraceError error) {
    using tracker::TraceError;
    switch (error) {
        case TraceError::None:
            return {};
        case TraceError::AlreadyOpen:
            return tr("another trace is open");
        case TraceError::NotFound:
            return tr("the trace doesn't exist anymore");
        case TraceError::NotOpen:
            return tr("the trace isn't open");
        case TraceError::NotFinished:
            return tr("the trace isn't finished yet");
        case TraceError::InUse:
            return tr("another Avel process is using the trace");
        case TraceError::Damaged:
            return tr("the trace's files can't be read");
        case TraceError::WriteFailed:
            return tr("a file of the trace couldn't be written or deleted");
        case TraceError::Busy:
            return tr("another Avel process is using the folder that holds the traces; try again");
        case TraceError::LaunchFailed:
            return tr("the installer couldn't be started");
        case TraceError::Cancelled:
            return tr("it was stopped before the installer started");
    }
    return {};
}

QString TraceText::stepText(tracker::SessionStep step) {
    using tracker::SessionStep;
    switch (step) {
        case SessionStep::Volumes:
            return tr("Recording where each drive's change journal is…");
        case SessionStep::KnownValues:
            return tr("Saving the environment variables and shared DLL counts…");
        case SessionStep::Snapshot:
            return tr("Taking a snapshot of the registry. This takes a few seconds…");
        case SessionStep::Draining:
            return tr("Reading the drives' change journals…");
        case SessionStep::Files:
            return tr("Finding the changed files…");
        case SessionStep::Registry:
            return tr("Comparing the registry with the snapshot…");
        case SessionStep::Saving:
            return tr("Saving the result…");
    }
    return {};
}

QString TraceText::watchText(tracker::WatchPhase phase) {
    using tracker::WatchPhase;
    switch (phase) {
        case WatchPhase::Running:
            return tr("Waiting for the installer…");
        case WatchPhase::WaitingForHelpers:
            return tr("The installer has exited. Waiting for the programs it started…");
        case WatchPhase::WaitingForWindowsInstaller:
            return tr("Waiting for Windows Installer to finish…");
        case WatchPhase::Settling:
            return tr("Waiting for the disk to settle…");
    }
    return {};
}

QString TraceText::watchEndText(tracker::WatchEnd end) {
    using tracker::WatchEnd;
    switch (end) {
        case WatchEnd::InstallerDone:
            return tr("The installation was over");
        case WatchEnd::Stopped:
            return tr("Stopped before the installer was done");
        case WatchEnd::TimedOut:
            return tr("Avel gave up waiting for the installer");
        case WatchEnd::LaunchFailed:
            return tr("The installer couldn't be started");
    }
    return {};
}

QString TraceText::launchErrorText(std::uint32_t error) {
    switch (error) {
        case ERROR_NOT_SUPPORTED:
            return tr("MSIX and AppX packages are installed by Windows itself, and Avel doesn't trace them.");
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return tr("The installer doesn't exist.");
        case ERROR_ACCESS_DENIED:
            return tr("Access to the installer was denied.");
        case ERROR_BAD_EXE_FORMAT:
            return tr("The file isn't a program Windows can run.");
        default:
            return ProgramText::systemErrorText(error);
    }
}

QString TraceText::importErrorText(tracker::ImportError error, int version) {
    using tracker::ImportError;
    switch (error) {
        case ImportError::None:
            return {};
        case ImportError::Damaged:
            return tr("The file is damaged or incomplete.");
        case ImportError::OtherFormat:
            return tr("The file isn't an Avel trace. Avel writes them with Export.");
        case ImportError::OtherVersion:
            return tr("The file is an Avel trace of format version %1, which this version of Avel can't read "
                      "(it reads version %2).")
                .arg(version)
                .arg(tracker::kExportSchemaVersion);
        case ImportError::NotFinished:
            return tr("The trace in the file was never finished.");
    }
    return {};
}

QString TraceText::changeText(tracker::ChangeKind change) {
    using tracker::ChangeKind;
    switch (change) {
        case ChangeKind::Created:
            return tr("Created");
        case ChangeKind::Deleted:
            return tr("Deleted");
        case ChangeKind::Modified:
            return tr("Modified");
        case ChangeKind::Renamed:
            return tr("Renamed");
    }
    return {};
}

QString TraceText::noiseText(tracker::NoiseReason reason) {
    using tracker::NoiseReason;
    switch (reason) {
        case NoiseReason::None:
            return {};
        case NoiseReason::SystemActivity:
            return tr("Windows' own activity (logs, caches, updates, indexing)");
        case NoiseReason::UserActivity:
            return tr("The user's activity (recent files, Explorer's views, browser caches)");
        case NoiseReason::Temporary:
            return tr("A temporary folder");
        case NoiseReason::Avel:
            return tr("Avel's own files and keys");
        case NoiseReason::Unresolved:
            return tr("Its path couldn't be determined");
        case NoiseReason::MetadataOnly:
            return tr("Only its attributes, times or security changed");
    }
    return {};
}

QString TraceText::tieText(tracker::TieReason reason) {
    using tracker::TieReason;
    switch (reason) {
        case TieReason::None:
            return tr("Nothing ties it to the program");
        case TieReason::InstallFolder:
            return tr("In the program's install folder");
        case TieReason::UninstallerFolder:
            return tr("In the folder of the program's uninstaller");
        case TieReason::IconFolder:
            return tr("In the folder of the program's icon");
        case TieReason::UninstallKey:
            return tr("The program's Uninstall entry");
        case TieReason::MsiRegistration:
            return tr("Windows Installer's registration of the program");
        case TieReason::NamedAfterProgram:
            return tr("Named after the program");
        case TieReason::NamedAfterPublisher:
            return tr("Named after the publisher");
        case TieReason::InsideTied:
            return tr("Inside an item tied to the program");
        case TieReason::ReferencesTied:
            return tr("Refers to an item tied to the program");
    }
    return {};
}

QString TraceText::spotText(tracker::KnownSpot spot) {
    using tracker::KnownSpot;
    switch (spot) {
        case KnownSpot::MachineEnvironment:
            return tr("Environment variable (all users)");
        case KnownSpot::UserEnvironment:
            return tr("Environment variable (one user)");
        case KnownSpot::SharedDlls:
            return tr("Shared DLL reference count");
    }
    return {};
}

QString TraceText::linkMatchText(tracker::LinkMatch match) {
    using tracker::LinkMatch;
    switch (match) {
        case LinkMatch::CreatedKey:
            return tr("the installer created its Uninstall entry");
        case LinkMatch::ChangedKey:
            return tr("the installer updated its Uninstall entry");
        case LinkMatch::ProductCode:
            return tr("the installer registered its Windows Installer product");
        case LinkMatch::Manual:
            return tr("linked by hand");
    }
    return {};
}

QString TraceText::journalStatusText(tracker::JournalStatus status) {
    using tracker::JournalStatus;
    switch (status) {
        case JournalStatus::Ok:
            return tr("recorded");
        case JournalStatus::Untracked:
            return tr("not recorded: it has no change journal");
        case JournalStatus::VolumeMissing:
            return tr("not recorded to the end: the drive went away");
        case JournalStatus::SerialChanged:
            return tr("not recorded to the end: the drive was formatted");
        case JournalStatus::Replaced:
            return tr("not recorded to the end: its change journal was replaced");
        case JournalStatus::Wrapped:
            return tr("not recorded to the end: its change journal ran over before Avel could read it");
        case JournalStatus::Unreadable:
            return tr("not recorded to the end: its change journal couldn't be read");
    }
    return {};
}

QString TraceText::incompleteText(const tracker::IncompleteReason& reason) {
    using tracker::IncompleteKind;
    switch (reason.kind) {
        case IncompleteKind::Volume:
            return tr("Drive %1: %2").arg(nativePath(reason.volume), journalStatusText(reason.status));
        case IncompleteKind::Registry: {
            const QString key =
                LeftoverText::locationText(leftovers::RegistryKeyLocation{reason.diagnostic.key});
            switch (reason.diagnostic.code) {
                case tracker::SnapshotDiagnosticCode::RootUnavailable:
                    return tr("The registry under %1 couldn't be opened, so it wasn't compared").arg(key);
                case tracker::SnapshotDiagnosticCode::KeyUnreadable:
                case tracker::SnapshotDiagnosticCode::KeyUnreadableAtStart:
                    break;
            }
            return tr("The registry key %1 couldn't be read, so changes in it are unknown").arg(key);
        }
        case IncompleteKind::SnapshotDamaged:
            return tr("The registry snapshot taken at the start is damaged, so the registry wasn't compared");
        case IncompleteKind::SavedValuesDamaged:
            return tr(
                "The values saved at the start are damaged, so the environment variables and shared "
                "DLL counts weren't compared");
    }
    return {};
}

QString TraceText::valueText(const std::optional<platform::RawValue>& value) {
    if (!value) {
        return tr("(none)");
    }
    if (value->type == REG_SZ || value->type == REG_EXPAND_SZ || value->type == REG_MULTI_SZ) {
        std::wstring text(value->data.size() / sizeof(wchar_t), L'\0');
        if (!text.empty()) {
            std::memcpy(text.data(), value->data.data(), text.size() * sizeof(wchar_t));
        }
        while (!text.empty() && text.back() == L'\0') {
            text.pop_back();
        }
        QString shown = toQString(text);
        if (value->type == REG_MULTI_SZ) {
            shown.replace(QChar(u'\0'), u" | "_s);
        }
        return shown;
    }
    if ((value->type == REG_DWORD && value->data.size() == 4) ||
        (value->type == REG_QWORD && value->data.size() == 8)) {
        std::uint64_t number = 0;
        std::memcpy(&number, value->data.data(), value->data.size());
        return QLocale().toString(static_cast<qulonglong>(number));
    }
    return tr("%n byte(s) of type %1", nullptr, static_cast<int>(value->data.size())).arg(value->type);
}

QString TraceText::linkText(const tracker::TraceLink& link) {
    QString text = link.name.empty() ? toQString(link.program_id) : toQString(link.name);
    QStringList details;
    if (!link.version.empty()) {
        details << toQString(link.version);
    }
    if (!link.publisher.empty()) {
        details << toQString(link.publisher);
    }
    return details.isEmpty() ? text : tr("%1 (%2)").arg(text, details.join(u", "_s));
}

QString TraceText::traceName(const tracker::TraceInfo& info) {
    const auto primary = std::find_if(info.links.begin(), info.links.end(),
                                      [](const tracker::TraceLink& link) { return link.primary; });
    const tracker::TraceLink* link = primary != info.links.end() ? &*primary
                                     : info.links.empty()        ? nullptr
                                                                 : &info.links.front();
    if (link != nullptr && !link->name.empty()) {
        return toQString(link->name);
    }
    if (!info.description.empty()) {
        return toQString(info.description);
    }
    return tr("Trace %1").arg(toQString(info.id));
}

QString TraceText::timeText(std::chrono::system_clock::time_point time) {
    return QLocale().toString(toDateTime(time), QLocale::ShortFormat);
}

QString TraceText::sinceText(std::chrono::system_clock::time_point time) {
    const QDateTime when = toDateTime(time);
    if (when.date() == QDate::currentDate()) {
        return QLocale().toString(when.time(), QLocale::ShortFormat);
    }
    return QLocale().toString(when, QLocale::ShortFormat);
}

QString TraceText::processList(std::span<const tracker::WatchProcess> processes) {
    constexpr std::size_t kShown = 5;
    QStringList names;
    for (std::size_t i = 0; i < processes.size() && i < kShown; ++i) {
        const auto& process = processes[i];
        const QString name = process.image.empty()
                                 ? tr("process")
                                 : toQString(std::wstring(platform::last_path_component(process.image)));
        names << tr("%1 (%2)").arg(name).arg(process.pid);
    }
    QString text = names.join(u", "_s);
    if (processes.size() > kShown) {
        text = tr("%1 and %n more", nullptr, static_cast<int>(processes.size() - kShown)).arg(text);
    }
    return text;
}

QStringList TraceText::summary(const tracker::TraceInfo& info) {
    QStringList lines;
    QString state = stateText(info.state);
    if (const QString reason = stopReasonText(info.stop_reason); !reason.isEmpty()) {
        state = tr("%1: %2").arg(state, reason);
    }
    lines << tr("State: %1").arg(state);
    if (info.stopped) {
        lines << tr("Recorded from %1 to %2").arg(timeText(info.started), timeText(*info.stopped));
    } else {
        lines << tr("Recording since %1").arg(timeText(info.started));
    }
    lines << tr("Mode: %1").arg(modeText(info.mode));
    if (!info.command.empty()) {
        lines << tr("Command: %1").arg(toQString(info.command));
    }
    if (info.watch) {
        const auto& watch = *info.watch;
        QString outcome = watchEndText(watch.end);
        if (watch.end == tracker::WatchEnd::InstallerDone || watch.reboot_requested) {
            outcome = tr("%1 (exit code %2)").arg(outcome).arg(watch.exit_code);
        }
        lines << tr("Installer: %1").arg(outcome);
        if (!watch.left_running.empty()) {
            lines << tr("Left running by the installer: %1").arg(processList(watch.left_running));
        }
    }
    QStringList volumes;
    for (const auto& volume : info.volumes) {
        volumes << tr("%1 %2").arg(nativePath(volume.root), journalStatusText(volume.status));
    }
    if (!volumes.isEmpty()) {
        lines << tr("Drives: %1").arg(volumes.join(u"; "_s));
    }
    lines << tr("Registry: %1 keys and %2 values in the snapshot")
                 .arg(QLocale().toString(static_cast<qulonglong>(info.registry.keys)),
                      QLocale().toString(static_cast<qulonglong>(info.registry.values)));
    if (isFinished(info.state)) {
        lines << tr("Changes: %1; set aside as noise: %2")
                     .arg(QLocale().toString(static_cast<qulonglong>(info.counts.changes)),
                          QLocale().toString(static_cast<qulonglong>(info.counts.noise)));
        if (info.links.empty()) {
            lines << tr("Not linked to a program");
        }
        for (const auto& link : info.links) {
            lines << tr("Linked to %1: %2").arg(linkText(link), linkMatchText(link.matched_by));
        }
        if (info.unresolved_since) {
            lines << tr("Uninstalled since %1; the trace is deleted 30 days later")
                         .arg(timeText(*info.unresolved_since));
        }
    }
    if (info.imported) {
        const auto& imported = *info.imported;
        lines << tr("Imported on %1 (recorded as %2)")
                     .arg(timeText(imported.time), toQString(imported.origin_id));
        if (!imported.from_sid.empty()) {
            lines << tr("Items of %1 moved to %2")
                         .arg(nativePath(imported.from_profile), nativePath(imported.to_profile));
        }
        for (const auto& diagnostic : info.diagnostics) {
            if (diagnostic.code == tracker::TraceDiagnosticCode::UserNotRemapped) {
                lines << tr("Items of user %1 left as they were recorded").arg(toQString(diagnostic.subject));
            }
        }
    }
    for (const auto& reason : tracker::incomplete_reasons(info)) {
        lines << tr("Incomplete: %1").arg(incompleteText(reason));
    }
    return lines;
}

}  // namespace avel::app
