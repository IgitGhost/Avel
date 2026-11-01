#include "core/changes/system_items.h"

#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <chrono>
#include <thread>

#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "core/uninstall/registration.h"

namespace avel::changes {

namespace {

constexpr SECURITY_INFORMATION kServiceSecurity = DACL_SECURITY_INFORMATION;

ItemStatus skipped(Reason reason, DWORD code = 0) {
    return {ItemState::Skipped, reason, code};
}

Reason reason_for(DWORD error) {
    switch (error) {
        case ERROR_SERVICE_DOES_NOT_EXIST:
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return Reason::Missing;
        case ERROR_ACCESS_DENIED:
            return Reason::AccessDenied;
        case ERROR_SERVICE_MARKED_FOR_DELETE:
            return Reason::MarkedForDeletion;
        default:
            return Reason::Error;
    }
}

// The Task Scheduler's error for a task that couldn't be read or changed.
Reason task_reason(HRESULT result) {
    if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        return Reason::Missing;
    }
    return result == E_ACCESSDENIED ? Reason::AccessDenied : Reason::Error;
}

// Accounts whose services need no password to recreate.
bool is_passwordless_account(std::wstring_view account) {
    if (account.empty() || platform::iequals(account, L"LocalSystem") || platform::iequals(account, L".\\LocalSystem") ||
        platform::istarts_with(account, L"NT SERVICE\\") || platform::iends_with(account, L"$")) {
        return true;
    }
    for (const auto name : {L"NT AUTHORITY\\LocalService", L"NT AUTHORITY\\LOCAL SERVICE",
                            L"NT AUTHORITY\\NetworkService", L"NT AUTHORITY\\NETWORK SERVICE", L"NT AUTHORITY\\SYSTEM"}) {
        if (platform::iequals(account, name)) {
            return true;
        }
    }
    return false;
}

wil::unique_schandle open_service(const std::wstring& name, DWORD access, DWORD& error) {
    wil::unique_schandle manager(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        error = ::GetLastError();
        return {};
    }
    wil::unique_schandle service(::OpenServiceW(manager.get(), name.c_str(), access));
    error = service ? ERROR_SUCCESS : ::GetLastError();
    return service;
}

bool wait_for_state(SC_HANDLE service, DWORD state, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (!::QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status),
                                    &needed)) {
            return false;
        }
        if (status.dwCurrentState == state) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// The Task Scheduler, connected, for the lifetime of the object (COM included).
class TaskScheduler {
public:
    TaskScheduler() : com_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
        const VARIANT empty{};
        const auto root = wil::make_bstr_nothrow(L"\\");
        if (FAILED(::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(service_.put()))) ||
            FAILED(service_->Connect(empty, empty, empty, empty)) || !root ||
            FAILED(service_->GetFolder(root.get(), root_.put()))) {
            root_.reset();
        }
    }
    ~TaskScheduler() {
        root_.reset();
        service_.reset();
        if (SUCCEEDED(com_)) {
            ::CoUninitialize();
        }
    }
    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    ITaskService* service() const { return service_.get(); }
    ITaskFolder* root() const { return root_.get(); }

    wil::com_ptr_nothrow<IRegisteredTask> task(const std::wstring& path, HRESULT& result) const {
        wil::com_ptr_nothrow<IRegisteredTask> task;
        const auto name = wil::make_bstr_nothrow(path.c_str());
        result = root_ && name ? root_->GetTask(name.get(), task.put()) : E_FAIL;
        return task;
    }

    // The folder at `path`, created (with its parents) if `create`.
    wil::com_ptr_nothrow<ITaskFolder> folder(const std::wstring& path, bool create) const {
        wil::com_ptr_nothrow<ITaskFolder> current = root_;
        if (!current) {
            return current;
        }
        std::size_t start = 1;
        while (start < path.size()) {
            const std::size_t end = std::min(path.find(L'\\', start), path.size());
            const auto name = wil::make_bstr_nothrow(path.substr(start, end - start).c_str());
            wil::com_ptr_nothrow<ITaskFolder> next;
            if (!name) {
                return {};
            }
            if (FAILED(current->GetFolder(name.get(), next.put()))) {
                const VARIANT empty{};
                if (!create || FAILED(current->CreateFolder(name.get(), empty, next.put()))) {
                    return {};
                }
            }
            current = next;
            start = end + 1;
        }
        return current;
    }

