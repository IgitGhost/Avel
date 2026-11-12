#include "leftover_text.h"

#include <QStringList>

#include <algorithm>
#include <cstdlib>
#include <type_traits>
#include <vector>

#include "core/leftovers/startup_approved.h"
#include "core/leftovers/value_edits.h"
#include "core/platform/registry_address.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "core/tracker/trace_items.h"
#include "program_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using changes::ItemState;
using changes::Reason;
using leftovers::Confidence;
using leftovers::Evidence;
using leftovers::ReferenceKind;

namespace {

// Keys in the hive of the user running Avel read HKCU\..., as in Registry Editor.
QString registryText(const platform::RegistryAddress& address) {
    static const std::wstring current_sid = platform::current_user_sid();
    if (address.hive == platform::Hive::User && !current_sid.empty() &&
        platform::iequals(address.sid, current_sid)) {
        return address.path.empty() ? u"HKCU"_s : u"HKCU\\"_s + toQString(address.path);
    }
    return toQString(platform::display_path(address));
}

// Points with a real minus sign, so the column of numbers lines up.
QString signedPoints(int points) {
    return points > 0 ? u"+%1"_s.arg(points) : points < 0 ? u"−%1"_s.arg(-points) : u"0"_s;
}

QString valueNameText(const std::wstring& name) {
    return name.empty() ? LeftoverText::tr("(Default)") : toQString(name);
}

// Whose variables a user's Environment key holds: empty for the user running Avel, otherwise
// their account name (their SID when it isn't known).
QString environmentOwner(const platform::RegistryAddress& key) {
    static const std::wstring current_sid = platform::current_user_sid();
    if (!current_sid.empty() && platform::iequals(key.sid, current_sid)) {
        return {};
    }
    static const std::vector<platform::UserProfile> profiles = platform::enumerate_user_profiles();
    for (const auto& profile : profiles) {
        if (platform::iequals(profile.sid, key.sid) && !profile.account_name.empty()) {
            return toQString(profile.account_name);
        }
    }
    return toQString(key.sid);
}

}  // namespace

QString LeftoverText::categoryLabel(leftovers::Category category) {
    using leftovers::Category;
    switch (category) {
        case Category::Files:
            return tr("Files and folders");
        case Category::Shortcuts:
            return tr("Shortcuts");
        case Category::Registry:
            return tr("Registry");
        case Category::Services:
            return tr("Services");
        case Category::Tasks:
            return tr("Scheduled tasks");
        case Category::Packages:
            return tr("Packages");
        case Category::Firewall:
            return tr("Firewall rules (shown only)");
    }
    return {};
}

QString LeftoverText::kindLabel(const leftovers::Location& location, leftovers::Category category) {
    return std::visit(
        [category](const auto& item) -> QString {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, leftovers::FileLocation>) {
                if (item.directory) {
                    return tr("Folder");
                }
                return category == leftovers::Category::Shortcuts ? tr("Shortcut") : tr("File");
            } else if constexpr (std::is_same_v<T, leftovers::RegistryKeyLocation>) {
                return tr("Registry key");
            } else if constexpr (std::is_same_v<T, leftovers::RegistryValueLocation>) {
                return tr("Registry value");
            } else if constexpr (std::is_same_v<T, leftovers::ServiceLocation>) {
                return item.driver ? tr("Driver (shown only)") : tr("Service");
            } else if constexpr (std::is_same_v<T, leftovers::TaskLocation>) {
                return item.folder ? tr("Scheduled task folder") : tr("Scheduled task");
            } else if constexpr (std::is_same_v<T, leftovers::PackageLocation>) {
                return tr("Package");
            } else if constexpr (std::is_same_v<T, leftovers::FirewallRuleLocation>) {
                return tr("Firewall rule");
            } else if constexpr (std::is_same_v<T, leftovers::StartupApprovedLocation>) {
                return tr("Startup entry");
            } else if constexpr (std::is_same_v<T, leftovers::TaskStateLocation>) {
                return tr("Scheduled task state");
            } else if constexpr (std::is_same_v<T, leftovers::ServiceStartLocation>) {
                return tr("Service start type");
            } else {
                static_assert(std::is_same_v<T, leftovers::ValueEditLocation>);
                using leftovers::ValueEditKind;
                if (leftovers::value_spot(item.key) == leftovers::ValueSpot::SharedDlls) {
                    return tr("Shared DLL reference count");
                }
                switch (item.kind) {
                    case ValueEditKind::RemovePathEntries:
                        return tr("Entries in an environment variable");
                    case ValueEditKind::RestoreValue:
                        return tr("Environment variable to restore");
                    case ValueEditKind::DecrementSharedDll:
                    case ValueEditKind::DeleteValue:
                        break;
                }
                return tr("Environment variable");
            }
        },
        location);
}

