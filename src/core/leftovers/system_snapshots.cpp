// Snapshots of services, scheduled tasks, COM classes and firewall rules for the leftover scan.
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <array>
#include <string>

#include "core/leftovers/scanners.h"
#include "core/platform/paths.h"
#include "core/platform/registry.h"
#include "core/platform/text.h"

namespace avel::leftovers::detail {

namespace {

constexpr REGSAM kRead = KEY_READ | KEY_WOW64_64KEY;
constexpr DWORD kDriverTypes = SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER | SERVICE_ADAPTER | SERVICE_RECOGNIZER_DRIVER;

std::wstring string_value(const std::vector<platform::RegistryValue>& values, std::wstring_view name) {
    const auto* value = platform::find_value(values, name);
    if (!value) {
        return {};
    }
    const auto text = value->as_string().value_or(L"");
    return value->type == REG_EXPAND_SZ ? platform::expand_environment(text) : text;
}

// The default value of `parent\subkey` as text, expanded; empty if it has none. Reads one value
// without enumerating the key (there are tens of thousands of COM classes).
std::wstring default_string(HKEY parent, const std::wstring& subkey) {
    wchar_t buffer[MAX_PATH * 2];
    DWORD type = 0;
    DWORD size = sizeof(buffer);
    if (::RegGetValueW(parent, subkey.c_str(), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type,
                       buffer, &size) != ERROR_SUCCESS) {
        return {};  // Missing, or longer than any path worth reading
    }
    const std::wstring text(buffer);
    return type == REG_EXPAND_SZ ? platform::expand_environment(text) : text;
}

// Collects the tasks of `folder` and its subfolders, skipping Windows' own (\Microsoft).
void collect_tasks(ITaskFolder* folder, std::vector<ScheduledTask>& tasks) {
    wil::unique_bstr folder_path;
    if (SUCCEEDED(folder->get_Path(&folder_path)) && folder_path &&
        platform::istarts_with(folder_path.get(), LR"(\Microsoft)") &&
        (folder_path.get()[10] == L'\0' || folder_path.get()[10] == L'\\')) {
        return;
    }
    wil::com_ptr_nothrow<IRegisteredTaskCollection> collection;
    LONG count = 0;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, collection.put())) && SUCCEEDED(collection->get_Count(&count))) {
        for (LONG i = 1; i <= count; ++i) {
            VARIANT index{};
            index.vt = VT_I4;
            index.lVal = i;
            wil::com_ptr_nothrow<IRegisteredTask> task;
            wil::unique_bstr path;
            wil::com_ptr_nothrow<ITaskDefinition> definition;
            wil::com_ptr_nothrow<IActionCollection> actions;
            if (FAILED(collection->get_Item(index, task.put())) || FAILED(task->get_Path(&path)) ||
                FAILED(task->get_Definition(definition.put())) || FAILED(definition->get_Actions(actions.put()))) {
                continue;  // Tasks this account may not read
            }
            ScheduledTask entry;
            entry.path = path.get();
            LONG action_count = 0;
            actions->get_Count(&action_count);
            for (LONG a = 1; a <= action_count; ++a) {
                wil::com_ptr_nothrow<IAction> action;
                TASK_ACTION_TYPE type{};
                if (FAILED(actions->get_Item(a, action.put())) || FAILED(action->get_Type(&type)) ||
                    type != TASK_ACTION_EXEC) {
                    continue;
                }
                const auto exec = action.try_query<IExecAction>();
                wil::unique_bstr program;
                wil::unique_bstr arguments;
                if (!exec || FAILED(exec->get_Path(&program)) || !program) {
                    continue;
                }
                exec->get_Arguments(&arguments);
                std::wstring command(platform::trim(program.get()));
                if (command.find(L' ') != std::wstring::npos && command.front() != L'"') {
                    command = L"\"" + command + L"\"";
                }
                if (arguments && *arguments.get()) {
                    command += L" " + std::wstring(arguments.get());
                }
                entry.actions.push_back(std::move(command));
            }
            if (!entry.actions.empty()) {
                tasks.push_back(std::move(entry));
            }
        }
    }
    wil::com_ptr_nothrow<ITaskFolderCollection> folders;
    LONG folder_count = 0;
    if (SUCCEEDED(folder->GetFolders(0, folders.put())) && SUCCEEDED(folders->get_Count(&folder_count))) {
        for (LONG i = 1; i <= folder_count; ++i) {
            VARIANT index{};
            index.vt = VT_I4;
            index.lVal = i;
            wil::com_ptr_nothrow<ITaskFolder> subfolder;
            if (SUCCEEDED(folders->get_Item(index, subfolder.put()))) {
                collect_tasks(subfolder.get(), tasks);
            }
        }
    }
}

}  // namespace

