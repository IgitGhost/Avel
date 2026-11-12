#pragma once

#include <QCoreApplication>
#include <QString>

#include "core/tools/startup.h"
#include "core/tools/startup_changes.h"

namespace avel::app {

// Translated, user-facing text for the Startup manager's codes (tools/startup, startup_changes).
class StartupText {
    Q_DECLARE_TR_FUNCTIONS(StartupText)

public:
    // A service's display name, otherwise the value's, file's or task's name.
    static QString nameText(const tools::StartupEntry& entry);
    // "Run", "RunOnce", "RunOnceEx", "Startup folder", "Scheduled task", "Service".
    static QString kindLabel(tools::StartupKind kind);
    // The kind, with "(32-bit)" for an entry of a WOW6432Node key.
    static QString sourceText(const tools::StartupEntry& entry);
    // "Enabled", "Disabled"; a service whose start is Disabled: "Disabled completely".
    static QString stateText(const tools::StartupEntry& entry);
    // How the state is kept, in a sentence: "Disabled in Task Manager's list of startup apps",
    // "Start type: Automatic (delayed start)", "The task is enabled", ...
    static QString stateDetails(const tools::StartupEntry& entry);
    // "Machine" (every user), or the user's account name (their SID when it isn't known).
    static QString userText(const tools::StartupEntry& entry);
    // The signer when the signature is valid, otherwise the company of the file's version resource.
    static QString publisherText(const tools::StartupEntry& entry);
    // "Valid, signed by Contoso Ltd", "Not signed", "Not checked", ...
    static QString signatureText(const tools::StartupSigner& signer);
    // Why an entry was left out of a change.
    static QString issueText(tools::StartupPlanIssueCode code, tools::StartupAction action);
    static QString diagnosticText(const tools::StartupDiagnostic& diagnostic);
    // The change set's description, shown in the Undo Center: "Disable startup entry Contoso",
    // "Delete 3 startup entries". `name` names the entry when there is only one.
    static QString planDescription(tools::StartupAction action, tools::ServiceDisable serviceDisable,
                                   int count, const QString& name);
};

}  // namespace avel::app
