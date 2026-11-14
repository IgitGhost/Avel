#include "extensions_text.h"

#include <QDateTime>
#include <QLocale>

#include <variant>

#include "core/leftovers/value_edits.h"
#include "core/platform/text.h"
#include "leftover_text.h"
#include "program_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using tools::BrowserKind;
using tools::ExtensionInstall;
using tools::ExtensionRemoval;
using tools::ExtensionSourceKind;
using tools::ExtensionStore;

namespace {

// A registry value's data as text, when it is text.
QString valueText(const std::optional<platform::RawValue>& value) {
    if (!value) {
        return {};
    }
    const auto text = leftovers::string_value_text(*value);
    return text ? toQString(*text) : QString();
}

}  // namespace

QString ExtensionsText::browserName(BrowserKind browser) {
    switch (browser) {
        case BrowserKind::Chrome:
            return tr("Google Chrome");
        case BrowserKind::ChromeBeta:
            return tr("Google Chrome Beta");
        case BrowserKind::ChromeDev:
            return tr("Google Chrome Dev");
        case BrowserKind::ChromeCanary:
            return tr("Google Chrome Canary");
        case BrowserKind::Edge:
            return tr("Microsoft Edge");
        case BrowserKind::EdgeBeta:
            return tr("Microsoft Edge Beta");
        case BrowserKind::EdgeDev:
            return tr("Microsoft Edge Dev");
        case BrowserKind::EdgeCanary:
            return tr("Microsoft Edge Canary");
        case BrowserKind::Brave:
            return tr("Brave");
        case BrowserKind::Vivaldi:
            return tr("Vivaldi");
        case BrowserKind::Opera:
            return tr("Opera");
        case BrowserKind::OperaGx:
            return tr("Opera GX");
        case BrowserKind::Chromium:
            return tr("Chromium");
        case BrowserKind::Firefox:
            return tr("Firefox");
    }
    return {};
}

QString ExtensionsText::nameText(const tools::BrowserExtension& entry) {
    return toQString(entry.name.empty() ? entry.extension_id : entry.name);
}

QString ExtensionsText::userText(const std::wstring& sid, const std::wstring& name,
                                 const std::wstring& currentSid) {
    if (sid.empty()) {
        return tr("Machine (all users)");
    }
    const QString text = toQString(name.empty() ? sid : name);
    return !currentSid.empty() && platform::iequals(sid, currentSid) ? tr("%1 (you)").arg(text) : text;
}

QString ExtensionsText::profileText(const tools::BrowserExtension& entry) {
    if (!entry.in_profile) {
        return tr("Not in a profile");
    }
    return toQString(entry.profile_name.empty() ? entry.profile_directory : entry.profile_name);
}

QString ExtensionsText::stateText(const tools::BrowserExtension& entry) {
    if (!entry.in_profile) {
        return tr("Not installed");
    }
    return entry.enabled ? tr("Enabled") : tr("Disabled");
}

QString ExtensionsText::stateDetails(const tools::BrowserExtension& entry) {
    if (!entry.in_profile) {
        return tr(
            "No profile has it: the browser installs it at its next start, or the browser it is meant "
            "for isn't used here.");
    }
    if (entry.enabled) {
        return {};
    }
    const QStringList reasons = disableReasons(entry.disable_reasons);
    return reasons.isEmpty() ? QString() : tr("Disabled: %1").arg(reasons.join(u"; "_s));
}

QStringList ExtensionsText::disableReasons(std::uint32_t reasons) {
    // Chromium's extensions::disable_reason::DisableReason bits.
    QStringList texts;
    std::uint32_t unknown = 0;
    for (std::uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((reasons & bit) == 0) {
            continue;
        }
        switch (bit) {
            case 1u << 0:
                texts << tr("by the user");
                break;
            case 1u << 1:
                texts << tr("it asks for new permissions");
                break;
            case 1u << 2:
                texts << tr("being reloaded");
                break;
            case 1u << 3:
                texts << tr("a requirement isn't met");
                break;
            case 1u << 4:
                texts << tr("installed by another program, turned off by the browser");
                break;
            case 1u << 8:
                texts << tr("not verified by the store");
                break;
            case 1u << 9:
                texts << tr("flagged by Safe Browsing");
                break;
            case 1u << 10:
                texts << tr("its files are corrupted");
                break;
            case 1u << 11:
                texts << tr("installed remotely, waiting for approval");
                break;
            case 1u << 13:
                texts << tr("installed by another program, waiting for the user's approval");
                break;
            case 1u << 14:
                texts << tr("a policy requires an update");
                break;
            case 1u << 15:
                texts << tr("waiting for a parent's approval");
                break;
            case 1u << 16:
                texts << tr("blocked by a policy");
                break;
            case 1u << 17:
                texts << tr("being reinstalled");
                break;
            case 1u << 18:
                texts << tr("not allowed by Enhanced Safe Browsing");
                break;
            case 1u << 20:
                texts << tr("a policy allows only extensions from the store");
                break;
            case 1u << 21:
                texts << tr("its manifest version is no longer supported");
                break;
            case 1u << 22:
                texts << tr("developer extensions are turned off");
                break;
            default:
                unknown |= bit;
                break;
        }
    }
    if (unknown != 0) {
        texts << tr("other reasons (code %1)").arg(unknown);
    }
    return texts;
}

