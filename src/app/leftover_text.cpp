#include "leftover_text.h"

#include <QStringList>

#include <algorithm>
#include <cstdlib>
#include <type_traits>
#include <vector>

#include "core/platform/registry_address.h"
#include "core/platform/user_profiles.h"
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
                return tr("Service");
            } else if constexpr (std::is_same_v<T, leftovers::TaskLocation>) {
                return tr("Scheduled task");
            } else if constexpr (std::is_same_v<T, leftovers::PackageLocation>) {
                return tr("Package");
            } else {
                return tr("Firewall rule");
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
            } else {
                return toQString(item.rule_id);
            }
        },
        location);
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
        case Evidence::Depth:
            return record.level <= 0 ? tr("At the top of a common location")
                                     : tr("%n level(s) below a common location", nullptr, record.level);
        case Evidence::DirectlyInsideKnownFolder:
            return tr("Directly inside a system folder");
        case Evidence::ProductNameDodgyMatch:
            return tr("Its name only resembles the program's");
        case Evidence::ItemNameEqualsCompanyName:
            return tr("Named after the publisher, whose other programs may use it");
        case Evidence::UsedBySimilarNamedApp:
            return tr("May belong to another program with a similar name");
        case Evidence::OtherUserData:
            return tr("In another user's profile");
        case Evidence::QuestionableDirectoryName:
            return tr("Generic name, such as “data” or “config”");
        case Evidence::StartupIsRunOnce:
            return tr("Runs only once (RunOnce)");
        case Evidence::DirectoryStillUsed:
            return tr("Another installed program uses this folder");
        case Evidence::StoreApp:
            return tr("Store app data, which Windows manages itself");
    }
    return {};
}

QString LeftoverText::evidenceSummary(std::span<const leftovers::Record> records) {
    // Depth only fine-tunes the score; the list shows what actually matched.
    std::vector<const leftovers::Record*> sorted;
    for (const auto& record : records) {
        if (record.evidence != Evidence::Depth && leftovers::points(record) != 0) {
            sorted.push_back(&record);
        }
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
        return std::abs(leftovers::points(*a)) > std::abs(leftovers::points(*b));
    });
    QStringList for_it;
    QStringList against;
    for (const auto* record : sorted) {
        (leftovers::points(*record) > 0 ? for_it : against) << evidenceText(*record);
    }
    const QString separator = u"; "_s;
    if (against.isEmpty()) {
        return for_it.join(separator);
    }
    if (for_it.isEmpty()) {
        return tr("Against it: %1").arg(against.join(separator));
    }
    return tr("%1; against it: %2").arg(for_it.join(separator), against.join(separator));
}

QString LeftoverText::evidenceDetails(std::span<const leftovers::Record> records) {
    QString rows;
    for (const auto& record : records) {
        QString text = evidenceText(record).toHtmlEscaped();
        if (!record.detail.empty()) {
            text += u"<br><small>"_s + toQString(record.detail).toHtmlEscaped() + u"</small>"_s;
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
