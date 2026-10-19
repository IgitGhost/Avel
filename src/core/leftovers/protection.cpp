// The folder and key blacklists are taken from Bulk Crap Uninstaller, Copyright (c) 2017 Marcin
// Szeniak, Apache License 2.0 (UninstallTools/Junk/Finders/Drive/CommonDriveJunkScanner.cs,
// Registry/SoftwareRegKeyScanner.cs).
#include "core/leftovers/protection.h"

#include <windows.h>

#include <algorithm>
#include <array>

#include "core/platform/known_folders.h"
#include "core/platform/paths.h"
#include "core/platform/registry.h"
#include "core/platform/text.h"
#include "core/platform/user_hives.h"
#include "core/platform/user_profiles.h"

namespace avel::leftovers {

namespace {

// BCU's DirectoryNameBlacklist, plus "Package Cache" (WiX Burn's shared cache).
constexpr std::array<std::wstring_view, 25> kFolderBlacklist = {
    L"Microsoft", L"Microsoft Games", L"Temp", L"Programs", L"Common", L"Common Files", L"Clients", L"Downloads",
    L"Desktop", L"Internet Explorer", L"Windows", L"Windows NT", L"Windows Photo Viewer", L"Windows Mail",
    L"Windows Defender", L"Windows Media Player", L"Uninstall Information", L"Reference Assemblies",
    L"InstallShield Installation Information", L"Installer", L"winsxs", L"WindowsApps", L"DirectX",
    L"DirectXRedist", L"Package Cache"};

// BCU SoftwareRegKeyScanner's key blacklist.
constexpr std::array<std::wstring_view, 6> kKeyBlacklist = {L"Microsoft", L"Wow6432Node", L"Windows",
                                                            L"Classes",   L"Clients",     L"RegisteredApplications"};

// BCU's QuestionableDirectoryNames.
constexpr std::array<std::wstring_view, 6> kQuestionableNames = {L"install", L"settings",      L"config",
                                                                 L"configuration", L"users", L"data"};

// Folders at the top of a volume that belong to Windows.
constexpr std::array<std::wstring_view, 10> kVolumeSystemFolders = {
    L"System Volume Information", L"$Recycle.Bin", L"Recovery", L"Config.Msi", L"$WinREAgent",
    L"$Windows.~BT",              L"$Windows.~WS", L"$SysReset", L"Boot",      L"EFI"};

// HKLM's system hives: never touched through raw registry edits.
constexpr std::array<std::wstring_view, 8> kSystemHives = {L"SYSTEM",   L"SECURITY",   L"SAM",     L"HARDWARE",
                                                           L"BCD00000000", L"COMPONENTS", L"DRIVERS", L"SCHEMA"};

// Keys Windows relies on, below SOFTWARE; neither they nor their ancestors may be removed. The
// WOW6432Node variants are derived from these.
constexpr std::array<std::wstring_view, 16> kCoreSoftwareKeys = {
    LR"(Classes\CLSID)",
    LR"(Classes\Interface)",
    LR"(Classes\TypeLib)",
    LR"(Classes\AppID)",
    LR"(Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache)",
    LR"(Microsoft\Windows\CurrentVersion\Run)",
    LR"(Microsoft\Windows\CurrentVersion\RunOnce)",
    LR"(Microsoft\Windows\CurrentVersion\Uninstall)",
    LR"(Microsoft\Windows\CurrentVersion\App Paths)",
    LR"(Microsoft\Windows\CurrentVersion\SharedDLLs)",
    LR"(Microsoft\Windows\CurrentVersion\Installer\Folders)",
    LR"(Microsoft\Windows\CurrentVersion\Shell Extensions\Approved)",
    LR"(Microsoft\Windows\CurrentVersion\Policies)",
    LR"(Microsoft\Windows NT\CurrentVersion)",
    LR"(Policies)",
    LR"(RegisteredApplications)"};

template <size_t N>
bool in_list(const std::array<std::wstring_view, N>& list, std::wstring_view name) {
    return std::any_of(list.begin(), list.end(), [&](auto item) { return platform::iequals(item, name); });
}

std::wstring_view first_component(std::wstring_view path) {
    return path.substr(0, path.find(L'\\'));
}

// `key` is `ancestor` or below it, comparing registry paths literally.
bool key_path_is_under(std::wstring_view key, std::wstring_view ancestor) {
    if (!platform::istarts_with(key, ancestor)) {
        return false;
    }
    return key.size() == ancestor.size() || key[ancestor.size()] == L'\\';
}

std::vector<std::wstring> core_keys() {
    std::vector<std::wstring> keys;
    for (const auto key : kCoreSoftwareKeys) {
        keys.push_back(LR"(SOFTWARE\)" + std::wstring(key));
        keys.push_back(LR"(SOFTWARE\WOW6432Node\)" + std::wstring(key));
        if (platform::istarts_with(key, LR"(Classes\)")) {
            keys.push_back(LR"(SOFTWARE\Classes\WOW6432Node\)" + std::wstring(key.substr(8)));
        }
    }
    return keys;
}

ProtectionReason check_key_path(platform::Hive hive, std::wstring_view path, bool is_value) {
    static const std::vector<std::wstring> kCoreKeys = core_keys();
    if (hive == platform::Hive::Machine && in_list(kSystemHives, first_component(path))) {
        return ProtectionReason::SystemRegistry;
    }
    if (key_path_is_under(path, LR"(SOFTWARE\Avel)") || key_path_is_under(path, LR"(SOFTWARE\WOW6432Node\Avel)")) {
        return ProtectionReason::AvelData;
    }
    if (is_value) {
        return ProtectionReason::None;  // A value in a core key (a Run entry, say) is fine on its own
    }
    if (path.find(L'\\') == std::wstring_view::npos) {
        return ProtectionReason::CoreRegistryKey;  // A hive's top-level key: SOFTWARE, Environment, ...
    }
    for (const auto& core : kCoreKeys) {
        if (key_path_is_under(core, path)) {
            return ProtectionReason::CoreRegistryKey;
        }
    }
    return ProtectionReason::None;
}

void add_onedrive_folders(HKEY hive, std::vector<std::pair<std::wstring, ProtectionReason>>& subtrees) {
    platform::RegistryKey accounts;
    if (platform::RegistryKey::open(hive, LR"(Software\Microsoft\OneDrive\Accounts)", KEY_READ, accounts) ==
        ERROR_SUCCESS) {
        for (const auto& name : accounts.subkey_names()) {
            platform::RegistryKey account;
            if (platform::RegistryKey::open(accounts.get(), name, KEY_READ, account) != ERROR_SUCCESS) {
                continue;
            }
            if (const auto* folder = platform::find_value(account.values(), L"UserFolder")) {
                if (const auto path = folder->as_string(); path && platform::is_absolute_path(*path)) {
                    subtrees.emplace_back(platform::normalize_path(*path), ProtectionReason::CloudFolder);
                }
            }
        }
    }
    platform::RegistryKey environment;
    if (platform::RegistryKey::open(hive, L"Environment", KEY_READ, environment) == ERROR_SUCCESS) {
        const auto values = environment.values();
        for (const auto name : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"}) {
            if (const auto* value = platform::find_value(values, name)) {
                if (const auto path = value->as_string(); path && platform::is_absolute_path(*path)) {
                    subtrees.emplace_back(platform::normalize_path(*path), ProtectionReason::CloudFolder);
                }
            }
        }
    }
}

std::wstring module_directory() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return length > 0 && length < MAX_PATH ? platform::parent_path(buffer) : std::wstring();
}

}  // namespace

