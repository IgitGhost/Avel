#pragma once

#include <QColor>
#include <QCoreApplication>
#include <QString>

#include <span>

#include "core/changes/change_set.h"
#include "core/leftovers/confidence.h"
#include "core/leftovers/environment.h"
#include "core/leftovers/leftover.h"
#include "core/platform/secure_folder.h"

namespace avel::app {

// Translated, user-facing text for the leftover scan's and the change sets' codes.
class LeftoverText {
    Q_DECLARE_TR_FUNCTIONS(LeftoverText)

public:
    static QString categoryLabel(leftovers::Category category);
    // "Folder", "Registry value", "Scheduled task", ...
    static QString kindLabel(const leftovers::Location& location, leftovers::Category category);
    // The path, key, service or task name. Keys in the current user's hive read HKCU\...
    static QString locationText(const leftovers::Location& location);

    static QString confidenceLabel(leftovers::Confidence confidence);
    static QColor confidenceColor(leftovers::Confidence confidence);

    // One piece of evidence, e.g. "Points into the program's folder (COM server)".
    static QString evidenceText(const leftovers::Record& record);
    // The column text: the evidence for, strongest first, then what speaks against it.
    static QString evidenceSummary(std::span<const leftovers::Record> records);
    // The tooltip: every record with its points and what matched, and the score.
    static QString evidenceDetails(std::span<const leftovers::Record> records);
    static QString referenceText(leftovers::ReferenceKind kind);
    static QString diagnosticText(const leftovers::ScanDiagnostic& diagnostic);

    static QString itemStateText(changes::ItemState state);
    static QString reasonText(changes::Reason reason);
    // "Skipped: in use by a running program", with the system's text for an error code.
    static QString statusText(const changes::ItemStatus& status);
    static QString setStateText(changes::SetState state);
    static QString folderStatusText(platform::FolderStatus status);
};

}  // namespace avel::app
