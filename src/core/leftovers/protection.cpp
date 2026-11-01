// The folder and key blacklists are taken from Bulk Crap Uninstaller, Copyright (c) 2017 Marcin
// Szeniak, Apache License 2.0 (UninstallTools/Junk/Finders/Drive/CommonDriveJunkScanner.cs,
// Registry/SoftwareRegKeyScanner.cs).
#include "core/leftovers/protection.h"

#include <windows.h>
#include <winsvc.h>

#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>

#include "core/leftovers/startup_approved.h"
#include "core/leftovers/value_edits.h"
#include "core/platform/known_folders.h"
#include "core/platform/paths.h"
#include "core/platform/registry.h"
#include "core/platform/signature.h"
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

constexpr std::wstring_view kTaskCache =
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache)";
constexpr std::wstring_view kTaskCache32 =
    LR"(SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache)";

// HKLM's system hives: never touched through raw registry edits.
constexpr std::array<std::wstring_view, 8> kSystemHives = {L"SYSTEM",   L"SECURITY",   L"SAM",     L"HARDWARE",
                                                           L"BCD00000000", L"COMPONENTS", L"DRIVERS", L"SCHEMA"};

// Keys Windows relies on, below SOFTWARE; neither they nor their ancestors may be removed. The
// WOW6432Node variants are derived from these. A "*" component stands for any one key, so
// "Classes\Installer\*" keeps Windows Installer's Products, Features, UpgradeCodes, ... keys while
// a single product's key below them (Products\<packed code>) may go.
constexpr std::array<std::wstring_view, 20> kCoreSoftwareKeys = {
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
    LR"(Microsoft\Windows\CurrentVersion\Installer\*)",
    LR"(Microsoft\Windows\CurrentVersion\Installer\UserData\*\*)",
    LR"(Microsoft\Windows\CurrentVersion\Installer\Managed\*\Installer\*)",
    LR"(Classes\Installer\*)",
    LR"(Microsoft\Installer\*)",  // A user's own Windows Installer registration
    LR"(Microsoft\Windows\CurrentVersion\Shell Extensions\Approved)",
    LR"(Microsoft\Windows\CurrentVersion\Policies)",
    LR"(Microsoft\Windows NT\CurrentVersion)",
    LR"(Policies)",
    LR"(RegisteredApplications)"};

// What Windows keeps for registered packages, below SOFTWARE (or a user's Software, whose Classes
// is their classes hive): the deployment service writes it when a package is registered and
// removes it with the package. A package goes through PackageLocation; this never goes as raw keys
// or values, even when an install trace saw it created (an installer that registers a sparse
// package, as Notepad++ does for its context menu, has Windows write it while the trace runs).
// Nothing inside them may go, nor may their ancestors.
constexpr std::array<std::wstring_view, 10> kPackageKeys = {
    LR"(Classes\PackagedCom)",            // Packaged COM servers, and the index of their classes
    LR"(Classes\ActivatableClasses)",     // Packaged Windows Runtime classes
    LR"(Classes\Extensions\ContractId)",  // Packaged apps' extensions (launch, protocols, ...)
    LR"(Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel)",
    LR"(Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppContainer)",
    LR"(Microsoft\Windows\CurrentVersion\AppModel)",
    LR"(Microsoft\Windows\CurrentVersion\Appx)",
    LR"(Microsoft\Windows\CurrentVersion\AppHost\IndexedDB)",
    LR"(Microsoft\Windows\CurrentVersion\BackgroundAccessApplications)",
    LR"(Microsoft\SecurityManager\CapAuthz)",  // Packages' capabilities
};

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

// `key` is a key that `pattern` describes, or an ancestor of one: its components match the
// pattern's first ones, where "*" matches any component.
bool key_path_leads_to(std::wstring_view key, std::wstring_view pattern) {
    while (true) {
        const size_t key_end = std::min(key.find(L'\\'), key.size());
        const size_t pattern_end = std::min(pattern.find(L'\\'), pattern.size());
        if (pattern.empty()) {
            return key.empty();  // The key is deeper than anything the pattern describes
        }
        const std::wstring_view part = pattern.substr(0, pattern_end);
        if (part != L"*" && !platform::iequals(part, key.substr(0, key_end))) {
            return false;
        }
        if (key_end == key.size()) {
            return true;
        }
        key.remove_prefix(key_end + 1);
        pattern.remove_prefix(std::min(pattern_end + 1, pattern.size()));
    }
}

// The keys below SOFTWARE, with their WOW6432Node variants.
template <size_t N>
std::vector<std::wstring> software_keys(const std::array<std::wstring_view, N>& list) {
    std::vector<std::wstring> keys;
    for (const auto key : list) {
        keys.push_back(LR"(SOFTWARE\)" + std::wstring(key));
        keys.push_back(LR"(SOFTWARE\WOW6432Node\)" + std::wstring(key));
        if (platform::istarts_with(key, LR"(Classes\)")) {
            keys.push_back(LR"(SOFTWARE\Classes\WOW6432Node\)" + std::wstring(key.substr(8)));
        }
    }
    return keys;
}

