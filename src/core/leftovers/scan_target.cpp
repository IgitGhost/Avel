#include "core/leftovers/scan_target.h"

#include <windows.h>

#include "core/inventory/uninstall_entry.h"
#include "core/platform/command_line.h"
#include "core/platform/paths.h"

namespace avel::leftovers {

namespace {

constexpr wchar_t kUninstallKey[] = LR"(Microsoft\Windows\CurrentVersion\Uninstall\)";

std::optional<platform::RegistryAddress> uninstall_key(const inventory::Program& program) {
    if (!program.registry) {
        return std::nullopt;
    }
    using inventory::RegistryView;
    const auto& location = *program.registry;
    switch (location.view) {
        case RegistryView::Machine64:
            return platform::RegistryAddress{platform::Hive::Machine, L"",
                                             LR"(SOFTWARE\)" + std::wstring(kUninstallKey) + location.key_name};
        case RegistryView::Machine32:
            return platform::RegistryAddress{platform::Hive::Machine, L"",
                                             LR"(SOFTWARE\WOW6432Node\)" + std::wstring(kUninstallKey) +
                                                 location.key_name};
        case RegistryView::User:
        case RegistryView::User32:
            if (program.user_sid.empty()) {
                return std::nullopt;
            }
            return platform::RegistryAddress{
                platform::Hive::User, program.user_sid,
                (location.view == RegistryView::User ? LR"(Software\)" : LR"(Software\WOW6432Node\)") +
                    std::wstring(kUninstallKey) + location.key_name};
    }
    return std::nullopt;
}

std::wstring windows_directory() {
    wchar_t buffer[MAX_PATH] = {};
    const UINT length = ::GetWindowsDirectoryW(buffer, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(buffer, length) : std::wstring();
}

}  // namespace

ScanTarget make_scan_target(const inventory::Program& program) {
    ScanTarget target;
    target.program_id = program.id;
    target.display_name = program.display_name;
    target.names = make_product_names(program.display_name, program.publisher);
    target.scope = program.scope;
    target.user_sid = program.user_sid;
    target.store_app = program.source == inventory::Source::Appx;
    target.package_full_name = program.package_full_name;
    target.uninstall_key = uninstall_key(program);
    target.msi_product_code = program.msi_product_code;
    if (target.msi_product_code.empty() && program.windows_installer && program.registry) {
        // An MSI product's Uninstall key is named after its product code.
        target.msi_product_code = inventory::extract_guid(program.registry->key_name).value_or(L"");
    }

    if (!program.install_location.empty() && platform::is_absolute_path(program.install_location)) {
        target.install_location = platform::long_path(program.install_location);
    }
    const std::wstring windows = windows_directory();
    const auto outside_windows = [&](const std::wstring& path) {
        return windows.empty() || !platform::path_is_under(path, windows);
    };
    if (!program.uninstall_string.empty()) {
        const auto command = platform::split_command_line(program.uninstall_string);
        if (platform::is_absolute_path(command.executable)) {
            const std::wstring file = platform::long_path(command.executable);
            // msiexec, rundll32 and friends say nothing about where the program lives.
            if (outside_windows(file)) {
                target.uninstaller = file;
                target.uninstaller_folder = platform::parent_path(file);
            }
        }
    }

    // Many entries don't record their folder. The uninstaller's or the icon's folder stands in
    // when it is named after the program, as BCU's info adders do.
    if (target.install_location.empty()) {
        std::wstring icon = program.display_icon;
        if (const size_t comma = icon.find_last_of(L','); comma != std::wstring::npos &&
                                                          icon.find_first_of(L"\\/", comma) == std::wstring::npos) {
            icon.resize(comma);  // "path,index"
        }
        std::wstring icon_folder;
        if (platform::is_absolute_path(icon)) {
            icon_folder = platform::parent_path(platform::long_path(icon));
        }
        for (const auto* candidate : {&target.uninstaller_folder, &icon_folder}) {
            if (!candidate->empty() && outside_windows(*candidate) &&
                match_product_name(target.names, platform::last_path_component(*candidate)) >= 0) {
                target.install_location = *candidate;
                break;
            }
        }
    }
    return target;
}

}  // namespace avel::leftovers