QString LeftoverText::locationText(const leftovers::Location& location) {
    return std::visit(
        [](const auto& item) -> QString {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, leftovers::FileLocation>) {
                return toQString(item.path);
            } else if constexpr (std::is_same_v<T, leftovers::RegistryKeyLocation>) {
                return registryText(item.key);
            } else if constexpr (std::is_same_v<T, leftovers::RegistryValueLocation>) {
                return registryText(item.key) + u" : "_s +
                       (item.value_name.empty() ? tr("(Default)") : toQString(item.value_name));
            } else if constexpr (std::is_same_v<T, leftovers::ServiceLocation>) {
                return toQString(item.name);
            } else if constexpr (std::is_same_v<T, leftovers::TaskLocation>) {
                return toQString(item.path);
            } else if constexpr (std::is_same_v<T, leftovers::PackageLocation>) {
                return toQString(item.full_name);
            } else if constexpr (std::is_same_v<T, leftovers::FirewallRuleLocation>) {
                return toQString(item.rule_id);
            } else if constexpr (std::is_same_v<T, leftovers::StartupApprovedLocation>) {
                const QString name = valueNameText(item.value_name);
                const QString where = registryText(item.key);
                if (!item.desired) {
                    return tr("Clear the startup state of %1 (%2)").arg(name, where);
                }
                return leftovers::approval_disabled(*item.desired)
                           ? tr("Disable %1 at startup (%2)").arg(name, where)
                           : tr("Enable %1 at startup (%2)").arg(name, where);
            } else if constexpr (std::is_same_v<T, leftovers::TaskStateLocation>) {
                return item.enabled_after ? tr("Enable the scheduled task %1").arg(toQString(item.path))
                                          : tr("Disable the scheduled task %1").arg(toQString(item.path));
            } else if constexpr (std::is_same_v<T, leftovers::ServiceStartLocation>) {
                return tr("Change the start type of the service %1 from %2 to %3")
                    .arg(toQString(item.name), serviceStartText(item.before), serviceStartText(item.after));
            } else {
                static_assert(std::is_same_v<T, leftovers::ValueEditLocation>);
                return valueEditText(item);
            }
        },
        location);
}

