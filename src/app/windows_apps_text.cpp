#include "windows_apps_text.h"

#include <algorithm>

#include "program_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using tools::AppCategory;
using tools::AppProtection;
using tools::AppWarning;

QString WindowsAppsText::nameText(const tools::WindowsApp& app) {
    return toQString(app.display_name.empty() ? app.name : app.display_name);
}

QString WindowsAppsText::categoryText(AppCategory category) {
    switch (category) {
        case AppCategory::Other:
            return tr("Other");
        case AppCategory::OftenRemoved:
            return tr("Often removed");
        case AppCategory::SemiCritical:
            return tr("Semi-critical");
        case AppCategory::Critical:
            return tr("Critical");
    }
    return {};
}

QString WindowsAppsText::categoryColumnText(const tools::WindowsApp& app) {
    switch (app.protection) {
        case AppProtection::System:
            return tr("Part of Windows");
        case AppProtection::Critical:
            return categoryText(AppCategory::Critical);
        case AppProtection::ExternalLocation:
            return tr("Part of a desktop program");
        case AppProtection::None:
            break;
    }
    const QString category = categoryText(app.category);
    return app.category == AppCategory::SemiCritical ? u"⚠ "_s + category : category;
}

QString WindowsAppsText::warningText(AppWarning warning) {
    switch (warning) {
        case AppWarning::None:
            return {};
        case AppWarning::OpensFilesByDefault:
            return tr(
                "It opens a common type of file by default: without it, those files ask which app "
                "to open them with.");
        case AppWarning::DefaultTerminal:
            return tr("Console programs open in it: without it, they open in the old console window.");
        case AppWarning::BrowserIntegration:
            return tr(
                "Microsoft Edge's integration with Windows and the web content of other apps go "
                "with it.");
        case AppWarning::GameSignIn:
            return tr("Games sign in to Xbox and show its notifications through it.");
        case AppWarning::GameBar:
            return tr("Game Bar goes with it: recording, screenshots and performance overlays in games.");
        case AppWarning::GamingServices:
            return tr("Games from the Microsoft Store and PC Game Pass need it to start and update.");
        case AppWarning::Widgets:
            return tr("The Widgets board, the taskbar's weather and the lock screen's widgets go with it.");
        case AppWarning::ScreenCapture:
            return tr("Win+Shift+S and the Print Screen key take screenshots with it.");
        case AppWarning::MediaFormats:
            return tr("A media extension: apps can't open or play its formats without it.");
        case AppWarning::CloudFiles:
            return tr(
                "OneDrive's integration with File Explorer goes with it: files on demand and their "
                "status.");
        case AppWarning::WingetCatalog:
            return tr(
                "winget's catalog of apps (its \"winget\" source) goes with it: winget "
                "can't find or install apps from that source.");
        case AppWarning::DisplayLanguage:
            return tr("A language pack: Windows' display language may change back without it.");
        case AppWarning::DefaultMediaPlayer:
            return tr(
                "Music and videos open in it by default: without it, they ask which app to open "
                "them with.");
        case AppWarning::GamePass:
            return tr("PC Game Pass games are installed, started and updated with it.");
    }
    return {};
}

QString WindowsAppsText::protectionText(const tools::WindowsApp& app) {
    switch (app.protection) {
        case AppProtection::None:
            return {};
        case AppProtection::System:
            return tr(
                "Signed as part of Windows: Windows doesn't let it be removed, and Avel never "
                "changes it.");
        case AppProtection::Critical:
            return tr("Windows or other apps need it, so Avel never changes it.");
        case AppProtection::ExternalLocation:
            return tr("Part of the desktop program in %1: it gives that program its context menus, "
                      "notifications or its integration with Windows. It goes when that program is "
                      "uninstalled; removed alone, the program breaks, so Avel never changes it.")
                .arg(toQString(app.external_location));
    }
    return {};
}

QString WindowsAppsText::installedForText(const tools::WindowsApp& app) {
    const bool current =
        std::any_of(app.users.begin(), app.users.end(), [](const auto& user) { return user.current; });
    const int others = static_cast<int>(app.users.size()) - (current ? 1 : 0);
    QString text;
    if (current) {
        text = others > 0 ? tr("You and %n other user(s)", nullptr, others) : tr("You");
    } else if (others > 0) {
        text = tr("%n other user(s)", nullptr, others);
    }
    if (app.provisioned) {
        text = text.isEmpty() ? tr("New users") : tr("%1, new users").arg(text);
    }
    return text.isEmpty() ? tr("Nobody") : text;
}

