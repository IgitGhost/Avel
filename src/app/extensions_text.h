#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <string>

#include "core/leftovers/leftover.h"
#include "core/tools/browser_extensions.h"
#include "core/tools/browser_extensions_changes.h"

namespace avel::app {

// Translated, user-facing text for the browser extensions tool's codes (tools/browser_extensions,
// browser_extensions_changes).
class ExtensionsText {
    Q_DECLARE_TR_FUNCTIONS(ExtensionsText)

public:
    // "Google Chrome", "Microsoft Edge Beta", "Firefox", ...
    static QString browserName(tools::BrowserKind browser);
    // The manifest's name, else the extension id.
    static QString nameText(const tools::BrowserExtension& entry);
    // "Machine (all users)" for a machine source's entry, else the account name (the SID when it
    // isn't known), with "(you)" for `currentSid`.
    static QString userText(const std::wstring& sid, const std::wstring& name,
                            const std::wstring& currentSid);
    // The profile's display name, else its folder; "Not in a profile" for a source-only entry.
    static QString profileText(const tools::BrowserExtension& entry);
    // "Enabled", "Disabled"; "Not installed" for a source-only entry.
    static QString stateText(const tools::BrowserExtension& entry);
    // The State column's tooltip and the details' line: why it is disabled, or what a source-only
    // entry is.
    static QString stateDetails(const tools::BrowserExtension& entry);
    // Chromium's disable reasons, one per bit, in words ("By you", "Asks for new permissions", ...).
    static QStringList disableReasons(std::uint32_t reasons);
    // "From the store", "By the user", "Developer", "Policy", ...
    static QString installText(tools::ExtensionInstall install);
    // What the install code means, in a sentence.
    static QString installDetails(tools::ExtensionInstall install);
    // "Chrome Web Store", "Edge Add-ons", "Opera add-ons", "Firefox Add-ons"; empty for None.
    static QString storeText(tools::ExtensionStore store);
    // The Removal column: "Removable", "In the browser", "policies.json", "Not supported".
    static QString removalText(tools::ExtensionRemoval removal);
    // Why it can or can't be removed here, in a sentence.
    static QString removalDetails(tools::ExtensionRemoval removal);
    // "Forced install policy (ExtensionInstallForcelist)", "External registration", ...
    static QString sourceKindText(tools::ExtensionSourceKind kind);
    // What the source is for, in a sentence.
    static QString sourceKindDetails(tools::ExtensionSourceKind kind);
    // "Your own copy from the store stays; remove it on the browser's extensions page."
    static QString keepsUserCopyText();
    // "Not read by the browser", and why.
    static QString ignoredByBrowserText();
    // The browser drops what the removed sources installed at its next start.
    static QString nextStartText();
    // The machine is managed: removed policy entries may be written again.
    static QString managedText();
    // A plan item, in words: "Delete the registry key HKLM\...", "Change HKLM\...\1 from “a” to “b”",
    // "Delete the value HKLM\... : 2 (“c”)".
    static QString itemText(const leftovers::Location& location);
    // Why a selected extension was left out of a removal.
    static QString issueText(tools::ExtensionPlanIssueCode code);
    static QString diagnosticText(const tools::ExtensionDiagnostic& diagnostic);
    // Install time, in the UI locale; empty when unknown.
    static QString installTimeText(std::optional<std::int64_t> milliseconds);
    // The change set's description, shown in the Undo Center: "Remove browser extension Contoso
    // Helper", "Remove 3 browser extensions". `name` names the extension when there is only one.
    static QString planDescription(int count, const QString& name);
};

}  // namespace avel::app