private:
    HRESULT com_;
    wil::com_ptr_nothrow<ITaskService> service_;
    wil::com_ptr_nothrow<ITaskFolder> root_;
};

std::wstring task_parent(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L'\\');
    return slash == std::wstring::npos || slash == 0 ? L"\\" : path.substr(0, slash);
}

std::wstring task_name(const std::wstring& path) {
    return path.substr(path.find_last_of(L'\\') + 1);
}

// The root folder, and Windows' own (\Microsoft and below).
bool is_windows_task_folder(const std::wstring& path) {
    return path.empty() || path == L"\\" || platform::iequals(path, L"\\Microsoft") ||
           platform::istarts_with(path, L"\\Microsoft\\");
}

}  // namespace

std::optional<ServiceBackup> backup_service(HKEY services, const std::wstring& name, ItemStatus& failure) {
    DWORD error = 0;
    const auto service = open_service(
        name, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS | READ_CONTROL, error);
    if (!service) {
        failure = skipped(reason_for(error), error);
        return std::nullopt;
    }
    DWORD needed = 0;
    ::QueryServiceConfigW(service.get(), nullptr, 0, &needed);
    std::vector<std::byte> buffer(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (needed == 0 || !::QueryServiceConfigW(service.get(), config, needed, &needed)) {
        failure = skipped(Reason::Error, ::GetLastError());
        return std::nullopt;
    }
    if (config->dwServiceType & SERVICE_DRIVER) {
        failure = skipped(Reason::Protected);
        return std::nullopt;
    }
    ServiceBackup backup;
    backup.name = name;
    backup.type = config->dwServiceType;
    backup.start_type = config->dwStartType;
    backup.error_control = config->dwErrorControl;
    backup.binary_path = config->lpBinaryPathName ? config->lpBinaryPathName : L"";
    backup.load_order_group = config->lpLoadOrderGroup ? config->lpLoadOrderGroup : L"";
    backup.start_name = config->lpServiceStartName ? config->lpServiceStartName : L"";
    backup.display_name = config->lpDisplayName ? config->lpDisplayName : L"";
    for (const wchar_t* dependency = config->lpDependencies; dependency && *dependency;
         dependency += wcslen(dependency) + 1) {
        backup.dependencies.emplace_back(dependency);
    }
    if (!is_passwordless_account(backup.start_name)) {
        failure = skipped(Reason::NeedsPassword);
        return std::nullopt;
    }

    DWORD count = 0;
    if (!::EnumDependentServicesW(service.get(), SERVICE_STATE_ALL, nullptr, 0, &needed, &count) &&
        ::GetLastError() == ERROR_MORE_DATA) {
        failure = skipped(Reason::HasDependents);
        return std::nullopt;
    }

    SERVICE_STATUS_PROCESS status{};
    if (::QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status),
                               &needed)) {
        backup.was_running = status.dwCurrentState != SERVICE_STOPPED;
    }

    needed = 0;
    ::QueryServiceObjectSecurity(service.get(), kServiceSecurity, nullptr, 0, &needed);
    backup.security.resize(needed);
    if (needed == 0 ||
        !::QueryServiceObjectSecurity(service.get(), kServiceSecurity, backup.security.data(), needed, &needed)) {
        failure = skipped(Reason::AccessDenied, ::GetLastError());
        return std::nullopt;
    }
    if (const LSTATUS result = capture_key_readonly(services, name, {L"Security"}, backup.key);
        result != ERROR_SUCCESS) {
        failure = skipped(reason_for(result), result);
        return std::nullopt;
    }
    return backup;
}