QString LeftoverText::valueEditText(const leftovers::ValueEditLocation& edit) {
    using leftovers::ValueEditKind;
    using leftovers::ValueSpot;
    const QString name = valueNameText(edit.value_name);
    const auto spot = leftovers::value_spot(edit.key);
    if (spot == ValueSpot::SharedDlls) {
        // The value's name is the DLL's path.
        if (!edit.desired) {
            return tr("Delete the shared DLL count of %1, which the installer created").arg(name);
        }
        const auto before = edit.expected ? leftovers::shared_dll_count(*edit.expected) : std::nullopt;
        const auto after = leftovers::shared_dll_count(*edit.desired);
        if (before && after) {
            return tr("Lower the shared DLL count of %1 from %2 to %3").arg(name).arg(*before).arg(*after);
        }
        return tr("Lower the shared DLL count of %1 by %2").arg(name).arg(edit.delta);
    }
    QStringList entries;
    for (const auto& entry : edit.entries) {
        entries << toQString(entry);
    }
    const QString list = entries.join(u"; "_s);
    const bool remove = edit.kind == ValueEditKind::RemovePathEntries && !entries.isEmpty();
    const bool restore = edit.kind == ValueEditKind::RestoreValue;
    if (spot == ValueSpot::MachineEnvironment) {
        if (remove) {
            return tr("Remove %1 from the system variable %2").arg(list, name);
        }
        return restore ? tr("Restore the system variable %1 to its value before the installation").arg(name)
                       : tr("Delete the system variable %1, which the installer created").arg(name);
    }
    if (spot == ValueSpot::UserEnvironment) {
        const QString owner = environmentOwner(edit.key);
        if (owner.isEmpty()) {
            if (remove) {
                return tr("Remove %1 from the user variable %2").arg(list, name);
            }
            return restore ? tr("Restore the user variable %1 to its value before the installation").arg(name)
                           : tr("Delete the user variable %1, which the installer created").arg(name);
        }
        if (remove) {
            return tr("Remove %1 from the variable %2 of user %3").arg(list, name, owner);
        }
        return restore
                   ? tr("Restore the variable %1 of user %2 to its value before the installation")
                         .arg(name, owner)
                   : tr("Delete the variable %1 of user %2, which the installer created").arg(name, owner);
    }
    return registryText(edit.key) + u" : "_s + name;  // Not a key Avel edits values in
}

QString LeftoverText::valueEditDetails(const leftovers::ValueEditLocation& edit) {
    QStringList lines;
    lines << registryText(edit.key) + u" : "_s + valueNameText(edit.value_name);
    if (const auto now = edit.expected ? leftovers::string_value_text(*edit.expected) : std::nullopt) {
        lines << tr("Now: %1").arg(toQString(*now));
    }
    if (!edit.desired) {
        lines << tr("Afterwards: deleted");
    } else if (const auto then = leftovers::string_value_text(*edit.desired)) {
        lines << tr("Afterwards: %1").arg(toQString(*then));
    }
    return lines.join(u'\n');
}

QString LeftoverText::tracedChangeText(leftovers::TracedChange change) {
    using leftovers::TracedChange;
    switch (change) {
        case TracedChange::Created:
            return tr("Created by the installer");
        case TracedChange::Deleted:
            return tr("Deleted by the installer; it existed before");
        case TracedChange::Modified:
            return tr("Changed by the installer; it existed before");
        case TracedChange::Renamed:
            return tr("Renamed or moved here by the installer; it existed before");
    }
    return {};
}

QString LeftoverText::traceUseText(const leftovers::TraceUse& use, const QString& program) {
    const auto count = [](std::size_t value) { return static_cast<int>(value); };
    QStringList parts;
    parts << tr("%n item(s) its installer created", nullptr, count(use.items));
    if (use.tied > 0) {
        parts << tr("%n of them tied to the program", nullptr, count(use.tied));
    }
    if (use.value_edits > 0) {
        parts << tr("%n edit(s) to environment variables or shared DLL counts", nullptr,
                    count(use.value_edits));
    }
    if (use.gone > 0) {
        parts << tr("%n already gone", nullptr, count(use.gone));
    }
    if (use.protected_count > 0) {
        parts << tr("%n protected, left in place", nullptr, count(use.protected_count));
    }
    if (use.shown_only > 0) {
        parts << tr("%n change(s) to what existed before, shown only", nullptr, count(use.shown_only));
    }
    if (use.excluded > 0) {
        parts << tr("%n item(s) excluded in its review, not checked automatically", nullptr,
                    count(use.excluded));
    }
    return tr("Install trace of %1: %2.").arg(program, parts.join(u"; "_s));
}

QString LeftoverText::confidenceLabel(Confidence confidence) {
    switch (confidence) {
        case Confidence::VeryGood:
            return tr("Very likely");
        case Confidence::Good:
            return tr("Likely");
        case Confidence::Questionable:
            return tr("Uncertain");
        case Confidence::Bad:
            return tr("Unlikely");
        case Confidence::Unknown:
            break;
    }
    return tr("Unknown");
}

