#include "program_text.h"

#include <QLocale>

#include <windows.h>

using namespace Qt::StringLiterals;

namespace avel::app {

using inventory::Architecture;
using inventory::DiagnosticCode;
using inventory::InstallerType;

QString formatSize(std::uint64_t bytes) {
    return QLocale().formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeTraditionalFormat);
}

QString ProgramText::typeLabel(const inventory::Program& program) {
    if (program.source == inventory::Source::Appx) {
        return tr("Store app");
    }
    // Technology names are proper names and stay untranslated.
    switch (program.installer_type) {
        case InstallerType::Msi:
            return u"MSI"_s;
        case InstallerType::Nsis:
            return u"NSIS"_s;
        case InstallerType::InnoSetup:
            return u"Inno Setup"_s;
        case InstallerType::InstallShield:
            return u"InstallShield"_s;
        case InstallerType::WixBurn:
            return u"WiX Burn"_s;
        case InstallerType::Squirrel:
            return u"Squirrel"_s;
        case InstallerType::PowerShell:
            return u"PowerShell"_s;
        case InstallerType::SdbInst:
            return u"SdbInst"_s;
        case InstallerType::Steam:
            return u"Steam"_s;
        case InstallerType::Appx:
            return tr("Store app");
        case InstallerType::Unknown:
            break;
    }
    return tr("Desktop app");
}

QString ProgramText::architectureLabel(Architecture architecture) {
    switch (architecture) {
        case Architecture::X86:
            return tr("32-bit");
        case Architecture::X64:
            return tr("64-bit");
        case Architecture::Arm:
            return u"ARM"_s;
        case Architecture::Arm64:
            return u"ARM64"_s;
        case Architecture::Neutral:
            return tr("Any");
        case Architecture::Unknown:
            break;
    }
    return tr("Unknown");
}

QString ProgramText::scopeLabel(const inventory::Program& program) {
    if (program.scope == inventory::Scope::Machine) {
        return tr("All users");
    }
    if (!program.user_name.empty()) {
        return toQString(program.user_name);
    }
    return program.user_sid.empty() ? tr("Current user") : toQString(program.user_sid);
}

QString ProgramText::diagnosticText(const inventory::Diagnostic& diagnostic) {
    const QString subject = toQString(diagnostic.subject);
    switch (diagnostic.code) {
        case DiagnosticCode::UninstallKeyUnreadable:
            return tr("Could not read the registry key %1 (error %2).").arg(subject).arg(diagnostic.error);
        case DiagnosticCode::UserHiveLoadFailed:
            return tr("Could not load the registry of user %1 (error %2); programs installed only for that "
                      "user are "
                      "not shown.")
                .arg(subject)
                .arg(diagnostic.error);
        case DiagnosticCode::MsiAllUsersDenied:
            return tr(
                "Windows Installer products of other users are not shown: administrator rights are "
                "required.");
        case DiagnosticCode::MsiEnumerationFailed:
            return tr("Windows Installer products could not be listed (error %1).").arg(diagnostic.error);
        case DiagnosticCode::AppxEnumerationFailed:
            return tr("Store apps could not be listed (error %1).").arg(diagnostic.error);
    }
    return {};
}

QString ProgramText::planText(const uninstall::UninstallPlan& plan, uninstall::Mode requested) {
    using uninstall::Method;
    if (!plan.possible()) {
        return planErrorText(plan.error);
    }
    if (plan.method == Method::Package) {
        return tr("Windows package, removed for the current user");
    }
    if (plan.method == Method::Msi) {
        return plan.quiet ? tr("Silently, with Windows Installer") : tr("Windows Installer dialog");
    }
    if (plan.quiet) {
        inventory::Program technology;
        technology.installer_type = plan.installer_type;
        return plan.installer_type == InstallerType::Unknown
                   ? tr("Silently, with the program's own silent command")
                   : tr("Silently (%1)").arg(typeLabel(technology));
    }
    return requested == uninstall::Mode::Quiet ? tr("The program's uninstaller window (no silent mode known)")
                                               : tr("The program's uninstaller window");
}

QString ProgramText::planErrorText(uninstall::PlanError error) {
    using uninstall::PlanError;
    switch (error) {
        case PlanError::None:
            return {};
        case PlanError::NoUninstaller:
            return tr("Can't uninstall: no uninstaller is registered");
        case PlanError::RemovalBlocked:
            return tr("Can't uninstall: its installer doesn't allow it");
        case PlanError::OtherUser:
            return tr("Can't uninstall: installed for another user, who has to uninstall it");
        case PlanError::ElevationRequired:
            return tr("Can't uninstall: administrator rights are required");
        case PlanError::UnsupportedCommand:
            return tr("Can't uninstall: its uninstall command isn't a program Avel can run");
    }
    return {};
}

QString ProgramText::outcomeText(const uninstall::UninstallResult& result) {
    using uninstall::Outcome;
    switch (result.outcome) {
        case Outcome::Removed:
            return result.install_location_remains ? tr("Removed; files remain in its folder")
                                                   : tr("Removed");
        case Outcome::RebootRequired:
            return tr("Removed; restart Windows to finish");
        case Outcome::StillInstalled:
            return result.exit_code ? tr("Still installed (uninstaller exit code %1)").arg(*result.exit_code)
                                    : tr("Still installed");
        case Outcome::Cancelled:
            return tr("Cancelled");
        case Outcome::Failed:
            return tr("Failed: %1").arg(systemErrorText(result.error));
        case Outcome::NotPossible:
            return planErrorText(result.plan_error);
        case Outcome::Simulated:
            return tr("Simulated; nothing was changed");
    }
    return {};
}

QString ProgramText::systemErrorText(std::uint32_t error) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    QString message =
        length > 0 ? QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).trimmed() : QString();
    ::LocalFree(buffer);
    // HRESULTs read best in hex, Win32 errors in decimal.
    const QString code = error > 0xFFFF
                             ? u"0x"_s + QString::number(error, 16).rightJustified(8, u'0').toUpper()
                             : QString::number(error);
    return message.isEmpty() ? tr("error %1").arg(code) : u"%1 (%2)"_s.arg(message, code);
}

}  // namespace avel::app
