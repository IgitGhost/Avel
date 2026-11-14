#pragma once

#include <QCoreApplication>
#include <QString>

#include <cstdint>

#include "core/tools/windows_apps.h"
#include "core/tools/windows_apps_changes.h"
#include "core/tools/windows_apps_data.h"

namespace avel::app {

// The Windows apps window's three ways to remove an app (tools/windows_apps_changes).
enum class WindowsAppsAction : std::uint8_t {
    RemoveForMe,        // The current user's registration; undoable
    Deprovision,        // "Don't install for new users"; undoable
    RemoveForEveryone,  // Every user, deleted from this PC; irreversible
};

// Translated, user-facing text for the Windows apps tool's codes (tools/windows_apps,
// windows_apps_data, windows_apps_changes).
class WindowsAppsText {
    Q_DECLARE_TR_FUNCTIONS(WindowsAppsText)

public:
    // The display name, else the identity name.
    static QString nameText(const tools::WindowsApp& app);
    // "Often removed", "Semi-critical", "Critical", "Other".
    static QString categoryText(tools::AppCategory category);
    // The Category column: the protection of a protected app ("Part of Windows", "Critical", "Part of
    // a desktop program"), else the category, with a warning mark for a semi-critical one.
    static QString categoryColumnText(const tools::WindowsApp& app);
    // What removing a semi-critical app breaks, in a sentence; empty for None.
    static QString warningText(tools::AppWarning warning);
    // Why a protected app is never changed, in a sentence; empty when it isn't protected.
    static QString protectionText(const tools::WindowsApp& app);
    // The Installed for column: "You", "You and 2 other users", "3 users", with "new users" when it is
    // provisioned; "Nobody" when it is only staged.
    static QString installedForText(const tools::WindowsApp& app);
    // "Store", "Signed as part of Windows", ...
    static QString signatureText(platform::PackageSignatureKind kind);
    // Why an app was left out of a change.
    static QString issueText(tools::WindowsAppsPlanIssueCode code, WindowsAppsAction action);
    // The action's name, as its button says it, without an ellipsis or accelerator.
    static QString actionName(WindowsAppsAction action);
    // What the action does, and whether and how it can be undone.
    static QString actionDetails(WindowsAppsAction action);
    // The change set's description, shown in the Undo Center: "Remove Microsoft News for me",
    // "Don't install 3 apps for new users", "Remove Clipchamp for everyone". `name` names the app
    // when there is only one.
    static QString planDescription(WindowsAppsAction action, int count, const QString& name);
    // Why the list can't be changed: it was made without administrator rights.
    static QString incompleteListingText();
    // The listing was cut short by this HRESULT.
    static QString listingErrorText(std::int32_t error);
};

}  // namespace avel::app