std::vector<std::wstring> core_keys() { return software_keys(kCoreSoftwareKeys); }

// "AppX" and 32 letters and digits: the ProgID Windows makes for a package's file type or protocol.
bool is_package_prog_id(std::wstring_view name) {
    return name.size() == 36 && platform::istarts_with(name, L"AppX") &&
           std::all_of(name.begin() + 4, name.end(), [](wchar_t c) {
               return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
           });
}

// The path below a hive's classes (SOFTWARE\Classes, and its WOW6432Node views), if it is there.
std::optional<std::wstring_view> below_classes(std::wstring_view path) {
    for (const std::wstring_view classes : {std::wstring_view(LR"(SOFTWARE\Classes\WOW6432Node\)"),
                                            std::wstring_view(LR"(SOFTWARE\WOW6432Node\Classes\)"),
                                            std::wstring_view(LR"(SOFTWARE\Classes\)")}) {
        if (path.size() > classes.size() && platform::istarts_with(path, classes)) {
            return path.substr(classes.size());
        }
    }
    return std::nullopt;
}

// A registered package's own key or value (kPackageKeys, and its ProgIDs: Classes\AppX…, and the
// values that name one, in a file type's OpenWithProgids). `value_name` is null for a key.
bool is_package_registration(std::wstring_view path, const std::wstring* value_name) {
    static const std::vector<std::wstring> kKeys = software_keys(kPackageKeys);
    for (const auto& key : kKeys) {
        if (key_path_is_under(path, key) || (!value_name && key_path_leads_to(path, key))) {
            return true;
        }
    }
    const auto classes = below_classes(path);
    if (!classes) {
        return false;
    }
    return is_package_prog_id(first_component(*classes)) || (value_name && is_package_prog_id(*value_name));
}

