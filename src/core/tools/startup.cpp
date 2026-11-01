#include "core/tools/startup.h"

#include <windows.h>
#include <objbase.h>

#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>

#include "core/force/force.h"
#include "core/platform/command_line.h"
#include "core/platform/hash.h"
#include "core/platform/paths.h"
#include "core/platform/registry.h"
#include "core/platform/shortcuts.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"

namespace avel::tools {

namespace {

using leftovers::UserScope;
using platform::Hive;
using platform::RegistryAddress;

constexpr REGSAM kRead = KEY_READ | KEY_WOW64_64KEY;
constexpr std::wstring_view kCurrentVersion = LR"(\Microsoft\Windows\CurrentVersion\)";
constexpr std::wstring_view kApproved = LR"(\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\)";

// Windows programs that run whatever their arguments say (a command, a script, a URL). An entry
// that runs one of them isn't Windows' own just because the program is Microsoft's.
constexpr std::array<std::wstring_view, 13> kCommandHosts = {
    L"cmd.exe",      L"conhost.exe",    L"cscript.exe", L"explorer.exe", L"mshta.exe",
    L"msiexec.exe",  L"powershell.exe", L"pwsh.exe",    L"reg.exe",      L"regsvr32.exe",
    L"rundll32.exe", L"schtasks.exe",   L"wscript.exe"};

// The file an entry runs.
struct Target {
    std::wstring file;
    bool command_host = false;  // `file` is one of kCommandHosts
};

// What the list keeps about an entry until it is complete.
struct Pending {
    bool command_host = false;
    bool avel_disabled = false;  // A service named in StartupEnvironment::avel_disabled_services
};

struct Collector {
    explicit Collector(const StartupEnvironment& environment)
        : environment(environment),
          windows(platform::normalize_path(environment.scan.machine_folders.windows)) {}

