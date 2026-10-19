#include "core/inventory/registry_source.h"

#include <shlwapi.h>

#include <array>
#include <filesystem>
#include <system_error>

#include "core/platform/registry.h"
#include "core/platform/text.h"
#include "core/platform/time.h"
#include "core/platform/user_profiles.h"

namespace avel::inventory {

namespace {

using platform::RegistryKey;

constexpr wchar_t kUninstallPath[] = LR"(Software\Microsoft\Windows\CurrentVersion\Uninstall)";
constexpr wchar_t kUninstallPath32[] = LR"(Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall)";

bool has_text(const std::vector<platform::RegistryValue>& values, std::wstring_view name) {
    const auto* value = platform::find_value(values, name);
    if (value == nullptr) {
        return false;
    }
    const auto text = value->as_string();
    return text && !platform::trim(*text).empty();
}

// Display names like "@%SystemRoot%\system32\foo.dll,-101" are resource references, which
// Settings resolves to localized text.
std::wstring load_indirect_string(std::wstring_view reference) {
    const std::wstring source(reference);
    std::array<wchar_t, 512> buffer{};
    if (FAILED(::SHLoadIndirectString(source.c_str(), buffer.data(), static_cast<UINT>(buffer.size()),
                                      nullptr))) {
        return {};
    }
    return buffer.data();
}

void scan_path(HKEY root, const wchar_t* path, REGSAM view_flag, UninstallKeyContext context,
               RegistryScanResult& result) {
    RegistryKey uninstall_key;
    const LSTATUS status = RegistryKey::open(root, path, KEY_READ | view_flag, uninstall_key);
    if (status == ERROR_FILE_NOT_FOUND) {
        return;
    }
    if (status != ERROR_SUCCESS) {
        result.diagnostics.push_back({DiagnosticCode::UninstallKeyUnreadable, context.location.path, status});
        return;
    }
    scan_uninstall_key(uninstall_key.get(), context, result);
}

void scan_user_hive(HKEY hive, const platform::UserProfile& profile, bool is_current_user,
                    RegistryScanResult& result) {
    const std::wstring root_label = L"HKU\\" + profile.sid;

    UninstallKeyContext context;
    context.scope = Scope::User;
    context.user_sid = profile.sid;
    context.user_name = profile.account_name;
    context.resolve_indirect = load_indirect_string;
    if (is_current_user) {
        context.expand = [](std::wstring_view text) { return platform::expand_environment(text); };
    } else {
        context.expand = [profile](std::wstring_view text) {
            return platform::expand_environment_for_user(text, profile);
        };
    }

    context.location.view = RegistryView::User;
    context.location.path = root_label + L"\\" + kUninstallPath;
    context.id_prefix = L"reg:" + root_label + L":";
    scan_path(hive, kUninstallPath, 0, context, result);

    context.location.view = RegistryView::User32;
    context.location.path = root_label + L"\\" + kUninstallPath32;
    context.id_prefix = L"reg:" + root_label + L"\\WOW6432Node:";
    context.architecture = Architecture::X86;
    scan_path(hive, kUninstallPath32, 0, context, result);
}

void scan_users(RegistryScanResult& result) {
    const std::wstring current_sid = platform::current_user_sid();
    for (const auto& profile : platform::enumerate_user_profiles()) {
        const bool is_current_user = platform::iequals(profile.sid, current_sid);
        if (profile.hive_loaded) {
            RegistryKey hive;
            if (RegistryKey::open(HKEY_USERS, profile.sid, KEY_READ, hive) == ERROR_SUCCESS) {
                scan_user_hive(hive.get(), profile, is_current_user, result);
            }
            continue;
        }

        // Signed-out user: load their hive privately. RegLoadAppKey keeps it invisible to other
        // processes and unloads it when the last handle closes, so the user can still sign in.
        const std::filesystem::path hive_file = std::filesystem::path(profile.profile_path) / L"NTUSER.DAT";
        std::error_code ignored;
        if (profile.profile_path.empty() || !std::filesystem::exists(hive_file, ignored)) {
            continue;
        }
        HKEY loaded = nullptr;
        const LSTATUS status = ::RegLoadAppKeyW(hive_file.c_str(), &loaded, KEY_READ, REG_PROCESS_APPKEY, 0);
        if (status != ERROR_SUCCESS) {
            result.diagnostics.push_back({DiagnosticCode::UserHiveLoadFailed, profile.sid, status});
            continue;
        }
        const wil::unique_hkey hive(loaded);
        scan_user_hive(hive.get(), profile, is_current_user, result);
    }
}

}  // namespace

void scan_uninstall_key(HKEY uninstall_key, const UninstallKeyContext& context, RegistryScanResult& result) {
    // Reopen through the wrapper (an empty subkey path yields a new handle to the same key).
    RegistryKey uninstall;
    if (RegistryKey::open(uninstall_key, L"", KEY_READ, uninstall) != ERROR_SUCCESS) {
        return;
    }
    const std::wstring base_path = context.location.path;
    for (const auto& name : uninstall.subkey_names()) {
        RegistryKey entry_key;
        const LSTATUS status = RegistryKey::open(uninstall_key, name, KEY_READ, entry_key);
        if (status != ERROR_SUCCESS) {
            result.diagnostics.push_back(
                {DiagnosticCode::UninstallKeyUnreadable, base_path + L"\\" + name, status});
            continue;
        }
        UninstallKeyContext entry_context = context;
        entry_context.location.key_name = name;
        entry_context.location.path = base_path + L"\\" + name;
        const auto values = entry_key.values();
        if (auto program = parse_uninstall_entry(entry_context, values)) {
            // Like Settings, fall back to when the key was last written if there is no InstallDate.
            // An InstallDate that doesn't parse means no date at all, as in Settings.
            if (!program->install_date && !has_text(values, L"InstallDate")) {
                if (const auto written = entry_key.last_write_time()) {
                    program->install_date = platform::local_date(*written);
                }
            }
            result.programs.push_back(std::move(*program));
        }
    }
}

RegistryScanResult scan_registry() {
    RegistryScanResult result;

    UninstallKeyContext machine;
    machine.scope = Scope::Machine;
    machine.expand = [](std::wstring_view text) { return platform::expand_environment(text); };
    machine.resolve_indirect = load_indirect_string;

    machine.location.view = RegistryView::Machine64;
    machine.location.path = std::wstring(L"HKLM\\") + kUninstallPath;
    machine.id_prefix = L"reg:HKLM64:";
    machine.architecture = Architecture::X64;
    scan_path(HKEY_LOCAL_MACHINE, kUninstallPath, KEY_WOW64_64KEY, machine, result);

    machine.location.view = RegistryView::Machine32;
    machine.location.path = std::wstring(L"HKLM\\") + kUninstallPath32;
    machine.id_prefix = L"reg:HKLM32:";
    machine.architecture = Architecture::X86;
    scan_path(HKEY_LOCAL_MACHINE, kUninstallPath, KEY_WOW64_32KEY, machine, result);

    scan_users(result);
    return result;
}

}  // namespace avel::inventory
