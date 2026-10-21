#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "core/inventory/program.h"

namespace avel::uninstall {

enum class Mode : std::uint8_t {
    Interactive,  // Run the program's own uninstaller with its UI
    Quiet,        // Run it silently where a silent switch is known; interactive otherwise
};

enum class Method : std::uint8_t {
    Command,  // Run an uninstaller executable
    Msi,      // Run msiexec; its exit codes are well defined
    Package,  // Remove a packaged app through the package manager
};

// Why a program can't be uninstalled the normal way. Avel Force (force.h) removes programs whose
// uninstaller is missing or broken.
enum class PlanError : std::uint8_t {
    None,
    NoUninstaller,       // No uninstall command and nothing else to go on
    RemovalBlocked,      // NoRemove=1, or a system package
    OtherUser,           // Installed for another account: its uninstaller would act on our profile
    ElevationRequired,   // Installed for all users, but this process isn't elevated
    UnsupportedCommand,  // The command isn't an executable (e.g. a steam:// link)
    UninstallerMissing,  // The uninstaller's file is gone (Program::uninstaller_missing)
    MsiPackageMissing,   // Windows Installer can't find the product's package (Program::msi_package_missing)
};

struct UninstallPlan {
    PlanError error = PlanError::None;
    Method method = Method::Command;
    // Effective mode: false when quiet mode was asked for but no silent switch is known.
    bool quiet = false;
    // The technology the plan is based on (may be refined from the inventory's guess).
    inventory::InstallerType installer_type = inventory::InstallerType::Unknown;
    std::wstring executable;  // Command and Msi: unquoted; may be a bare name like "msiexec.exe"
    std::wstring arguments;
    std::wstring package_full_name;  // Package

    bool possible() const { return error == PlanError::None; }
    // The full command line as it will be run (Command and Msi), for display and logs.
    std::wstring command_line() const;
};

struct PlanContext {
    std::wstring current_user_sid;
    bool elevated = true;
    // Refines InstallerType::Unknown by looking at the uninstaller executable (see
    // installer_detection.h). Optional; tests leave it empty.
    std::function<inventory::InstallerType(const std::wstring& executable)> detect_installer;
    // Probes the file system when splitting unquoted command lines. Optional; defaults to the
    // real file system.
    std::function<bool(const std::wstring& path)> file_exists;
};

// Decides how to uninstall `program`. Pure apart from the context's callbacks.
//
// Silent switches by technology (the uninstaller's own QuietUninstallString wins when present):
//   MSI           msiexec.exe /x {ProductCode} /qn /norestart
//   Inno Setup    /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
//   NSIS          /S
//   WiX Burn      /quiet /norestart (the bundle's command already has /uninstall)
//   Squirrel      -s (with --uninstall)
//   SdbInst       -q
// Interactive MSI removal uses /x rather than the registered /I, which would open the
// repair/remove maintenance dialog instead of removing.
UninstallPlan make_uninstall_plan(const inventory::Program& program, Mode mode, const PlanContext& context);

std::string_view to_string(Mode mode);
std::string_view to_string(Method method);
std::string_view to_string(PlanError error);

}  // namespace avel::uninstall
