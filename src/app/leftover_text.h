#pragma once

#include <QColor>
#include <QCoreApplication>
#include <QString>

#include <span>
#include <string_view>

#include "core/changes/change_set.h"
#include "core/leftovers/confidence.h"
#include "core/leftovers/environment.h"
#include "core/leftovers/leftover.h"
#include "core/leftovers/scan.h"
#include "core/platform/secure_folder.h"

namespace avel::app {

// Translated, user-facing text for the leftover scan's and the change sets' codes.
class LeftoverText {
    Q_DECLARE_TR_FUNCTIONS(LeftoverText)

public:
    static QString categoryLabel(leftovers::Category category);
    // "Folder", "Registry value", "Scheduled task", ...
    static QString kindLabel(const leftovers::Location& location, leftovers::Category category);
    // The path, key, service or task name. Keys in the current user's hive read HKCU\... A value
    // edit is described in words (valueEditText), and so is a state change ("Disable Contoso at
    // startup (HKCU\...\StartupApproved\Run)", "Change the start type of the service ContosoSvc
    // from Automatic to Manual").
    static QString locationText(const leftovers::Location& location);
    // "Automatic (delayed start)", "Manual", "Disabled", as the Services console says it.
    static QString serviceStartText(const leftovers::ServiceStart& start);
    // "Remove C:\Contoso\bin from the system variable Path", "Lower the shared DLL count of
    // C:\Program Files\Common Files\x.dll from 2 to 1", "Delete the user variable CONTOSO_HOME,
    // which the installer created", "Restore the system variable X to its value before the
    // installation".
    static QString valueEditText(const leftovers::ValueEditLocation& edit);
    // For a tooltip: the value the edit changes, and its data now and afterwards when it is text.
    static QString valueEditDetails(const leftovers::ValueEditLocation& edit);
    // How an install trace saw an item that existed before the installation change.
    static QString tracedChangeText(leftovers::TracedChange change);
    // What a program's install trace brought to the scan, in a sentence.
    static QString traceUseText(const leftovers::TraceUse& use, const QString& program);

    static QString confidenceLabel(leftovers::Confidence confidence);
    static QColor confidenceColor(leftovers::Confidence confidence);

    // One piece of evidence, e.g. "Points into the program's folder (COM server)".
    static QString evidenceText(const leftovers::Record& record);
    // The column text: the evidence for, strongest first, then what speaks against it.
    static QString evidenceSummary(std::span<const leftovers::Record> records);
    // The tooltip: every record with its points and what matched, and the score.
    static QString evidenceDetails(std::span<const leftovers::Record> records);
    static QString referenceText(leftovers::ReferenceKind kind);
    // Why an install trace's item is tied to the program, from the tracker's code for it
    // (tracker::TieReason, the detail of Evidence::TracedInstallRoot); empty for an unknown code.
    static QString tieText(std::wstring_view code);
    static QString diagnosticText(const leftovers::ScanDiagnostic& diagnostic);

    static QString itemStateText(changes::ItemState state);
    static QString reasonText(changes::Reason reason);
    // "Skipped: in use by a running program", with the system's text for an error code.
    static QString statusText(const changes::ItemStatus& status);
    // The same, worded for the item: a value edit that was made is "Edited", not "Removed"; a state
    // change reads "Disabled", "Enabled" or "Start type changed", and "...: its state has changed
    // since" when compare-and-set left it alone.
    static QString statusText(const changes::ItemStatus& status, const leftovers::Location& location);
    static QString setStateText(changes::SetState state);
    static QString folderStatusText(platform::FolderStatus status);
};

}  // namespace avel::app