ItemStatus remove_service(const ServiceBackup& backup) {
    DWORD error = 0;
    const auto service = open_service(backup.name, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE, error);
    if (!service) {
        return {ItemState::Failed, reason_for(error), error};
    }
    SERVICE_STATUS status{};
    if (!::ControlService(service.get(), SERVICE_CONTROL_STOP, &status)) {
        error = ::GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) {
            return {ItemState::Failed, error == ERROR_ACCESS_DENIED ? Reason::AccessDenied : Reason::InUse, error};
        }
    }
    if (!wait_for_state(service.get(), SERVICE_STOPPED, std::chrono::seconds(30))) {
        return {ItemState::Failed, Reason::InUse, ERROR_SERVICE_REQUEST_TIMEOUT};
    }
    if (!::DeleteService(service.get())) {
        error = ::GetLastError();
        return {ItemState::Failed, reason_for(error), error};
    }
    return {ItemState::Done, Reason::None, 0};
}

ItemStatus restore_service(HKEY services, const ServiceBackup& backup) {
    if (service_exists(backup.name)) {
        return {ItemState::RestoreSkipped, Reason::Exists, 0};
    }
    wil::unique_schandle manager(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
    if (!manager) {
        return {ItemState::RestoreFailed, Reason::Error, ::GetLastError()};
    }
    std::wstring dependencies;
    for (const auto& dependency : backup.dependencies) {
        dependencies += dependency + L'\0';
    }
    dependencies += L'\0';
    const auto text = [](const std::wstring& value) { return value.empty() ? nullptr : value.c_str(); };
    wil::unique_schandle service(::CreateServiceW(
        manager.get(), backup.name.c_str(), text(backup.display_name), SERVICE_ALL_ACCESS, backup.type,
        backup.start_type, backup.error_control, backup.binary_path.c_str(), text(backup.load_order_group), nullptr,
        backup.dependencies.empty() ? nullptr : dependencies.c_str(), text(backup.start_name), nullptr));
    if (!service) {
        return {ItemState::RestoreFailed, Reason::Error, ::GetLastError()};
    }
    // Description, Parameters, failure actions and the rest live in the key.
    merge_key(services, backup.key);
    // Only a security the service didn't get by default (setting it adds a Security subkey).
    DWORD needed = 0;
    ::QueryServiceObjectSecurity(service.get(), kServiceSecurity, nullptr, 0, &needed);
    std::vector<std::byte> current(needed);
    const bool same = needed && ::QueryServiceObjectSecurity(service.get(), kServiceSecurity, current.data(), needed,
                                                             &needed) &&
                      current == backup.security;
    if (!backup.security.empty() && !same) {
        ::SetServiceObjectSecurity(service.get(), kServiceSecurity, const_cast<std::byte*>(backup.security.data()));
    }
    if (backup.was_running) {
        ::StartServiceW(service.get(), 0, nullptr);  // Best effort: it was running before removal
    }
    return {ItemState::Restored, Reason::None, 0};
}

bool service_exists(const std::wstring& name) {
    DWORD error = 0;
    return static_cast<bool>(open_service(name, SERVICE_QUERY_STATUS, error)) || error == ERROR_ACCESS_DENIED;
}

std::optional<TaskBackup> backup_task(const std::wstring& path, ItemStatus& failure) {
    if (platform::istarts_with(path, L"\\Microsoft\\")) {
        failure = skipped(Reason::Protected);
        return std::nullopt;
    }
    const TaskScheduler scheduler;
    HRESULT result = S_OK;
    const auto task = scheduler.task(path, result);
    if (!task) {
        failure = result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ? skipped(Reason::Missing)
                                                                       : skipped(Reason::Error, static_cast<DWORD>(result));
        return std::nullopt;
    }
    wil::unique_bstr xml;
    wil::unique_bstr sddl;
    wil::com_ptr_nothrow<ITaskDefinition> definition;
    wil::com_ptr_nothrow<IPrincipal> principal;
    TASK_LOGON_TYPE logon = TASK_LOGON_NONE;
    if (FAILED(task->get_Xml(&xml)) || !xml || FAILED(task->GetSecurityDescriptor(DACL_SECURITY_INFORMATION, &sddl)) ||
        FAILED(task->get_Definition(definition.put())) || FAILED(definition->get_Principal(principal.put())) ||
        FAILED(principal->get_LogonType(&logon))) {
        failure = skipped(Reason::AccessDenied);
        return std::nullopt;
    }
    if (logon == TASK_LOGON_PASSWORD) {
        failure = skipped(Reason::NeedsPassword);
        return std::nullopt;
    }
    return TaskBackup{path, xml.get(), sddl ? sddl.get() : L""};
}

ItemStatus remove_task(const TaskBackup& backup) {
    const TaskScheduler scheduler;
    const auto folder = scheduler.folder(task_parent(backup.path), false);
    const auto name = wil::make_bstr_nothrow(task_name(backup.path).c_str());
    if (!folder || !name) {
        return {ItemState::Failed, Reason::Missing, 0};
    }
    const HRESULT result = folder->DeleteTask(name.get(), 0);
    if (FAILED(result)) {
        return {ItemState::Failed, result == E_ACCESSDENIED ? Reason::AccessDenied : Reason::Error,
                static_cast<std::uint32_t>(result)};
    }
    return {ItemState::Done, Reason::None, 0};
}

ItemStatus restore_task(const TaskBackup& backup) {
    if (task_exists(backup.path)) {
        return {ItemState::RestoreSkipped, Reason::Exists, 0};
    }
    const TaskScheduler scheduler;
    const auto folder = scheduler.folder(task_parent(backup.path), true);
    wil::com_ptr_nothrow<ITaskDefinition> definition;
    const auto xml = wil::make_bstr_nothrow(backup.xml.c_str());
    if (!folder || !scheduler.service() || !xml || FAILED(scheduler.service()->NewTask(0, definition.put())) ||
        FAILED(definition->put_XmlText(xml.get()))) {
        return {ItemState::RestoreFailed, Reason::Error, 0};
    }
    // Registered as the principal the definition names, which needs no password: tasks with a
    // stored password were never removed.
    wil::com_ptr_nothrow<IPrincipal> principal;
    TASK_LOGON_TYPE logon = TASK_LOGON_NONE;
    wil::unique_bstr user;
    wil::unique_bstr group;
    if (SUCCEEDED(definition->get_Principal(principal.put()))) {
        principal->get_LogonType(&logon);
        principal->get_UserId(&user);
        principal->get_GroupId(&group);
    }
    VARIANT account{};
    if (group && *group.get()) {
        account.vt = VT_BSTR;
        account.bstrVal = group.get();
        logon = TASK_LOGON_GROUP;
    } else if (user && *user.get()) {
        account.vt = VT_BSTR;
        account.bstrVal = user.get();
    }
    const auto name = wil::make_bstr_nothrow(task_name(backup.path).c_str());
    wil::com_ptr_nothrow<IRegisteredTask> registered;
    const VARIANT no_password{};
    const VARIANT no_security{};
    // Registered with the default security first: passing it here would store it in the task's
    // XML and reorder its entries.
    const HRESULT result = name ? folder->RegisterTaskDefinition(name.get(), definition.get(), TASK_CREATE, account,
                                                                 no_password, logon, no_security, registered.put())
                                : E_OUTOFMEMORY;
    if (FAILED(result)) {
        return {ItemState::RestoreFailed, Reason::Error, static_cast<std::uint32_t>(result)};
    }
    wil::unique_bstr current;
    const auto sddl = wil::make_bstr_nothrow(backup.sddl.c_str());
    if (!backup.sddl.empty() && sddl && SUCCEEDED(registered->GetSecurityDescriptor(DACL_SECURITY_INFORMATION, &current)) &&
        (!current || backup.sddl != current.get())) {
        registered->SetSecurityDescriptor(sddl.get(), 0);
    }
    return {ItemState::Restored, Reason::None, 0};
}

bool task_exists(const std::wstring& path) {
    const TaskScheduler scheduler;
    HRESULT result = S_OK;
    return static_cast<bool>(scheduler.task(path, result));
}

std::optional<TaskFolderBackup> backup_task_folder(const std::wstring& path, ItemStatus& failure) {
    if (is_windows_task_folder(path)) {
        failure = skipped(Reason::Protected);
        return std::nullopt;
    }
    const TaskScheduler scheduler;
    if (!scheduler.root()) {
        failure = skipped(Reason::Error);
        return std::nullopt;
    }
    const auto folder = scheduler.folder(path, false);
    if (!folder) {
        failure = skipped(Reason::Missing);
        return std::nullopt;
    }
    wil::com_ptr_nothrow<IRegisteredTaskCollection> tasks;
    wil::com_ptr_nothrow<ITaskFolderCollection> folders;
    LONG task_count = 0;
    LONG folder_count = 0;
    wil::unique_bstr sddl;
    if (FAILED(folder->GetTasks(TASK_ENUM_HIDDEN, tasks.put())) || FAILED(tasks->get_Count(&task_count)) ||
        FAILED(folder->GetFolders(0, folders.put())) || FAILED(folders->get_Count(&folder_count))) {
        failure = skipped(Reason::AccessDenied);
        return std::nullopt;
    }
    if (task_count > 0 || folder_count > 0) {
        failure = skipped(Reason::NotEmpty);
        return std::nullopt;
    }
    if (FAILED(folder->GetSecurityDescriptor(DACL_SECURITY_INFORMATION, &sddl))) {
        failure = skipped(Reason::AccessDenied);
        return std::nullopt;
    }
    return TaskFolderBackup{path, sddl ? sddl.get() : L""};
}

ItemStatus remove_task_folder(const TaskFolderBackup& backup) {
    const TaskScheduler scheduler;
    const auto parent = scheduler.folder(task_parent(backup.path), false);
    const auto name = wil::make_bstr_nothrow(task_name(backup.path).c_str());
    if (!parent || !name) {
        return {ItemState::Failed, Reason::Missing, 0};
    }
    const HRESULT result = parent->DeleteFolder(name.get(), 0);
    if (FAILED(result)) {
        const Reason reason = result == E_ACCESSDENIED                            ? Reason::AccessDenied
                              : result == HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY) ? Reason::NotEmpty
                                                                                  : Reason::Error;
        return {ItemState::Failed, reason, static_cast<std::uint32_t>(result)};
    }
    return {ItemState::Done, Reason::None, 0};
}