QString ExtensionsText::installText(ExtensionInstall install) {
    switch (install) {
        case ExtensionInstall::Store:
            return tr("From the store");
        case ExtensionInstall::User:
            return tr("By the user");
        case ExtensionInstall::Developer:
            return tr("Developer");
        case ExtensionInstall::ExternalPref:
            return tr("External file");
        case ExtensionInstall::ExternalRegistry:
            return tr("Registry");
        case ExtensionInstall::Policy:
            return tr("Policy");
        case ExtensionInstall::Sideload:
            return tr("Another program");
        case ExtensionInstall::Unknown:
            break;
    }
    return tr("Unknown");
}

QString ExtensionsText::installDetails(ExtensionInstall install) {
    switch (install) {
        case ExtensionInstall::Store:
            return tr("Installed by the user from the browser's store.");
        case ExtensionInstall::User:
            return tr("Installed by the user, not from the store (a file or another site).");
        case ExtensionInstall::Developer:
            return tr("Loaded by a developer: unpacked, from the command line, or a temporary add-on.");
        case ExtensionInstall::ExternalPref:
            return tr("Installed by a program, through a file next to the browser or the program.");
        case ExtensionInstall::ExternalRegistry:
            return tr("Installed by a program, through a registration in the registry.");
        case ExtensionInstall::Policy:
            return tr("Installed by a policy.");
        case ExtensionInstall::Sideload:
            return tr("Put in a Firefox extensions folder by another program.");
        case ExtensionInstall::Unknown:
            break;
    }
    return tr("The profile doesn't say how it was installed.");
}

QString ExtensionsText::storeText(ExtensionStore store) {
    switch (store) {
        case ExtensionStore::ChromeWebStore:
            return tr("Chrome Web Store");
        case ExtensionStore::EdgeAddons:
            return tr("Edge Add-ons");
        case ExtensionStore::OperaAddons:
            return tr("Opera add-ons");
        case ExtensionStore::Mozilla:
            return tr("Firefox Add-ons");
        case ExtensionStore::None:
            break;
    }
    return {};
}

QString ExtensionsText::removalText(ExtensionRemoval removal) {
    switch (removal) {
        case ExtensionRemoval::Removable:
            return tr("Removable");
        case ExtensionRemoval::NotRemovable:
            return tr("In the browser");
        case ExtensionRemoval::ManagedByPoliciesJson:
            return tr("policies.json");
        case ExtensionRemoval::NotSupported:
            return tr("Not supported");
    }
    return {};
}

QString ExtensionsText::removalDetails(ExtensionRemoval removal) {
    switch (removal) {
        case ExtensionRemoval::Removable:
            return tr(
                "Something outside the browser installs it: Avel removes the policy entries and "
                "registrations below, and the browser removes the extension at its next start.");
        case ExtensionRemoval::NotRemovable:
            return tr(
                "Nothing outside the browser installs it, so the browser removes it itself: open its "
                "extensions page and remove it there.");
        case ExtensionRemoval::ManagedByPoliciesJson:
            return tr(
                "Firefox's policies.json installs it. That file belongs to whoever set Firefox up, and Avel "
                "doesn't edit it.");
        case ExtensionRemoval::NotSupported:
            return tr(
                "A policy names it together with other extensions in one registry key. Avel can't take "
                "out one of them without renaming the key.");
    }
    return {};
}

QString ExtensionsText::sourceKindText(ExtensionSourceKind kind) {
    switch (kind) {
        case ExtensionSourceKind::ForceList:
            return tr("Forced install policy (ExtensionInstallForcelist)");
        case ExtensionSourceKind::Settings:
            return tr("Extension settings policy (ExtensionSettings)");
        case ExtensionSourceKind::ExternalRegistry:
            return tr("External registration");
        case ExtensionSourceKind::FirefoxRegistry:
            return tr("Firefox registration");
        case ExtensionSourceKind::FirefoxInstall:
            return tr("Firefox install policy (Extensions\\Install)");
        case ExtensionSourceKind::FirefoxLocked:
            return tr("Firefox locked policy (Extensions\\Locked)");
        case ExtensionSourceKind::PoliciesJson:
            return tr("Firefox's policies.json");
    }
    return {};
}

