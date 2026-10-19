#include "core/inventory/program.h"

namespace avel::inventory {

bool is_listed_in_settings(const Program& program) {
    if (program.display_name.empty() || program.system_component || program.is_update ||
        program.is_framework || program.msi_product_unregistered) {
        return false;
    }
    // Windows Installer products without an Uninstall key are internal components of other
    // programs; Settings never shows them.
    return program.source != Source::Msi;
}

std::string_view to_string(Source source) {
    switch (source) {
        case Source::Registry:
            return "registry";
        case Source::Msi:
            return "msi";
        case Source::Appx:
            return "appx";
    }
    return "unknown";
}

std::string_view to_string(Scope scope) {
    switch (scope) {
        case Scope::Machine:
            return "machine";
        case Scope::User:
            return "user";
    }
    return "unknown";
}

std::string_view to_string(Architecture architecture) {
    switch (architecture) {
        case Architecture::Unknown:
            return "unknown";
        case Architecture::X86:
            return "x86";
        case Architecture::X64:
            return "x64";
        case Architecture::Arm:
            return "arm";
        case Architecture::Arm64:
            return "arm64";
        case Architecture::Neutral:
            return "neutral";
    }
    return "unknown";
}

std::string_view to_string(InstallerType type) {
    switch (type) {
        case InstallerType::Unknown:
            return "unknown";
        case InstallerType::Msi:
            return "msi";
        case InstallerType::Nsis:
            return "nsis";
        case InstallerType::InnoSetup:
            return "inno-setup";
        case InstallerType::InstallShield:
            return "installshield";
        case InstallerType::WixBurn:
            return "wix-burn";
        case InstallerType::Squirrel:
            return "squirrel";
        case InstallerType::PowerShell:
            return "powershell";
        case InstallerType::SdbInst:
            return "sdbinst";
        case InstallerType::Steam:
            return "steam";
        case InstallerType::Appx:
            return "appx";
    }
    return "unknown";
}

std::string_view to_string(RegistryView view) {
    switch (view) {
        case RegistryView::Machine64:
            return "machine-64";
        case RegistryView::Machine32:
            return "machine-32";
        case RegistryView::User:
            return "user";
        case RegistryView::User32:
            return "user-32";
    }
    return "unknown";
}

}  // namespace avel::inventory