void take_services(const ScanEnvironment& environment, Snapshots& snapshots) {
    platform::RegistryKey services;
    if (platform::RegistryKey::open(environment.machine, LR"(SYSTEM\CurrentControlSet\Services)", kRead, services) ==
        ERROR_SUCCESS) {
        for (const auto& name : services.subkey_names()) {
            platform::RegistryKey service;
            if (platform::RegistryKey::open(services.get(), name, kRead, service) != ERROR_SUCCESS) {
                continue;
            }
            const auto values = service.values();
            const auto* type = platform::find_value(values, L"Type");
            if (!type || (type->as_integer().value_or(0) & kDriverTypes) != 0) {
                continue;  // Drivers are Avel Force's business, never a leftover here
            }
            ServiceEntry entry{name, platform::referenced_paths(string_value(values, L"ImagePath"))};
            platform::RegistryKey parameters;
            if (platform::RegistryKey::open(service.get(), L"Parameters", kRead, parameters) == ERROR_SUCCESS) {
                const std::wstring dll = string_value(parameters.values(), L"ServiceDll");
                if (platform::is_absolute_path(dll)) {
                    entry.paths.push_back(platform::normalize_path(dll));
                }
            }
            if (!entry.paths.empty()) {
                snapshots.services.push_back(std::move(entry));
            }
        }
    }

    // Firewall rules: "v2.31|Action=Allow|...|App=C:\path\app.exe|Name=...|".
    platform::RegistryKey rules;
    if (platform::RegistryKey::open(environment.machine,
                                    LR"(SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\FirewallRules)",
                                    kRead, rules) == ERROR_SUCCESS) {
        for (const auto& value : rules.values()) {
            const auto text = value.as_string();
            if (!text) {
                continue;
            }
            const size_t start = text->find(L"|App=");
            if (start == std::wstring::npos) {
                continue;
            }
            const size_t end = text->find(L'|', start + 5);
            const std::wstring program = platform::expand_environment(
                text->substr(start + 5, end == std::wstring::npos ? std::wstring::npos : end - start - 5));
            if (platform::is_absolute_path(program)) {
                snapshots.firewall_rules.push_back({value.name, platform::normalize_path(program)});
            }
        }
    }
}

void take_tasks(const ScanEnvironment& environment, Snapshots& snapshots) {
    std::vector<ScheduledTask> tasks;
    if (environment.tasks) {
        tasks = *environment.tasks;
    } else {
        wil::com_ptr_nothrow<ITaskService> service;
        const VARIANT empty{};
        wil::com_ptr_nothrow<ITaskFolder> root;
        const auto root_path = wil::make_bstr_nothrow(L"\\");
        if (SUCCEEDED(::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(service.put()))) &&
            SUCCEEDED(service->Connect(empty, empty, empty, empty)) && root_path &&
            SUCCEEDED(service->GetFolder(root_path.get(), root.put()))) {
            collect_tasks(root.get(), tasks);
        }
    }
    for (const auto& task : tasks) {
        TaskEntry entry{task.path, {}};
        for (const auto& action : task.actions) {
            for (auto& path : platform::referenced_paths(action)) {
                entry.paths.push_back(std::move(path));
            }
        }
        if (!entry.paths.empty()) {
            snapshots.tasks.push_back(std::move(entry));
        }
    }
}