    const StartupEnvironment& environment;
    const std::wstring windows;
    std::vector<StartupEntry> entries;
    std::vector<Pending> pending;      // One per entry
    std::set<std::wstring> seen_keys;  // Kernel names of the keys read, lowercase
    std::map<std::wstring, std::vector<platform::RawValue>> approvals;  // By the key's lowercase display path
    std::vector<StartupDiagnostic> diagnostics;
};

std::wstring software(const UserScope* user) {
    return user ? L"Software" : L"SOFTWARE";  // A user's hive spells it "Software"; the case is only cosmetic
}

RegistryAddress address_of(const UserScope* user, std::wstring path) {
    return user ? RegistryAddress{Hive::User, user->profile.sid, std::move(path)}
                : RegistryAddress{Hive::Machine, L"", std::move(path)};
}

HKEY root_of(const StartupEnvironment& environment, const UserScope* user) {
    return user ? user->hive : environment.scan.machine;
}

std::wstring expand_for(const UserScope* user, std::wstring_view text) {
    return user ? platform::expand_environment_for_user(text, user->profile)
                : platform::expand_environment(text);
}

// The machine, then each user.
std::vector<const UserScope*> scopes(const StartupEnvironment& environment) {
    std::vector<const UserScope*> list{nullptr};
    for (const auto& user : environment.scan.users) {
        list.push_back(&user);
    }
    return list;
}

// As platform::missing_command_file probes: only "not found" on a fixed disk counts, not "access
// denied", and network shares and removable media aren't probed. A folder isn't missing.
bool known_missing(const std::wstring& path) {
    if (path.size() < 3 || path[1] != L':' || ::GetDriveTypeW(path.substr(0, 3).c_str()) != DRIVE_FIXED) {
        return false;
    }
    if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    const DWORD error = ::GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

Target file_target(std::wstring_view path) {
    path = platform::trim(path);
    return {platform::is_absolute_path(path) ? platform::long_path(path) : std::wstring()};
}

// The file an expanded command line runs: the script or DLL of a Windows host, else the program.
Target resolve_command(std::wstring_view command, const std::wstring& windows) {
    std::wstring text(platform::trim(command));
    // The forms service image paths use.
    if (platform::istarts_with(text, LR"(\??\)")) {
        text.erase(0, 4);
    } else if (!windows.empty() && platform::istarts_with(text, LR"(\SystemRoot\)")) {
        text = windows + text.substr(11);  // Keeps the separator after "\SystemRoot"
    } else if (!windows.empty() && (platform::istarts_with(text, LR"(System32\)") ||
                                    platform::istarts_with(text, LR"(SysWOW64\)"))) {
        text = windows + L"\\" + text;
    }
    const platform::CommandLine parts = platform::split_command_line(text);
    if (parts.executable.empty()) {
        return {};
    }
    if (const std::wstring hosted = platform::hosted_file(parts); platform::is_absolute_path(hosted)) {
        return file_target(hosted);
    }
    std::wstring program = parts.executable;
    if (program.find_first_of(L"\\/") == std::wstring::npos && !windows.empty()) {
        // A bare name ("ctfmon.exe", "rundll32") is found in the system folder.
        if (program.find(L'.') == std::wstring::npos) {
            program += L".exe";
        }
        program = windows + LR"(\System32\)" + program;
    }
    Target target = file_target(program);
    const auto name = platform::last_path_component(target.file);
    target.command_host =
        !target.file.empty() && std::any_of(kCommandHosts.begin(), kCommandHosts.end(),
                                            [&](auto host) { return platform::iequals(host, name); });
    return target;
}

// RunOnceEx's "DllFile|Function|Arguments", or "||Command".
Target run_once_ex_target(const std::wstring& text, const std::wstring& windows) {
    const std::size_t bar = text.find(L'|');
    if (bar == std::wstring::npos) {
        return resolve_command(text, windows);
    }
    if (bar == 0 && text.size() > 1 && text[1] == L'|') {
        return resolve_command(std::wstring_view(text).substr(2), windows);
    }
    return file_target(std::wstring_view(text).substr(0, bar));
}

StartupEntry make_entry(StartupKind kind, const UserScope* user, std::wstring name) {
    StartupEntry entry;
    entry.kind = kind;
    if (user) {
        entry.user_sid = user->profile.sid;
        entry.user_name = user->profile.account_name;
    }
    entry.name = std::move(name);
    return entry;
}

void add(Collector& collector, StartupEntry entry, const Target& target, bool avel_disabled = false) {
    entry.target = target.file;
    entry.target_missing = !entry.target.empty() && known_missing(entry.target);
    collector.entries.push_back(std::move(entry));
    collector.pending.push_back({target.command_host, avel_disabled});
}

// Each key is read once: a key the 32- and 64-bit views share has two paths.
bool first_time(Collector& collector, const platform::RegistryKey& key) {
    const std::wstring name = key.kernel_name();
    return name.empty() || collector.seen_keys.insert(platform::to_lower(name)).second;
}

// The values of a StartupApproved key, exactly as stored, read once.
const std::vector<platform::RawValue>& approved_values(Collector& collector, const UserScope* user,
                                                       const RegistryAddress& key) {
    const auto [found, inserted] =
        collector.approvals.try_emplace(platform::to_lower(platform::display_path(key)));
    if (inserted) {
        const HKEY root = root_of(collector.environment, user);
        platform::RegistryKey opened;
        if (root && platform::RegistryKey::open(root, key.path, kRead, opened) == ERROR_SUCCESS) {
            platform::read_raw_values(opened.get(), found->second);
        }
    }
    return found->second;
}

// Task Manager's mark: the first byte's low bit set means disabled (03, 01), clear enabled (02, 06).
bool approval_disabled(const std::optional<platform::RawValue>& value) {
    return value && !value->data.empty() && (std::to_integer<unsigned>(value->data.front()) & 1u) != 0;
}

// Pairs the entry with its value in `list` (Run, Run32 or StartupFolder) of the same hive. Comes
// after the entry's own delete location: the value, if there is one, goes with it.
void pair_approval(Collector& collector, StartupEntry& entry, const UserScope* user, std::wstring_view list,
                   const std::wstring& value_name) {
    StartupApproval approval{address_of(user, software(user) + std::wstring(kApproved) + std::wstring(list)),
                             value_name, std::nullopt};
    for (const auto& value : approved_values(collector, user, approval.key)) {
        if (platform::iequals(value.name, value_name)) {
            approval.value = value;
            break;
        }
    }
    if (approval_disabled(approval.value)) {
        entry.state = StartupState::Disabled;
    }
    if (approval.value) {
        entry.delete_locations.push_back(leftovers::RegistryValueLocation{approval.key, value_name});
    }
    entry.approval = std::move(approval);
}

void collect_run(Collector& collector, StartupKind kind) {
    for (const UserScope* user : scopes(collector.environment)) {
        const HKEY root = root_of(collector.environment, user);
        if (!root) {
            continue;
        }
        for (const bool wow64 : {false, true}) {
            const std::wstring path = software(user) + (wow64 ? L"\\WOW6432Node" : L"") +
                                      std::wstring(kCurrentVersion) +
                                      (kind == StartupKind::Run ? L"Run" : L"RunOnce");
            platform::RegistryKey key;
            if (platform::RegistryKey::open(root, path, kRead, key) != ERROR_SUCCESS ||
                !first_time(collector, key)) {
                continue;
            }
            const RegistryAddress key_address = address_of(user, path);
            for (const auto& value : key.values()) {
                const auto text = value.as_string();
                if ((value.type != REG_SZ && value.type != REG_EXPAND_SZ) || !text ||
                    platform::trim(*text).empty()) {
                    continue;  // Runs nothing
                }
                StartupEntry entry = make_entry(kind, user, value.name);
                entry.wow64 = wow64;
                entry.command = *text;
                entry.value_type = value.type;
                entry.delete_locations.push_back(leftovers::RegistryValueLocation{key_address, value.name});
                // The machine's 32-bit Run values are paired with Run32. A user's 32-bit Run key
                // has no list of its own (see docs/tools.md): delete only, like RunOnce.
                if (kind == StartupKind::Run && !(user && wow64)) {
                    pair_approval(collector, entry, user, wow64 ? L"Run32" : L"Run", value.name);
                }
                add(collector, std::move(entry), resolve_command(expand_for(user, *text), collector.windows));
            }
        }
    }
}

// RunOnceEx exists for the machine only: Windows doesn't read a user's.
void collect_run_once_ex(Collector& collector) {
    for (const bool wow64 : {false, true}) {
        const std::wstring path = std::wstring(L"SOFTWARE") + (wow64 ? L"\\WOW6432Node" : L"") +
                                  std::wstring(kCurrentVersion) + L"RunOnceEx";
        platform::RegistryKey key;
        if (platform::RegistryKey::open(collector.environment.scan.machine, path, kRead, key) !=
                ERROR_SUCCESS ||
            !first_time(collector, key)) {
            continue;
        }
        for (const auto& name : key.subkey_names()) {
            platform::RegistryKey subkey;
            if (platform::RegistryKey::open(key.get(), name, kRead, subkey) != ERROR_SUCCESS) {
                continue;
            }
            StartupEntry entry = make_entry(StartupKind::RunOnceEx, nullptr, name);
            entry.wow64 = wow64;
            entry.delete_locations.push_back(
                leftovers::RegistryKeyLocation{address_of(nullptr, path + L"\\" + name)});
            Target target;
            for (const auto& value : subkey.values()) {
                const auto text = value.as_string();
                // The default value is the section's title.
                if (value.name.empty() || !text || platform::trim(*text).empty()) {
                    continue;
                }
                entry.command = *text;
                target = run_once_ex_target(platform::expand_environment(*text), collector.windows);
                break;
            }
            add(collector, std::move(entry), target);
        }
    }
}

void collect_folder(Collector& collector, const std::wstring& folder, const UserScope* user) {
    if (folder.empty()) {
        return;
    }
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = (folder.size() > 248 ? L"\\\\?\\" + folder : folder) + L"\\*";
    wil::unique_hfind find(::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                              nullptr, FIND_FIRST_EX_LARGE_FETCH));
    if (!find) {
        return;  // Missing, or another user's that this account may not read
    }
    do {
        // Windows runs the files in the folder itself, not in its subfolders.
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !platform::iequals(data.cFileName, L"desktop.ini")) {
            names.emplace_back(data.cFileName);
        }
    } while (::FindNextFileW(find.get(), &data));

