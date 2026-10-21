#include "core/uninstall/uninstall_plan.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

#include "core/inventory/uninstall_entry.h"
#include "core/platform/command_line.h"
#include "core/platform/text.h"

namespace avel::uninstall {

namespace {

using inventory::InstallerType;
using inventory::Program;
using inventory::Scope;
using inventory::Source;

constexpr wchar_t kMsiexec[] = L"msiexec.exe";

bool is_msiexec(std::wstring_view executable) {
    const std::wstring file = std::filesystem::path(executable).filename().wstring();
    return platform::iequals(file, L"msiexec.exe") || platform::iequals(file, L"msiexec");
}

// "steam://uninstall/440" and the like: a URL scheme rather than a path ("C:" is a drive).
bool is_url(std::wstring_view command) {
    command = platform::trim(command);
    const size_t colon = command.find(L':');
    if (colon == std::wstring_view::npos || colon < 2) {
        return false;
    }
    for (size_t i = 0; i < colon; ++i) {
        const wchar_t c = command[i];
        if (!std::iswalnum(c) && c != L'-' && c != L'+' && c != L'.') {
            return false;
        }
    }
    return true;
}

bool has_token(std::wstring_view arguments, std::wstring_view token) {
    size_t start = 0;
    while (start < arguments.size()) {
        const size_t end = std::min(arguments.find(L' ', start), arguments.size());
        if (platform::iequals(arguments.substr(start, end - start), token)) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

// Appends `extra` to the arguments. NSIS requires "_?=<dir>" to stay last, so switches go before it.
void add_switches(std::wstring& arguments, std::wstring_view extra) {
    const size_t in_place = arguments.find(L"_?=");
    if (in_place != std::wstring::npos) {
        arguments.insert(in_place, std::wstring(extra) + L' ');
        return;
    }
    if (!arguments.empty()) {
        arguments += L' ';
    }
    arguments += extra;
}

std::wstring msi_product_code_of(const Program& program) {
    if (!program.msi_product_code.empty()) {
        return program.msi_product_code;
    }
    if (program.installer_type == InstallerType::Msi) {
        return inventory::extract_guid(program.uninstall_string).value_or(std::wstring());
    }
    return {};
}

}  // namespace

std::wstring UninstallPlan::command_line() const {
    if (executable.empty()) {
        return {};
    }
    std::wstring line =
        executable.find_first_of(L" \t") == std::wstring::npos ? executable : L'"' + executable + L'"';
    if (!arguments.empty()) {
        line += L' ';
        line += arguments;
    }
    return line;
}

UninstallPlan make_uninstall_plan(const Program& program, Mode mode, const PlanContext& context) {
    UninstallPlan plan;
    plan.installer_type = program.installer_type;
    const auto fail = [&plan](PlanError error) {
        plan.error = error;
        return plan;
    };

    if (program.no_remove) {
        return fail(PlanError::RemovalBlocked);
    }
    if (program.scope == Scope::User && !program.user_sid.empty() &&
        !platform::iequals(program.user_sid, context.current_user_sid)) {
        return fail(PlanError::OtherUser);
    }

    // Packaged apps are removed per user, which needs no elevation.
    if (program.source == Source::Appx) {
        if (program.package_full_name.empty()) {
            return fail(PlanError::NoUninstaller);
        }
        plan.method = Method::Package;
        plan.quiet = true;  // The package manager shows no UI either way
        plan.installer_type = InstallerType::Appx;
        plan.package_full_name = program.package_full_name;
        return plan;
    }
    if (program.scope == Scope::Machine && !context.elevated) {
        return fail(PlanError::ElevationRequired);
    }

    const auto split = [&context](std::wstring_view text) {
        return context.file_exists ? platform::split_command_line(text, context.file_exists)
                                   : platform::split_command_line(text);
    };
    const platform::CommandLine command =
        program.uninstall_string.empty() ? platform::CommandLine{} : split(program.uninstall_string);

    // Windows Installer products: always through msiexec with the product code.
    const std::wstring product_code = msi_product_code_of(program);
    if (!product_code.empty() &&
        (program.source == Source::Msi || program.windows_installer || is_msiexec(command.executable))) {
        if (program.msi_package_missing) {
            return fail(PlanError::MsiPackageMissing);
        }
        plan.method = Method::Msi;
        plan.installer_type = InstallerType::Msi;
        plan.executable = kMsiexec;
        plan.arguments = L"/x " + product_code;
        if (mode == Mode::Quiet) {
            plan.arguments += L" /qn /norestart";
            plan.quiet = true;
        }
        return plan;
    }

    if (program.uninstall_string.empty()) {
        return fail(PlanError::NoUninstaller);
    }
    if (is_url(program.uninstall_string)) {
        return fail(PlanError::UnsupportedCommand);
    }
    if (program.uninstaller_missing) {
        return fail(PlanError::UninstallerMissing);
    }

    InstallerType type = program.installer_type;
    if (type == InstallerType::Unknown && context.detect_installer) {
        type = context.detect_installer(command.executable);
    }
    plan.installer_type = type;

    // The vendor's own silent command beats a guessed switch.
    if (mode == Mode::Quiet && !program.quiet_uninstall_string.empty() &&
        !is_url(program.quiet_uninstall_string)) {
        const platform::CommandLine quiet = split(program.quiet_uninstall_string);
        plan.method = is_msiexec(quiet.executable) ? Method::Msi : Method::Command;
        plan.executable = quiet.executable;
        plan.arguments = quiet.arguments;
        plan.quiet = true;
        return plan;
    }

    plan.method = is_msiexec(command.executable) ? Method::Msi : Method::Command;
    plan.executable = command.executable;
    plan.arguments = command.arguments;
    if (mode != Mode::Quiet) {
        return plan;
    }
    plan.quiet = true;
    switch (type) {
        case InstallerType::InnoSetup:
            if (!has_token(plan.arguments, L"/VERYSILENT")) {
                add_switches(plan.arguments, L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART");
            }
            break;
        case InstallerType::Nsis:
            if (!has_token(plan.arguments, L"/S")) {
                add_switches(plan.arguments, L"/S");
            }
            break;
        case InstallerType::WixBurn:
            if (!has_token(plan.arguments, L"/quiet")) {
                add_switches(plan.arguments, L"/quiet /norestart");
            }
            break;
        case InstallerType::Squirrel:
            if (!has_token(plan.arguments, L"-s")) {
                add_switches(plan.arguments, L"-s");
            }
            break;
        case InstallerType::SdbInst:
            if (!has_token(plan.arguments, L"-q")) {
                add_switches(plan.arguments, L"-q");
            }
            break;
        default:
            plan.quiet = false;  // No known silent switch: run the uninstaller interactively
            break;
    }
    return plan;
}

std::string_view to_string(Mode mode) {
    switch (mode) {
        case Mode::Interactive:
            return "interactive";
        case Mode::Quiet:
            return "quiet";
    }
    return "unknown";
}

std::string_view to_string(Method method) {
    switch (method) {
        case Method::Command:
            return "command";
        case Method::Msi:
            return "msi";
        case Method::Package:
            return "package";
    }
    return "unknown";
}

std::string_view to_string(PlanError error) {
    switch (error) {
        case PlanError::None:
            return "none";
        case PlanError::NoUninstaller:
            return "no-uninstaller";
        case PlanError::RemovalBlocked:
            return "removal-blocked";
        case PlanError::OtherUser:
            return "other-user";
        case PlanError::ElevationRequired:
            return "elevation-required";
        case PlanError::UnsupportedCommand:
            return "unsupported-command";
        case PlanError::UninstallerMissing:
            return "uninstaller-missing";
        case PlanError::MsiPackageMissing:
            return "msi-package-missing";
    }
    return "unknown";
}

}  // namespace avel::uninstall