std::string_view to_string(ProtectionReason reason) {
    switch (reason) {
        case ProtectionReason::None:
            return "none";
        case ProtectionReason::InvalidPath:
            return "invalid-path";
        case ProtectionReason::DriveRoot:
            return "drive-root";
        case ProtectionReason::WindowsFolder:
            return "windows-folder";
        case ProtectionReason::ProgramFiles:
            return "program-files";
        case ProtectionReason::ProgramData:
            return "program-data";
        case ProtectionReason::UserProfiles:
            return "user-profiles";
        case ProtectionReason::SystemArea:
            return "system-area";
        case ProtectionReason::CloudFolder:
            return "cloud-folder";
        case ProtectionReason::AvelData:
            return "avel-data";
        case ProtectionReason::KnownFolder:
            return "known-folder";
        case ProtectionReason::BlacklistedName:
            return "blacklisted-name";
        case ProtectionReason::CoreRegistryKey:
            return "core-registry-key";
        case ProtectionReason::SystemRegistry:
            return "system-registry";
        case ProtectionReason::WindowsTask:
            return "windows-task";
    }
    return "unknown";
}

bool is_blacklisted_folder_name(std::wstring_view name) {
    // Matching compares names without their "extension" (see name_without_extension), so
    // "Microsoft.NET" counts as "Microsoft" here too.
    const size_t dot = name.find_last_of(L'.');
    return in_list(kFolderBlacklist, name) ||
           (dot != std::wstring_view::npos && in_list(kFolderBlacklist, name.substr(0, dot)));
}

bool is_blacklisted_key_name(std::wstring_view name) {
    return in_list(kKeyBlacklist, name);
}

bool is_questionable_folder_name(std::wstring_view name) {
    return in_list(kQuestionableNames, name);
}

HardFloor::HardFloor(Data data) : data_(std::move(data)) {
    data_.windows = platform::normalize_path(data_.windows);
    for (auto* list : {&data_.keep, &data_.subtrees}) {
        for (auto& [path, reason] : *list) {
            path = platform::normalize_path(path);
        }
        std::erase_if(*list, [](const auto& item) { return item.first.empty(); });
    }
}