    for (auto& name : names) {
        const std::wstring path = platform::normalize_path(folder + L"\\" + name);
        StartupEntry entry = make_entry(StartupKind::StartupFolder, user, name);
        entry.delete_locations.push_back(leftovers::FileLocation{path, false});
        pair_approval(collector, entry, user, L"StartupFolder", name);
        Target target;
        if (platform::iends_with(name, L".lnk")) {
            // An advertised shortcut names a Windows Installer product, not a file: no target.
            if (const auto shortcut = platform::resolve_shortcut(path);
                shortcut && !shortcut->target.empty()) {
                entry.command = platform::exec_command(shortcut->target, shortcut->arguments);
                target = resolve_command(platform::exec_command(expand_for(user, shortcut->target),
                                                                expand_for(user, shortcut->arguments)),
                                         collector.windows);
            }
        } else {
            entry.command = platform::exec_command(path, L"");
            target = file_target(path);
        }
        add(collector, std::move(entry), target);
    }
}

void collect_tasks(Collector& collector) {
    std::vector<platform::ScheduledTask> live;
    if (!collector.environment.tasks) {
        if (const HRESULT result = platform::scheduled_tasks(true, live); FAILED(result)) {
            collector.diagnostics.push_back(
                {StartupDiagnosticCode::TasksUnavailable, static_cast<std::int32_t>(result)});
        }
    }
    const auto& tasks = collector.environment.tasks ? *collector.environment.tasks : live;
    for (const auto& task : tasks) {
        const bool at_start =
            std::any_of(task.triggers.begin(), task.triggers.end(), [](platform::TaskTrigger trigger) {
                return trigger == platform::TaskTrigger::Logon || trigger == platform::TaskTrigger::Boot;
            });
        if (!at_start || platform::is_windows_task(task.path)) {
            continue;
        }
        StartupEntry entry = make_entry(StartupKind::Task, nullptr, task.path);
        entry.task_enabled = task.enabled;
        entry.state = task.enabled ? StartupState::Enabled : StartupState::Disabled;
        entry.delete_locations.push_back(leftovers::TaskLocation{task.path, false});
        Target target;
        // A task that runs only COM handlers has no command line, and no target.
        if (!task.actions.empty()) {
            entry.command = task.actions.front();
            target = resolve_command(platform::expand_environment(entry.command), collector.windows);
        }
        add(collector, std::move(entry), target);
    }
}

void collect_services(Collector& collector) {
    std::vector<platform::ServiceInfo> live;
    if (!collector.environment.services) {
        if (const DWORD error = platform::win32_services(live); error != ERROR_SUCCESS) {
            collector.diagnostics.push_back(
                {StartupDiagnosticCode::ServicesUnavailable, static_cast<std::int32_t>(error)});
        }
    }
    const auto& services = collector.environment.services ? *collector.environment.services : live;
    const auto& by_avel = collector.environment.avel_disabled_services;
    for (const auto& service : services) {
        if (platform::is_driver_type(service.type) || (service.type & SERVICE_WIN32) == 0) {
            continue;
        }
        const bool avel = std::any_of(by_avel.begin(), by_avel.end(), [&](const std::wstring& name) {
            return platform::iequals(name, service.name);
        });
        const bool automatic = service.start_type == SERVICE_AUTO_START;
        if (!automatic && !avel) {
            continue;
        }
        StartupEntry entry = make_entry(StartupKind::Service, nullptr, service.name);
        entry.display_name = service.display_name;
        entry.command = service.image_path;
        entry.service_start = service.start_type;
        entry.service_delayed = service.delayed;
        entry.state = automatic ? StartupState::Enabled : StartupState::Disabled;
        entry.disabled_by_avel = avel && !automatic;
        entry.delete_locations.push_back(leftovers::ServiceLocation{service.name, false});
        // A service svchost hosts is its DLL.
        const Target target =
            service.service_dll.empty()
                ? resolve_command(platform::expand_environment(service.image_path), collector.windows)
                : file_target(platform::expand_environment(service.service_dll));
        add(collector, std::move(entry), target, avel);
    }
}

// Each distinct target's version resource and signature, on a few threads: a signature check can
// take tens of milliseconds, and an auto-start service list has a hundred files.
void read_files(Collector& collector) {
    std::vector<std::wstring> files;
    std::unordered_map<std::wstring, std::size_t> index;
    std::vector<std::size_t> file_of(collector.entries.size(), SIZE_MAX);
    for (std::size_t i = 0; i < collector.entries.size(); ++i) {
        const auto& entry = collector.entries[i];
        if (entry.target.empty() || entry.target_missing) {
            continue;
        }
        const auto [found, inserted] = index.try_emplace(platform::to_lower(entry.target), files.size());
        if (inserted) {
            files.push_back(entry.target);
        }
        file_of[i] = found->second;
    }
    std::vector<platform::FileProduct> products(files.size());
    std::vector<StartupSigner> signers(files.size());
    const auto& signer = collector.environment.signer;
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (std::size_t i = next++; i < files.size(); i = next++) {
            products[i] = platform::file_product(files[i]);
            if (signer) {
                signers[i] = signer(files[i]);
            }
        }
        if (SUCCEEDED(com)) {
            ::CoUninitialize();
        }
    };
    const std::size_t worker_count =
        std::min<std::size_t>(files.size(), std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
    std::vector<std::thread> workers;
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers.emplace_back(work);
    }
    for (auto& worker : workers) {
        worker.join();
    }
    for (std::size_t i = 0; i < collector.entries.size(); ++i) {
        if (file_of[i] != SIZE_MAX) {
            collector.entries[i].product = products[file_of[i]];
            collector.entries[i].signer = signers[file_of[i]];
        }
    }
}