ItemStatus restore_task_folder(const TaskFolderBackup& backup) {
    if (task_folder_exists(backup.path)) {
        return {ItemState::RestoreSkipped, Reason::Exists, 0};
    }
    const TaskScheduler scheduler;
    const auto parent = scheduler.folder(task_parent(backup.path), true);
    const auto name = wil::make_bstr_nothrow(task_name(backup.path).c_str());
    wil::com_ptr_nothrow<ITaskFolder> created;
    const VARIANT default_security{};
    const HRESULT result =
        parent && name ? parent->CreateFolder(name.get(), default_security, created.put()) : E_OUTOFMEMORY;
    if (FAILED(result)) {
        return {ItemState::RestoreFailed, Reason::Error, static_cast<std::uint32_t>(result)};
    }
    // As for tasks: created with the default security, then given its own where that differs.
    wil::unique_bstr current;
    const auto sddl = wil::make_bstr_nothrow(backup.sddl.c_str());
    if (!backup.sddl.empty() && sddl &&
        SUCCEEDED(created->GetSecurityDescriptor(DACL_SECURITY_INFORMATION, &current)) &&
        (!current || backup.sddl != current.get())) {
        created->SetSecurityDescriptor(sddl.get(), 0);
    }
    return {ItemState::Restored, Reason::None, 0};
}