QColor LeftoverText::confidenceColor(Confidence confidence) {
    switch (confidence) {
        case Confidence::VeryGood:
            return QColor(0x2E, 0x8B, 0x57);
        case Confidence::Good:
            return QColor(0x7C, 0xB3, 0x42);
        case Confidence::Questionable:
            return QColor(0xE0, 0xA1, 0x00);
        case Confidence::Bad:
            return QColor(0xC0, 0x40, 0x30);
        case Confidence::Unknown:
            break;
    }
    return QColor(0x80, 0x80, 0x80);
}

QString LeftoverText::evidenceText(const leftovers::Record& record) {
    const auto with_reference = [&](const QString& plain, const QString& referenced) {
        return record.reference == ReferenceKind::None ? plain
                                                       : referenced.arg(referenceText(record.reference));
    };
    switch (record.evidence) {
        case Evidence::UninstallerKey:
            return tr("The program's Uninstall entry");
        case Evidence::InstallerRegistration:
            return tr("Windows Installer's registration of the program");
        case Evidence::TracedInstallRoot:
            return tr("Created by the program's installer, in its own folders or keys (install trace)");
        case Evidence::TracedValueUndo:
            return tr("Takes back exactly what the program's installer changed (install trace)");
        case Evidence::TracedCreated:
            return tr("Created during the installation, but not tied to the program (install trace)");
        case Evidence::TracedValueRestore:
            return tr(
                "Puts back the value from before the installation; "
                "the new one may be wanted (install trace)");
        case Evidence::ExplicitConnection:
            return tr("Named in the program's registration");
        case Evidence::PathReference:
            return with_reference(tr("Points into the program's folder"),
                                  tr("Points into the program's folder (%1)"));
        case Evidence::CompanyNameMatch:
            return tr("In a folder or key named after the publisher");
        case Evidence::AllSubdirsMatched:
            return tr("Holds nothing but other leftovers");
        case Evidence::StartupMatched:
            return with_reference(tr("Starts the program"), tr("Starts the program (%1)"));
        case Evidence::ProductNamePerfectMatch:
            return tr("Named exactly like the program");
        case Evidence::NoOwner:
            return tr("No installed program owns it");
        case Evidence::ExcludedInTraceReview:
            return tr("Excluded in the install trace's review: not checked automatically");
        case Evidence::HoldsTraceReviewExclusion:
            return tr("Holds an item excluded in the install trace's review: not checked automatically");
        case Evidence::Depth:
            return record.level <= 0 ? tr("At the top of a common location")
                                     : tr("%n level(s) below a common location", nullptr, record.level);
        case Evidence::DirectlyInsideKnownFolder:
            return tr("Directly inside a system folder");
        case Evidence::ProductNameDodgyMatch:
            return tr("Its name only resembles the program's");
        case Evidence::OutsideCommonLocations:
            return tr("Found by the disk search, away from where programs keep their files");
        case Evidence::ItemNameEqualsCompanyName:
            return tr("Named after the publisher, whose other programs may use it");
        case Evidence::UsedBySimilarNamedApp:
            return tr("May belong to another program with a similar name");
        case Evidence::OtherUserData:
            return tr("In another user's profile");
        case Evidence::QuestionableDirectoryName:
            return tr("Generic name, such as “data” or “config”");
        case Evidence::ContainerHoldsMore:
            return tr(
                "Holds more than the installer put in it, which may be another program's (install trace)");
        case Evidence::StartupIsRunOnce:
            return tr("Runs only once (RunOnce)");
        case Evidence::DirectoryStillUsed:
            return tr("Another installed program uses this folder");
        case Evidence::SharedDllStillUsed:
            return tr("Another program still uses this shared DLL, or one inside it");
        case Evidence::StoreApp:
            return tr("Store app data, which Windows manages itself");
    }
    return {};
}

