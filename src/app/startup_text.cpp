#include "startup_text.h"

#include <windows.h>

#include "leftover_text.h"
#include "program_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using tools::StartupAction;
using tools::StartupKind;
using tools::StartupState;

QString StartupText::nameText(const tools::StartupEntry& entry) {
    return toQString(entry.display_name.empty() ? entry.name : entry.display_name);
}

QString StartupText::kindLabel(StartupKind kind) {
    switch (kind) {
        case StartupKind::Run:
            return tr("Run");
        case StartupKind::RunOnce:
            return tr("RunOnce");
        case StartupKind::RunOnceEx:
            return tr("RunOnceEx");
        case StartupKind::StartupFolder:
            return tr("Startup folder");
        case StartupKind::Task:
            return tr("Scheduled task");
        case StartupKind::Service:
            return tr("Service");
    }
    return {};
}

QString StartupText::sourceText(const tools::StartupEntry& entry) {
    const QString kind = kindLabel(entry.kind);
    return entry.wow64 ? tr("%1 (32-bit)").arg(kind) : kind;
}

QString StartupText::stateText(const tools::StartupEntry& entry) {
    if (entry.state == StartupState::Enabled) {
        return tr("Enabled");
    }
    if (entry.kind == StartupKind::Service && entry.service_start == SERVICE_DISABLED) {
        return tr("Disabled completely");
    }
    return tr("Disabled");
}

QString StartupText::stateDetails(const tools::StartupEntry& entry) {
    const bool enabled = entry.state == StartupState::Enabled;
    switch (entry.kind) {
        case StartupKind::Run:
        case StartupKind::StartupFolder:
            if (!entry.approval) {
                return tr(
                    "Runs at every sign-in. Windows keeps no enabled or disabled state for this key, so "
                    "it can only be deleted.");
            }
            return enabled ? tr("Enabled in Task Manager's list of startup apps")
                           : tr("Disabled in Task Manager's list of startup apps");
        case StartupKind::RunOnce:
            return tr(
                "Runs once at the next sign-in, then Windows deletes it. It has no disabled state, so "
                "it can only be deleted.");
        case StartupKind::RunOnceEx:
            return tr(
                "Runs once at the next start, then Windows deletes it. It has no disabled state, so it "
                "can only be deleted.");
        case StartupKind::Task:
            return entry.task_enabled ? tr("The scheduled task is enabled")
                                      : tr("The scheduled task is disabled");
        case StartupKind::Service:
            return tr("Start type: %1")
                .arg(LeftoverText::serviceStartText({entry.service_start, entry.service_delayed}));
    }
    return {};
}

QString StartupText::userText(const tools::StartupEntry& entry) {
    if (entry.user_sid.empty()) {
        return tr("Machine");
    }
    return toQString(entry.user_name.empty() ? entry.user_sid : entry.user_name);
}

QString StartupText::publisherText(const tools::StartupEntry& entry) {
    if (entry.signer.valid && !entry.signer.name.empty()) {
        return toQString(entry.signer.name);
    }
    return toQString(entry.product.company_name);
}

QString StartupText::signatureText(const tools::StartupSigner& signer) {
    if (!signer.checked) {
        return tr("Not checked");
    }
    if (signer.valid) {
        return signer.name.empty() ? tr("Valid") : tr("Valid, signed by %1").arg(toQString(signer.name));
    }
    return signer.name.empty() ? tr("Not signed")
                               : tr("Not valid (signed by %1)").arg(toQString(signer.name));
}

QString StartupText::issueText(tools::StartupPlanIssueCode code, StartupAction action) {
    using tools::StartupPlanIssueCode;
    switch (code) {
        case StartupPlanIssueCode::UnknownId:
            return tr("No longer listed; refresh with F5");
        case StartupPlanIssueCode::Protected:
            return tr("Windows' own entry: Avel never changes it");
        case StartupPlanIssueCode::NotSupported:
            return tr("Has no disabled state: it can only be deleted");
        case StartupPlanIssueCode::AlreadyInState:
            return action == StartupAction::Enable ? tr("Already enabled") : tr("Already disabled");
    }
    return {};
}

QString StartupText::diagnosticText(const tools::StartupDiagnostic& diagnostic) {
    using tools::StartupDiagnosticCode;
    QString text;
    switch (diagnostic.code) {
        case StartupDiagnosticCode::TasksUnavailable:
            text = tr("The Task Scheduler couldn't be read, so no scheduled tasks are listed.");
            break;
        case StartupDiagnosticCode::ServicesUnavailable:
            text = tr("The services couldn't be read, so no services are listed.");
            break;
    }
    if (diagnostic.error != 0) {
        text += u' ' + ProgramText::systemErrorText(static_cast<std::uint32_t>(diagnostic.error));
    }
    return text;
}

QString StartupText::planDescription(StartupAction action, tools::ServiceDisable serviceDisable, int count,
                                     const QString& name) {
    const bool completely = serviceDisable == tools::ServiceDisable::Disabled;
    if (count == 1 && !name.isEmpty()) {
        switch (action) {
            case StartupAction::Disable:
                return completely ? tr("Disable startup entry %1 completely").arg(name)
                                  : tr("Disable startup entry %1").arg(name);
            case StartupAction::Enable:
                return tr("Enable startup entry %1").arg(name);
            case StartupAction::Delete:
                return tr("Delete startup entry %1").arg(name);
        }
    }
    switch (action) {
        case StartupAction::Disable:
            return completely ? tr("Disable %n startup entry(s) completely", nullptr, count)
                              : tr("Disable %n startup entry(s)", nullptr, count);
        case StartupAction::Enable:
            return tr("Enable %n startup entry(s)", nullptr, count);
        case StartupAction::Delete:
            return tr("Delete %n startup entry(s)", nullptr, count);
    }
    return {};
}

}  // namespace avel::app