std::vector<StartupEntry> finish(Collector& collector) {
    const force::OwnerIndex owners(collector.environment.scan, collector.environment.programs);
    std::vector<StartupEntry> entries;
    for (std::size_t i = 0; i < collector.entries.size(); ++i) {
        auto& entry = collector.entries[i];
        const Pending& pending = collector.pending[i];
        // Microsoft's own services aren't listed, unless Avel disabled one.
        if (entry.kind == StartupKind::Service && entry.signer.microsoft && !pending.avel_disabled) {
            continue;
        }
        if (entry.signer.microsoft && !pending.command_host && !collector.windows.empty() &&
            platform::path_is_under(entry.target, collector.windows)) {
            entry.protection = StartupProtection::WindowsOwn;
        }
        if (!entry.target.empty()) {
            if (const auto* program = owners.program_of(entry.target)) {
                entry.program_id = program->id;
            }
        }
        entry.id = startup_id(entry.delete_locations.front());
        entries.push_back(std::move(entry));
    }
    return entries;
}

}  // namespace

StartupList list_startup(const StartupEnvironment& environment) {
    // The caller may already have COM (in either apartment); only balance what this call starts.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto uninitialize = wil::scope_exit([&] {
        if (SUCCEEDED(com)) {
            ::CoUninitialize();
        }
    });

    Collector collector(environment);
    collect_run(collector, StartupKind::Run);
    collect_run(collector, StartupKind::RunOnce);
    collect_run_once_ex(collector);
    collect_folder(collector, environment.scan.machine_folders.common_startup, nullptr);
    for (const auto& user : environment.scan.users) {
        collect_folder(collector, user.folders.startup, &user);
    }
    collect_tasks(collector);
    collect_services(collector);
    read_files(collector);

    StartupList list;
    list.entries = finish(collector);
    list.diagnostics = std::move(collector.diagnostics);
    return list;
}