QString LeftoverText::evidenceSummary(std::span<const leftovers::Record> records) {
    // Depth only fine-tunes the score; the list shows what actually matched. Evidence worth no
    // points still says why the item is listed (an orphaned folder: no program owns it). An
    // exclusion in a trace's review, of the item or of something it holds, comes first: it is why
    // the item isn't checked.
    std::vector<const leftovers::Record*> sorted;
    QStringList parts;
    for (const auto& record : records) {
        if (record.evidence == Evidence::ExcludedInTraceReview ||
            record.evidence == Evidence::HoldsTraceReviewExclusion) {
            parts = QStringList{evidenceText(record)};
        } else if (record.evidence != Evidence::Depth) {
            sorted.push_back(&record);
        }
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
        return std::abs(leftovers::points(*a)) > std::abs(leftovers::points(*b));
    });
    QStringList for_it;
    QStringList against;
    for (const auto* record : sorted) {
        (leftovers::points(*record) >= 0 ? for_it : against) << evidenceText(*record);
    }
    const QString separator = u"; "_s;
    if (against.isEmpty()) {
        parts << for_it.join(separator);
    } else if (for_it.isEmpty()) {
        parts << tr("Against it: %1").arg(against.join(separator));
    } else {
        parts << tr("%1; against it: %2").arg(for_it.join(separator), against.join(separator));
    }
    parts.removeAll(QString());
    return parts.join(separator);
}

QString LeftoverText::evidenceDetails(std::span<const leftovers::Record> records) {
    QString rows;
    for (const auto& record : records) {
        QString text = evidenceText(record).toHtmlEscaped();
        if (!record.detail.empty()) {
            // A tied item's detail is the tracker's reason, a code; an excluded item's, the trace's id.
            QString detail =
                record.evidence == Evidence::TracedInstallRoot ? tieText(record.detail) : QString();
            if (record.evidence == Evidence::ExcludedInTraceReview) {
                detail = tr("Install trace %1").arg(toQString(record.detail));
            }
            if (detail.isEmpty()) {
                detail = toQString(record.detail);
            }
            text += u"<br><small>"_s + detail.toHtmlEscaped() + u"</small>"_s;
        }
        rows += u"<tr><td align=\"right\" style=\"padding-right:8px\">%1</td><td>%2</td></tr>"_s.arg(
            signedPoints(leftovers::points(record)), text);
    }
    const QString total =
        tr("Score %1: %2")
            .arg(signedPoints(leftovers::score(records)), confidenceLabel(leftovers::level(records)))
            .toHtmlEscaped();
    return u"<table>%1</table><p><b>%2</b></p>"_s.arg(rows, total);
}

QString LeftoverText::tieText(std::wstring_view code) {
    using tracker::TieReason;
    for (const auto reason :
         {TieReason::InstallFolder, TieReason::UninstallerFolder, TieReason::IconFolder,
          TieReason::UninstallKey, TieReason::MsiRegistration, TieReason::NamedAfterProgram,
          TieReason::NamedAfterPublisher, TieReason::InsideTied, TieReason::ReferencesTied}) {
        if (code != platform::to_utf16(tracker::to_string(reason))) {
            continue;
        }
        switch (reason) {
            case TieReason::InstallFolder:
                return tr("Tied to the program: its install folder, or in it");
            case TieReason::UninstallerFolder:
                return tr("Tied to the program: its uninstaller's folder, or in it");
            case TieReason::IconFolder:
                return tr("Tied to the program: its icon's folder, or in it");
            case TieReason::UninstallKey:
                return tr("Tied to the program: its Uninstall entry, or in it");
            case TieReason::MsiRegistration:
                return tr("Tied to the program: Windows Installer's registration of it");
            case TieReason::NamedAfterProgram:
                return tr("Tied to the program: named after it");
            case TieReason::NamedAfterPublisher:
                return tr("Tied to the program: named after its publisher");
            case TieReason::InsideTied:
                return tr("Tied to the program: inside another of its items");
            case TieReason::ReferencesTied:
                return tr("Tied to the program: refers to another of its items or folders");
            case TieReason::None:
                break;
        }
    }
    return {};
}