void take_com_classes(const ScanEnvironment& environment, Snapshots& snapshots) {
    struct Root {
        platform::RegistryAddress classes;  // ...\Classes
        const UserScope* user;
    };
    std::vector<Root> roots{{{platform::Hive::Machine, L"", LR"(SOFTWARE\Classes)"}, nullptr}};
    for (const auto& user : environment.users) {
        if (user.hive) {
            roots.push_back({{platform::Hive::User, user.profile.sid, LR"(Software\Classes)"}, &user});
        }
    }

    for (const auto& root : roots) {
        // Classes and their server files; 32-bit classes live under WOW6432Node in the machine's
        // classes only.
        std::vector<std::wstring> clsid_keys{root.classes.path + LR"(\CLSID)"};
        if (root.classes.hive == platform::Hive::Machine) {
            clsid_keys.push_back(root.classes.path + LR"(\WOW6432Node\CLSID)");
        }
        for (const auto& clsid_path : clsid_keys) {
            const platform::RegistryAddress address{root.classes.hive, root.classes.sid, clsid_path};
            platform::RegistryKey clsids;
            if (open_key(environment, address, clsids) != ERROR_SUCCESS) {
                continue;
            }
            for (const auto& clsid : clsids.subkey_names()) {
                if (clsid.empty() || clsid.front() != L'{') {
                    continue;
                }
                ComClass entry{{address.hive, address.sid, clsid_path + L"\\" + clsid}, clsid, {}, root.user};
                for (const auto server : {L"InprocServer32", L"LocalServer32"}) {
                    const std::wstring value = default_string(clsids.get(), clsid + L"\\" + server);
                    if (value.empty()) {
                        continue;
                    }
                    if (server == std::wstring_view(L"LocalServer32")) {
                        for (auto& path : platform::referenced_paths(value)) {
                            entry.paths.push_back(std::move(path));
                        }
                    } else if (platform::is_absolute_path(value)) {
                        entry.paths.push_back(platform::normalize_path(value));  // A DLL path, not a command
                    }
                }
                if (!entry.paths.empty()) {
                    snapshots.com_classes.push_back(std::move(entry));
                }
            }
        }

        // Shell extension handlers naming a class, by key name or default value.
        constexpr std::array<std::wstring_view, 7> kObjects = {
            L"*", L"AllFilesystemObjects", L"Directory", LR"(Directory\Background)", L"Folder", L"Drive",
            L"DesktopBackground"};
        constexpr std::array<std::wstring_view, 4> kHandlers = {L"ContextMenuHandlers", L"PropertySheetHandlers",
                                                                L"DragDropHandlers", L"CopyHookHandlers"};
        for (const auto object : kObjects) {
            for (const auto handler : kHandlers) {
                const platform::RegistryAddress address{
                    root.classes.hive, root.classes.sid,
                    root.classes.path + L"\\" + std::wstring(object) + LR"(\shellex\)" + std::wstring(handler)};
                platform::RegistryKey handlers;
                if (open_key(environment, address, handlers) != ERROR_SUCCESS) {
                    continue;
                }
                for (const auto& name : handlers.subkey_names()) {
                    platform::RegistryKey key;
                    std::wstring clsid;
                    if (platform::RegistryKey::open(handlers.get(), name, kRead, key) == ERROR_SUCCESS) {
                        clsid = string_value(key.values(), L"");
                    }
                    if (clsid.empty() || clsid.front() != L'{') {
                        clsid = name;
                    }
                    if (!clsid.empty() && clsid.front() == L'{') {
                        snapshots.class_references.push_back(
                            {RegistryKeyLocation{{address.hive, address.sid, address.path + L"\\" + name}}, clsid,
                             root.user});
                    }
                }
            }
        }
    }

    // Explorer's list of approved shell extensions: value names are class ids.
    std::vector<std::pair<platform::RegistryAddress, const UserScope*>> approved{
        {{platform::Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved)"},
         nullptr}};
    for (const auto& user : environment.users) {
        if (user.hive) {
            approved.push_back(
                {{platform::Hive::User, user.profile.sid, LR"(Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved)"},
                 &user});
        }
    }
    for (const auto& [address, user] : approved) {
        platform::RegistryKey key;
        if (open_key(environment, address, key) != ERROR_SUCCESS) {
            continue;
        }
        for (const auto& value : key.values()) {
            if (!value.name.empty() && value.name.front() == L'{') {
                snapshots.class_references.push_back({RegistryValueLocation{address, value.name}, value.name, user});
            }
        }
    }
}

}  // namespace avel::leftovers::detail