bool task_folder_exists(const std::wstring& path) {
    const TaskScheduler scheduler;
    return static_cast<bool>(scheduler.folder(path, false));
}

bool task_enabled(const std::wstring& path, bool& enabled, ItemStatus& failure) {
    const TaskScheduler scheduler;
    HRESULT result = S_OK;
    const auto task = scheduler.task(path, result);
    VARIANT_BOOL value = VARIANT_FALSE;
    if (task) {
        result = task->get_Enabled(&value);
    }
    if (FAILED(result)) {
        failure = skipped(task_reason(result), static_cast<DWORD>(result));
        return false;
    }
    enabled = value != VARIANT_FALSE;
    return true;
}

ItemStatus set_task_enabled(const std::wstring& path, bool enabled) {
    if (platform::istarts_with(path, L"\\Microsoft\\")) {
        return skipped(Reason::Protected);
    }
    const TaskScheduler scheduler;
    HRESULT result = S_OK;
    const auto task = scheduler.task(path, result);
    if (task) {
        result = task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE);
    }
    if (FAILED(result)) {
        return {ItemState::Failed, task_reason(result), static_cast<std::uint32_t>(result)};
    }
    return {ItemState::Done, Reason::None, 0};
}

bool service_start(const std::wstring& name, leftovers::ServiceStart& start, ItemStatus& failure) {
    DWORD error = 0;
    const auto service = open_service(name, SERVICE_QUERY_CONFIG, error);
    if (!service) {
        failure = skipped(reason_for(error), error);
        return false;
    }
    DWORD needed = 0;
    ::QueryServiceConfigW(service.get(), nullptr, 0, &needed);
    std::vector<std::byte> buffer(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (needed == 0 || !::QueryServiceConfigW(service.get(), config, needed, &needed)) {
        error = ::GetLastError();
        failure = skipped(reason_for(error), error);
        return false;
    }
    if (config->dwServiceType & SERVICE_DRIVER) {
        failure = skipped(Reason::Protected);
        return false;
    }
    SERVICE_DELAYED_AUTO_START_INFO delayed{};
    if (!::QueryServiceConfig2W(service.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
                                reinterpret_cast<BYTE*>(&delayed), sizeof(delayed), &needed)) {
        error = ::GetLastError();
        failure = skipped(reason_for(error), error);
        return false;
    }
    start = {config->dwStartType, delayed.fDelayedAutostart != FALSE};
    return true;
}

ItemStatus set_service_start(const std::wstring& name, const leftovers::ServiceStart& target,
                             const leftovers::ServiceStart& current) {
    DWORD error = 0;
    const auto service = open_service(name, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG, error);
    if (!service) {
        return {ItemState::Failed, reason_for(error), error};
    }
    const auto set_type = [&](std::uint32_t type) {
        return ::ChangeServiceConfigW(service.get(), SERVICE_NO_CHANGE, type, SERVICE_NO_CHANGE, nullptr,
                                      nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != FALSE;
    };
    if (target.start_type != current.start_type && !set_type(target.start_type)) {
        error = ::GetLastError();
        return {ItemState::Failed, reason_for(error), error};
    }
    if (target.delayed != current.delayed) {
        SERVICE_DELAYED_AUTO_START_INFO delayed{target.delayed ? TRUE : FALSE};
        if (!::ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed)) {
            error = ::GetLastError();
            if (target.start_type != current.start_type) {
                set_type(current.start_type);  // Back as it was: the item failed, nothing changed
            }
            return {ItemState::Failed, reason_for(error), error};
        }
    }
    return {ItemState::Done, Reason::None, 0};
}

ItemStatus remove_package_item(const std::wstring& full_name, const std::wstring& user_sid) {
    if (!user_sid.empty() && !platform::iequals(user_sid, platform::current_user_sid())) {
        return skipped(Reason::OtherUser);
    }
    if (!uninstall::is_package_installed(full_name)) {
        return skipped(Reason::Missing);
    }
    const std::int32_t result = uninstall::remove_package(full_name);
    if (result < 0) {
        return {ItemState::Failed, result == E_ACCESSDENIED ? Reason::AccessDenied : Reason::Error,
                static_cast<std::uint32_t>(result)};
    }
    return {ItemState::Done, Reason::None, 0};
}

bool package_exists(const std::wstring& full_name) {
    return uninstall::is_package_installed(full_name);
}

Json to_json(const ServiceBackup& backup) {
    Json json;
    put_text(json, "name", backup.name);
    json["type"] = backup.type;
    json["start_type"] = backup.start_type;
    json["error_control"] = backup.error_control;
    put_text(json, "binary_path", backup.binary_path);
    put_text(json, "load_order_group", backup.load_order_group);
    json["dependencies"] = Json::array();
    for (const auto& dependency : backup.dependencies) {
        json["dependencies"].push_back(platform::to_utf8(dependency));
    }
    put_text(json, "start_name", backup.start_name);
    put_text(json, "display_name", backup.display_name);
    json["security"] = to_hex(backup.security);
    json["was_running"] = backup.was_running;
    json["key"] = to_json(backup.key);
    return json;
}

bool from_json(const Json& json, ServiceBackup& backup) {
    const auto name = get_text(json, "name");
    const auto binary = get_text(json, "binary_path");
    const auto security = from_hex(json.value("security", std::string()));
    if (!json.is_object() || !name || !binary || !security || !json.contains("key") ||
        !from_json(json["key"], backup.key)) {
        return false;
    }
    backup.name = *name;
    backup.type = json.value("type", DWORD{0});
    backup.start_type = json.value("start_type", DWORD{0});
    backup.error_control = json.value("error_control", DWORD{0});
    backup.binary_path = *binary;
    backup.load_order_group = get_text(json, "load_order_group").value_or(L"");
    backup.dependencies.clear();
    if (json.contains("dependencies") && json["dependencies"].is_array()) {
        for (const auto& dependency : json["dependencies"]) {
            backup.dependencies.push_back(platform::to_utf16(dependency.get<std::string>()));
        }
    }
    backup.start_name = get_text(json, "start_name").value_or(L"");
    backup.display_name = get_text(json, "display_name").value_or(L"");
    backup.security = *security;
    backup.was_running = json.value("was_running", false);
    return true;
}

Json to_json(const TaskBackup& backup) {
    Json json;
    put_text(json, "path", backup.path);
    put_text(json, "xml", backup.xml);
    put_text(json, "sddl", backup.sddl);
    return json;
}

bool from_json(const Json& json, TaskBackup& backup) {
    const auto path = get_text(json, "path");
    const auto xml = get_text(json, "xml");
    if (!json.is_object() || !path || !xml) {
        return false;
    }
    backup = {*path, *xml, get_text(json, "sddl").value_or(L"")};
    return true;
}

Json to_json(const TaskFolderBackup& backup) {
    Json json;
    put_text(json, "path", backup.path);
    put_text(json, "sddl", backup.sddl);
    return json;
}

bool from_json(const Json& json, TaskFolderBackup& backup) {
    const auto path = get_text(json, "path");
    if (!json.is_object() || !path) {
        return false;
    }
    backup = {*path, get_text(json, "sddl").value_or(L"")};
    return true;
}

}  // namespace avel::changes