ProtectionReason check_key_path(platform::Hive hive, std::wstring_view path, bool is_value) {
    static const std::vector<std::wstring> kCoreKeys = core_keys();
    if (hive == platform::Hive::Machine && in_list(kSystemHives, first_component(path))) {
        return ProtectionReason::SystemRegistry;
    }
    if (key_path_is_under(path, LR"(SOFTWARE\Avel)") || key_path_is_under(path, LR"(SOFTWARE\WOW6432Node\Avel)")) {
        return ProtectionReason::AvelData;
    }
    // The Task Scheduler's own records of its tasks: a task or task folder goes through the Task
    // Scheduler (TaskLocation), never as raw keys or values.
    if (hive == platform::Hive::Machine &&
        (key_path_is_under(path, kTaskCache) || key_path_is_under(path, kTaskCache32))) {
        return ProtectionReason::CoreRegistryKey;
    }
    if (is_value) {
        return ProtectionReason::None;  // A value in a core key (a Run entry, say) is fine on its own
    }
    if (path.find(L'\\') == std::wstring_view::npos) {
        return ProtectionReason::CoreRegistryKey;  // A hive's top-level key: SOFTWARE, Environment, ...
    }
    for (const auto& core : kCoreKeys) {
        if (key_path_leads_to(path, core)) {
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
            const auto values = account.values();
            if (const auto* folder = platform::find_value(values, L"UserFolder")) {
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

// The file a service runs: its image, or for one svchost.exe hosts, its ServiceDll (under
// Parameters, or the service's own key).
std::wstring service_image(const std::wstring& name, const wchar_t* binary_path) {
    const auto paths = platform::referenced_paths(binary_path ? binary_path : L"");
    const std::wstring image = paths.empty() ? std::wstring() : paths.front();
    if (!platform::iequals(platform::last_path_component(image), L"svchost.exe")) {
        return image;
    }
    const std::wstring key_path = LR"(SYSTEM\CurrentControlSet\Services\)" + name;
    for (const std::wstring& path : {key_path + L"\\Parameters", key_path}) {
        platform::RegistryKey key;
        if (platform::RegistryKey::open(HKEY_LOCAL_MACHINE, path, KEY_READ, key) != ERROR_SUCCESS) {
            continue;
        }
        const auto values = key.values();
        const auto* dll = platform::find_value(values, L"ServiceDll");
        const auto text = dll ? dll->as_string() : std::nullopt;
        const auto dll_paths = text ? platform::referenced_paths(*text) : std::vector<std::wstring>();
        if (!dll_paths.empty()) {
            return dll_paths.front();
        }
    }
    return image;  // svchost.exe itself
}

}  // namespace

ServiceFacts live_service_facts(const std::wstring& name) {
    ServiceFacts facts;
    wil::unique_schandle manager(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        return facts;
    }
    wil::unique_schandle service(::OpenServiceW(manager.get(), name.c_str(), SERVICE_QUERY_CONFIG));
    if (!service) {
        if (::GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
            facts.status = ServiceFacts::Status::Missing;
        }
        return facts;
    }
    DWORD needed = 0;
    ::QueryServiceConfigW(service.get(), nullptr, 0, &needed);
    std::vector<std::byte> buffer(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (needed == 0 || !::QueryServiceConfigW(service.get(), config, needed, &needed)) {
        return facts;
    }
    facts.status = ServiceFacts::Status::Found;
    facts.driver = (config->dwServiceType & SERVICE_DRIVER) != 0;
    if (!facts.driver) {
        // Whatever the signature's state: a Microsoft file that fails its check is still Windows'.
        facts.microsoft = platform::verify_signature(service_image(name, config->lpBinaryPathName)).microsoft;
    }
    return facts;
}

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
        case ProtectionReason::PackageRegistration:
            return "package-registration";
        case ProtectionReason::NotEditable:
            return "not-editable";
        case ProtectionReason::WindowsService:
            return "windows-service";
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
                         // Packages' machine-wide data, which goes with the package
                         {machine.program_data + LR"(\Packages)", ProtectionReason::SystemArea},
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
        const ProtectionReason reason = check_key_path(key->key.hive, key->key.path, false);
        return reason == ProtectionReason::None && is_package_registration(key->key.path, nullptr)
                   ? ProtectionReason::PackageRegistration
                   : reason;
    }
    if (const auto* value = std::get_if<RegistryValueLocation>(&location)) {
        const ProtectionReason reason = check_key_path(value->key.hive, value->key.path, true);
        return reason == ProtectionReason::None && is_package_registration(value->key.path, &value->value_name)
                   ? ProtectionReason::PackageRegistration
                   : reason;
    }
    if (const auto* task = std::get_if<TaskLocation>(&location)) {
        if (task->folder &&
            (task->path.empty() || task->path == L"\\" || platform::iequals(task->path, L"\\Microsoft"))) {
            return ProtectionReason::WindowsTask;  // The root folder, or Windows' own
        }
        return platform::istarts_with(task->path, LR"(\Microsoft\)") ? ProtectionReason::WindowsTask
                                                                     : ProtectionReason::None;
    }
    if (const auto* edit = std::get_if<ValueEditLocation>(&location)) {
        // The only allow-list in the floor, and only for edits: the environment and SharedDLLs.
        const auto spot = value_spot(edit->key);
        if (!spot) {
            return ProtectionReason::NotEditable;
        }
        if (*spot == ValueSpot::SharedDlls) {
            return edit->kind == ValueEditKind::DecrementSharedDll || edit->kind == ValueEditKind::DeleteValue
                       ? ProtectionReason::None
                       : ProtectionReason::NotEditable;
        }
        return may_edit_variable(*spot, edit->value_name, edit->kind) ? ProtectionReason::None
                                                                      : ProtectionReason::NotEditable;
    }
    if (const auto* approval = std::get_if<StartupApprovedLocation>(&location)) {
        // The other allow-list: Explorer's three StartupApproved keys, written only as Task Manager
        // writes them (or the value deleted).
        if (!approval_list(approval->key)) {
            return ProtectionReason::NotEditable;
        }
        return !approval->desired || is_approval_value(*approval->desired) ? ProtectionReason::None
                                                                           : ProtectionReason::NotEditable;
    }
    if (const auto* task = std::get_if<TaskStateLocation>(&location)) {
        return platform::istarts_with(task->path, LR"(\Microsoft\)") ? ProtectionReason::WindowsTask
                                                                     : ProtectionReason::None;
    }
    if (const auto* service = std::get_if<ServiceStartLocation>(&location)) {
        const std::uint32_t type = service->after.start_type;
        if (type != SERVICE_AUTO_START && type != SERVICE_DEMAND_START && type != SERVICE_DISABLED) {
            return ProtectionReason::WindowsService;  // Never boot or system start: those are drivers'
        }
        const ServiceFacts facts =
            data_.services ? data_.services(service->name) : live_service_facts(service->name);
        switch (facts.status) {
            case ServiceFacts::Status::Missing:
                return ProtectionReason::None;  // Nothing to protect; the executor reports it missing
            case ServiceFacts::Status::Unreadable:
                return ProtectionReason::WindowsService;  // Unknown: refused
            case ServiceFacts::Status::Found:
                break;
        }
        return facts.driver || facts.microsoft ? ProtectionReason::WindowsService : ProtectionReason::None;
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
    // Values, services, tasks, packages, firewall rules, value edits and state changes: no known
    // folder or shared name to guard.
    return ProtectionReason::None;
}

}  // namespace avel::leftovers