const HardFloor& HardFloor::live() {
    static const HardFloor floor = [] {
        const platform::MachineFolders machine = platform::machine_folders();
        Data data;
        data.windows = machine.windows;
        data.keep = {{machine.program_files, ProtectionReason::ProgramFiles},
                     {machine.program_files_x86, ProtectionReason::ProgramFiles},
                     {machine.program_data, ProtectionReason::ProgramData},
                     {machine.users, ProtectionReason::UserProfiles},
                     {machine.common_programs, ProtectionReason::KnownFolder},
                     {machine.common_desktop, ProtectionReason::KnownFolder}};
        data.subtrees = {{machine.program_files + LR"(\WindowsApps)", ProtectionReason::SystemArea},
                         {machine.program_data + LR"(\Package Cache)", ProtectionReason::SystemArea},
                         {machine.program_data + LR"(\Avel)", ProtectionReason::AvelData},
                         {module_directory(), ProtectionReason::AvelData}};
        for (const auto& profile : platform::enumerate_user_profiles()) {
            if (profile.profile_path.empty()) {
                continue;
            }
            for (const auto suffix : {L"", LR"(\AppData)", LR"(\AppData\Roaming)", LR"(\AppData\Local)",
                                      LR"(\AppData\LocalLow)"}) {
                data.keep.emplace_back(profile.profile_path + suffix, ProtectionReason::UserProfiles);
            }
            // Packaged apps' data: Windows manages it and removes it with the package.
            data.subtrees.emplace_back(profile.profile_path + LR"(\AppData\Local\Packages)", ProtectionReason::SystemArea);
            platform::UserHive hive;
            if (platform::open_user_hive(profile, KEY_READ, hive) == ERROR_SUCCESS) {
                add_onedrive_folders(hive.key.get(), data.subtrees);
            }
        }
        return HardFloor(std::move(data));
    }();
    return floor;
}

ProtectionReason HardFloor::check(const Location& location) const {
    if (const auto* file = std::get_if<FileLocation>(&location)) {
        const std::wstring path = platform::normalize_path(file->path);
        if (!platform::is_absolute_path(path)) {
            return ProtectionReason::InvalidPath;
        }
        if (platform::is_root_path(path)) {
            return ProtectionReason::DriveRoot;
        }
        if (!data_.windows.empty() && platform::path_is_under(path, data_.windows)) {
            return ProtectionReason::WindowsFolder;
        }
        // The first folder below the volume root.
        std::wstring top = path;
        for (std::wstring parent = platform::parent_path(top); !parent.empty() && !platform::is_root_path(parent);
             parent = platform::parent_path(top)) {
            top = parent;
        }
        if (in_list(kVolumeSystemFolders, platform::last_path_component(top))) {
            return ProtectionReason::SystemArea;
        }
        for (const auto& [kept, reason] : data_.keep) {
            if (platform::path_is_under(kept, path)) {
                return reason;
            }
        }
        for (const auto& [subtree, reason] : data_.subtrees) {
            if (platform::path_is_under(path, subtree)) {
                return reason;
            }
        }
        return ProtectionReason::None;
    }
    if (const auto* key = std::get_if<RegistryKeyLocation>(&location)) {
        return check_key_path(key->key.hive, key->key.path, false);
    }
    if (const auto* value = std::get_if<RegistryValueLocation>(&location)) {
        return check_key_path(value->key.hive, value->key.path, true);
    }
    if (const auto* task = std::get_if<TaskLocation>(&location)) {
        return platform::istarts_with(task->path, LR"(\Microsoft\)") ? ProtectionReason::WindowsTask
                                                                     : ProtectionReason::None;
    }
    return ProtectionReason::None;
}

ProtectedPolicy::ProtectedPolicy(std::vector<std::wstring> known_folders) : known_folders_(std::move(known_folders)) {
    for (auto& folder : known_folders_) {
        folder = platform::normalize_path(folder);
    }
    std::erase_if(known_folders_, [](const auto& folder) { return folder.empty(); });
}

ProtectionReason ProtectedPolicy::check(const Location& location) const {
    if (const auto* file = std::get_if<FileLocation>(&location)) {
        if (is_blacklisted_folder_name(platform::last_path_component(file->path))) {
            return ProtectionReason::BlacklistedName;
        }
        for (const auto& known : known_folders_) {
            if (platform::path_is_under(known, file->path)) {
                return ProtectionReason::KnownFolder;
            }
        }
        return ProtectionReason::None;
    }
    if (const auto* key = std::get_if<RegistryKeyLocation>(&location)) {
        if (is_blacklisted_key_name(platform::last_path_component(key->key.path))) {
            return ProtectionReason::BlacklistedName;
        }
    }
    return ProtectionReason::None;
}

}  // namespace avel::leftovers