QString LeftoverText::referenceText(ReferenceKind kind) {
    switch (kind) {
        case ReferenceKind::None:
            return {};
        case ReferenceKind::RunValue:
            return tr("startup entry");
        case ReferenceKind::ServiceImage:
            return tr("service");
        case ReferenceKind::TaskAction:
            return tr("scheduled task");
        case ReferenceKind::AppPath:
            return tr("App Paths entry");
        case ReferenceKind::SharedDll:
            return tr("shared DLL entry");
        case ReferenceKind::InstallerFolder:
            return tr("Windows Installer folder entry");
        case ReferenceKind::MuiCache:
            return tr("Explorer's MUI cache");
        case ReferenceKind::ShortcutTarget:
            return tr("shortcut target");
        case ReferenceKind::ComServer:
            return tr("COM server");
        case ReferenceKind::FirewallApp:
            return tr("firewall rule");
        case ReferenceKind::PackageExternalLocation:
            return tr("package's external location");
    }
    return {};
}

QString LeftoverText::diagnosticText(const leftovers::ScanDiagnostic& diagnostic) {
    using leftovers::ScanDiagnosticCode;
    switch (diagnostic.code) {
        case ScanDiagnosticCode::NotElevated:
            return tr(
                "Avel isn't running as administrator, so other users' data and some system entries "
                "weren't checked.");
        case ScanDiagnosticCode::UserHiveUnavailable:
            return tr("The registry of user %1 couldn't be opened (error %2); their entries weren't checked.")
                .arg(toQString(diagnostic.subject))
                .arg(diagnostic.error);
        case ScanDiagnosticCode::DeepSearchUnavailable:
            return tr("Drive %1 couldn't be searched (error %2).")
                .arg(toQString(diagnostic.subject))
                .arg(diagnostic.error);
    }
    return {};
}

QString LeftoverText::itemStateText(ItemState state) {
    switch (state) {
        case ItemState::Pending:
            return tr("Waiting");
        case ItemState::BackedUp:
            return tr("Backed up, not finished");
        case ItemState::Done:
            return tr("Removed");
        case ItemState::Partial:
            return tr("Partly removed");
        case ItemState::Skipped:
            return tr("Skipped");
        case ItemState::Failed:
            return tr("Failed");
        case ItemState::Restored:
            return tr("Restored");
        case ItemState::RestoreSkipped:
            return tr("Not restored");
        case ItemState::RestoreFailed:
            return tr("Restore failed");
    }
    return {};
}

QString LeftoverText::reasonText(Reason reason) {
    switch (reason) {
        case Reason::None:
            return {};
        case Reason::Protected:
            return tr("a protected location");
        case Reason::PathChanged:
            return tr("it now leads somewhere else");
        case Reason::Missing:
            return tr("it was already gone");
        case Reason::AccessDenied:
            return tr("Windows denied access");
        case Reason::InUse:
            return tr("files are in use by running programs");
        case Reason::RegistryLink:
            return tr("a registry link, which Avel never follows");
        case Reason::UnsupportedVolume:
            return tr("not on a local NTFS drive");
        case Reason::HasDependents:
            return tr("other services depend on it");
        case Reason::NeedsPassword:
            return tr("it runs under an account whose password Avel can't restore");
        case Reason::MarkedForDeletion:
            return tr("Windows is already deleting it");
        case Reason::OtherUser:
            return tr("registered for another user");
        case Reason::NotUndoable:
            return tr("it couldn't be restored afterwards");
        case Reason::ReportOnly:
            return tr("shown only, never removed");
        case Reason::Contained:
            return tr("removed with the item that contains it");
        case Reason::Interrupted:
            return tr("the removal was interrupted");
        case Reason::Exists:
            return tr("something new is in its place");
        case Reason::BackupMissing:
            return tr("its backup is missing");
        case Reason::Error:
            return tr("an error occurred");
        case Reason::Changed:
            return tr("the value has changed since");
        case Reason::NotEmpty:
            return tr("the folder isn't empty");
    }
    return {};
}