std::wstring startup_id(const leftovers::Location& location) {
    const std::string identity = platform::to_utf8(leftovers::identity(location));
    return std::format(L"{:010x}", platform::fnv1a(identity.data(), identity.size()) >> 24);
}

std::string_view to_string(StartupKind kind) {
    switch (kind) {
        case StartupKind::Run:
            return "run";
        case StartupKind::RunOnce:
            return "run-once";
        case StartupKind::RunOnceEx:
            return "run-once-ex";
        case StartupKind::StartupFolder:
            return "startup-folder";
        case StartupKind::Task:
            return "task";
        case StartupKind::Service:
            return "service";
    }
    return "unknown";
}

std::string_view to_string(StartupState state) {
    switch (state) {
        case StartupState::Enabled:
            return "enabled";
        case StartupState::Disabled:
            return "disabled";
    }
    return "unknown";
}

std::string_view to_string(StartupProtection protection) {
    switch (protection) {
        case StartupProtection::None:
            return "none";
        case StartupProtection::WindowsOwn:
            return "windows-own";
    }
    return "unknown";
}

std::string_view to_string(StartupDiagnosticCode code) {
    switch (code) {
        case StartupDiagnosticCode::TasksUnavailable:
            return "tasks-unavailable";
        case StartupDiagnosticCode::ServicesUnavailable:
            return "services-unavailable";
    }
    return "unknown";
}

}  // namespace avel::tools