QString ExtensionsText::sourceKindDetails(ExtensionSourceKind kind) {
    switch (kind) {
        case ExtensionSourceKind::ForceList:
            return tr("The browser installs it and the user can't remove or turn it off.");
        case ExtensionSourceKind::Settings:
            return tr("The policy installs it, forced or as a default the user can turn off.");
        case ExtensionSourceKind::ExternalRegistry:
            return tr("A program asks the browser to install it.");
        case ExtensionSourceKind::FirefoxRegistry:
            return tr("A program points Firefox at its add-on file.");
        case ExtensionSourceKind::FirefoxInstall:
            return tr("Firefox installs the add-on from this address or file.");
        case ExtensionSourceKind::FirefoxLocked:
            return tr("The user can't turn the add-on off or remove it.");
        case ExtensionSourceKind::PoliciesJson:
            return tr("Avel doesn't edit this file.");
    }
    return {};
}

QString ExtensionsText::keepsUserCopyText() {
    return tr("Your own copy from the store stays; remove it on the browser's extensions page.");
}

QString ExtensionsText::ignoredByBrowserText() {
    return tr("Not read by the browser: removing it changes nothing in the browser, only cleans up.");
}

QString ExtensionsText::nextStartText() {
    return tr(
        "The browser removes what these sources installed at its next start; while it runs, it keeps the "
        "extension.");
}

QString ExtensionsText::managedText() {
    return tr(
        "This PC is joined to a domain or managed by an organization: removed policy entries may be "
        "written again.");
}

QString ExtensionsText::itemText(const leftovers::Location& location) {
    if (std::holds_alternative<leftovers::RegistryKeyLocation>(location)) {
        return tr("Delete the registry key %1").arg(LeftoverText::locationText(location));
    }
    if (std::holds_alternative<leftovers::RegistryValueLocation>(location)) {
        return tr("Delete the registry value %1").arg(LeftoverText::locationText(location));
    }
    if (const auto* edit = std::get_if<leftovers::ValueEditLocation>(&location)) {
        const QString value =
            LeftoverText::locationText(leftovers::RegistryValueLocation{edit->key, edit->value_name});
        const QString before = valueText(edit->expected);
        if (!edit->desired) {
            return before.isEmpty() ? tr("Delete the registry value %1").arg(value)
                                    : tr("Delete the registry value %1 (“%2”)").arg(value, before);
        }
        const QString after = valueText(edit->desired);
        return tr("Change the registry value %1 from “%2” to “%3”").arg(value, before, after);
    }
    return LeftoverText::locationText(location);
}

QString ExtensionsText::issueText(tools::ExtensionPlanIssueCode code) {
    using tools::ExtensionPlanIssueCode;
    switch (code) {
        case ExtensionPlanIssueCode::UnknownId:
            return tr("No longer listed; refresh with F5");
        case ExtensionPlanIssueCode::NotRemovable:
            return tr("Nothing outside the browser installs it: remove it on the browser's extensions page");
        case ExtensionPlanIssueCode::ManagedByPoliciesJson:
            return tr("Firefox's policies.json installs it; Avel doesn't edit that file");
        case ExtensionPlanIssueCode::NotSupported:
            return tr("A policy names it together with other extensions in one key");
        case ExtensionPlanIssueCode::Protected:
            return tr("A protected registry location holds what installs it");
    }
    return {};
}

QString ExtensionsText::diagnosticText(const tools::ExtensionDiagnostic& diagnostic) {
    using tools::ExtensionDiagnosticCode;
    const QString subject = toQString(diagnostic.subject);
    QString text;
    switch (diagnostic.code) {
        case ExtensionDiagnosticCode::ProfileUnreadable:
            text = tr("A browser profile's preferences couldn't be read: %1").arg(subject);
            break;
        case ExtensionDiagnosticCode::LocalStateUnreadable:
            text =
                tr("A browser's list of profiles couldn't be read, so its profiles were found by folder: %1")
                    .arg(subject);
            break;
        case ExtensionDiagnosticCode::AddonsUnreadable:
            text = tr("A Firefox profile's list of add-ons couldn't be read: %1").arg(subject);
            break;
        case ExtensionDiagnosticCode::ProfilesIniUnreadable:
            text = tr("Firefox's list of profiles couldn't be read: %1").arg(subject);
            break;
        case ExtensionDiagnosticCode::PolicyUnreadable:
            text = tr("An ExtensionSettings policy isn't valid JSON, so the browser ignores it too: %1")
                       .arg(subject);
            break;
        case ExtensionDiagnosticCode::PoliciesJsonUnreadable:
            text = tr("Firefox's policies.json couldn't be read: %1").arg(subject);
            break;
    }
    if (diagnostic.error != 0) {
        text += u' ' + ProgramText::systemErrorText(static_cast<std::uint32_t>(diagnostic.error));
    }
    return text;
}

QString ExtensionsText::installTimeText(std::optional<std::int64_t> milliseconds) {
    if (!milliseconds || *milliseconds <= 0) {
        return {};
    }
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(*milliseconds).toLocalTime(),
                              QLocale::ShortFormat);
}

QString ExtensionsText::planDescription(int count, const QString& name) {
    if (count == 1 && !name.isEmpty()) {
        return tr("Remove browser extension %1").arg(name);
    }
    return tr("Remove %n browser extension(s)", nullptr, count);
}

}  // namespace avel::app