QString LeftoverText::statusText(const changes::ItemStatus& status) {
    if (status.state == ItemState::Skipped && status.reason == Reason::Contained) {
        return tr("Removed with the item that contains it");
    }
    const QString state = itemStateText(status.state);
    QString text = state;
    if (status.reason == Reason::Error && status.code != 0) {
        text = tr("%1: %2").arg(state, ProgramText::systemErrorText(status.code));
    } else if (status.reason != Reason::None) {
        text = tr("%1: %2").arg(state, reasonText(status.reason));
        if (status.code != 0) {
            text += u" ("_s + ProgramText::systemErrorText(status.code) + u')';
        }
    } else if (status.code != 0) {
        text = tr("%1: %2").arg(state, ProgramText::systemErrorText(status.code));
    }
    return text;
}

QString LeftoverText::statusText(const changes::ItemStatus& status, const leftovers::Location& location) {
    if (status.state == ItemState::Done && std::holds_alternative<leftovers::ValueEditLocation>(location)) {
        return tr("Edited");
    }
    const bool state_change = std::holds_alternative<leftovers::StartupApprovedLocation>(location) ||
                              std::holds_alternative<leftovers::TaskStateLocation>(location) ||
                              std::holds_alternative<leftovers::ServiceStartLocation>(location);
    if (!state_change) {
        return statusText(status);
    }
    if (status.state == ItemState::Done) {
        if (const auto* approval = std::get_if<leftovers::StartupApprovedLocation>(&location)) {
            if (!approval->desired) {
                return tr("Cleared");
            }
            return leftovers::approval_disabled(*approval->desired) ? tr("Disabled") : tr("Enabled");
        }
        if (const auto* task = std::get_if<leftovers::TaskStateLocation>(&location)) {
            return task->enabled_after ? tr("Enabled") : tr("Disabled");
        }
        return tr("Start type changed");
    }
    // Compare-and-set: the item isn't in the state the change applies to (or, for undo, the state
    // it set) any more.
    if (status.reason == Reason::Changed) {
        return tr("%1: %2").arg(itemStateText(status.state), tr("its state has changed since"));
    }
    return statusText(status);
}

QString LeftoverText::serviceStartText(const leftovers::ServiceStart& start) {
    switch (start.start_type) {
        case SERVICE_BOOT_START:
            return tr("Boot");
        case SERVICE_SYSTEM_START:
            return tr("System");
        case SERVICE_AUTO_START:
            return start.delayed ? tr("Automatic (delayed start)") : tr("Automatic");
        case SERVICE_DEMAND_START:
            return tr("Manual");
        case SERVICE_DISABLED:
            return tr("Disabled");
        default:
            return QString::number(start.start_type);
    }
}

QString LeftoverText::setStateText(changes::SetState state) {
    using changes::SetState;
    switch (state) {
        case SetState::Running:
            return tr("Unfinished");
        case SetState::Complete:
            return tr("Removed");
        case SetState::Restored:
            return tr("Restored");
        case SetState::PartlyRestored:
            return tr("Partly restored");
    }
    return {};
}

QString LeftoverText::folderStatusText(platform::FolderStatus status) {
    using platform::FolderStatus;
    switch (status) {
        case FolderStatus::Ok:
            return {};
        case FolderStatus::NotFolder:
            return tr("a file is in its place");
        case FolderStatus::ReparsePoint:
            return tr("it is a link to another place");
        case FolderStatus::Redirected:
            return tr("it leads to another place");
        case FolderStatus::ForeignOwner:
            return tr("it isn't owned by the administrators");
        case FolderStatus::Unavailable:
            return tr("it can't be created or opened");
    }
    return {};
}

}  // namespace avel::app
