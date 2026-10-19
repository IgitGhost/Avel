// Removing services, scheduled tasks and packages, and putting services and tasks back.
#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "core/changes/change_set.h"
#include "core/changes/registry_backup.h"
#include "core/changes/serialization.h"

namespace avel::changes {

// Everything needed to recreate a service: its configuration as the Service Control Manager reports
// it, its security, and its registry key (Parameters, description, failure actions, ...).
struct ServiceBackup {
    std::wstring name;
    DWORD type = 0;
    DWORD start_type = 0;
    DWORD error_control = 0;
    std::wstring binary_path;
    std::wstring load_order_group;
    std::vector<std::wstring> dependencies;
    std::wstring start_name;  // The account it runs as
    std::wstring display_name;
    std::vector<std::byte> security;  // Owner, group and DACL of the service object
    bool was_running = false;
    KeySnapshot key;  // SYSTEM\CurrentControlSet\Services\<name>, without its Security subkey
};

// Checks that the service may be removed and backs it up. Skipped when it's a driver (never
// Avel's to remove here), other services depend on it, or it runs under an account with a password
// (undo couldn't restore that). Reads its key through `services`, the opened Services key.
std::optional<ServiceBackup> backup_service(HKEY services, const std::wstring& name, ItemStatus& failure);
// Stops the service (waiting up to 30 s) and deletes it.
ItemStatus remove_service(const ServiceBackup& backup);
// Recreates it, fills in its key, applies its security, and starts it if it was running.
ItemStatus restore_service(HKEY services, const ServiceBackup& backup);
bool service_exists(const std::wstring& name);

struct TaskBackup {
    std::wstring path;  // \Folder\Name
    std::wstring xml;   // The task's definition
    std::wstring sddl;  // Its security
};

// Skipped for Windows' own tasks (\Microsoft\...) and tasks that store a password.
std::optional<TaskBackup> backup_task(const std::wstring& path, ItemStatus& failure);
ItemStatus remove_task(const TaskBackup& backup);
ItemStatus restore_task(const TaskBackup& backup);
bool task_exists(const std::wstring& path);

// Removes a package registered for the current user (`user_sid`); other users' are skipped.
// Packages can't be put back: the caller must have allowed that.
ItemStatus remove_package_item(const std::wstring& full_name, const std::wstring& user_sid);
bool package_exists(const std::wstring& full_name);

Json to_json(const ServiceBackup& backup);
bool from_json(const Json& json, ServiceBackup& backup);
Json to_json(const TaskBackup& backup);
bool from_json(const Json& json, TaskBackup& backup);

}  // namespace avel::changes