QString WindowsAppsText::signatureText(platform::PackageSignatureKind kind) {
    using platform::PackageSignatureKind;
    switch (kind) {
        case PackageSignatureKind::None:
            return tr("Not signed");
        case PackageSignatureKind::Developer:
            return tr("Developer");
        case PackageSignatureKind::Enterprise:
            return tr("Enterprise");
        case PackageSignatureKind::Store:
            return tr("Microsoft Store");
        case PackageSignatureKind::System:
            return tr("Part of Windows");
    }
    return {};
}

QString WindowsAppsText::issueText(tools::WindowsAppsPlanIssueCode code, WindowsAppsAction action) {
    using Code = tools::WindowsAppsPlanIssueCode;
    switch (code) {
        case Code::UnknownId:
            return tr("No longer listed; refresh with F5");
        case Code::Protected:
            return tr("Protected: Avel never changes it");
        case Code::IncompleteListing:
            return tr("Avel isn't running as administrator, so who else has it isn't known");
        case Code::NotInstalledForCurrentUser:
            return tr("You don't have it");
        case Code::WouldDeletePackage:
            return action == WindowsAppsAction::RemoveForMe
                       ? tr("Nobody else has it and it isn't installed for new users, so Windows would "
                            "delete it and undo couldn't bring it back; only \"Remove for everyone\" removes "
                            "it")
                       : tr("No user has it, so Windows may delete it and undo couldn't bring it back; "
                            "only \"Remove for everyone\" removes it");
        case Code::NotProvisioned:
            return tr("It isn't installed for new users");
        case Code::IrreversibleNeedsConfirmation:
            return tr("Removing it for everyone can't be undone, and that wasn't confirmed");
    }
    return {};
}

QString WindowsAppsText::actionName(WindowsAppsAction action) {
    switch (action) {
        case WindowsAppsAction::RemoveForMe:
            return tr("Remove for me");
        case WindowsAppsAction::Deprovision:
            return tr("Don't install for new users");
        case WindowsAppsAction::RemoveForEveryone:
            return tr("Remove for everyone");
    }
    return {};
}

QString WindowsAppsText::actionDetails(WindowsAppsAction action) {
    switch (action) {
        case WindowsAppsAction::RemoveForMe:
            return tr(
                "Removes the app for you only; it stays on this PC for the other users or for new "
                "users. Undo installs it for you again, but your data in it (settings, saved "
                "files of its own) is lost.");
        case WindowsAppsAction::Deprovision:
            return tr(
                "New user accounts no longer get the app; those who have it keep it. Undo makes "
                "Windows install it for new users again.");
        case WindowsAppsAction::RemoveForEveryone:
            return tr(
                "Removes the app for every user and deletes it from this PC. It can't be undone: "
                "every user's data in it is lost, and it comes back only from the Microsoft Store "
                "or winget.");
    }
    return {};
}

QString WindowsAppsText::planDescription(WindowsAppsAction action, int count, const QString& name) {
    if (count == 1 && !name.isEmpty()) {
        switch (action) {
            case WindowsAppsAction::RemoveForMe:
                return tr("Remove %1 for me").arg(name);
            case WindowsAppsAction::Deprovision:
                return tr("Don't install %1 for new users").arg(name);
            case WindowsAppsAction::RemoveForEveryone:
                return tr("Remove %1 for everyone").arg(name);
        }
    }
    switch (action) {
        case WindowsAppsAction::RemoveForMe:
            return tr("Remove %n app(s) for me", nullptr, count);
        case WindowsAppsAction::Deprovision:
            return tr("Don't install %n app(s) for new users", nullptr, count);
        case WindowsAppsAction::RemoveForEveryone:
            return tr("Remove %n app(s) for everyone", nullptr, count);
    }
    return {};
}

QString WindowsAppsText::incompleteListingText() {
    return tr(
        "Avel isn't running as administrator, so only your apps are listed, and who else has them "
        "and whether they are installed for new users aren't known. Nothing can be changed here.");
}

QString WindowsAppsText::listingErrorText(std::int32_t error) {
    return tr("The list of apps was cut short, so some may be missing.") + u' ' +
           ProgramText::systemErrorText(static_cast<std::uint32_t>(error));
}

}  // namespace avel::app
