// Removing services, scheduled tasks, task folders and packages, and putting services, tasks and
// task folders back.
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

struct TaskFolderBackup {
    std::wstring path;  // \Folder
    std::wstring sddl;  // Its security
};

// A task folder is removed only when empty (tasks are removed before folders, deeper folders
// first). Skipped when it still holds tasks or folders (`not-empty`), for the root and Windows' own
// folders (\Microsoft...), and when it's gone.
std::optional<TaskFolderBackup> backup_task_folder(const std::wstring& path, ItemStatus& failure);
ItemStatus remove_task_folder(const TaskFolderBackup& backup);
// Created again (with any missing parents), then given its security if it differs.
ItemStatus restore_task_folder(const TaskFolderBackup& backup);
bool task_folder_exists(const std::wstring& path);

// State changes (ADR 0007). The queries fail with `failure` set to a skipped status (missing,
// access-denied, error); the changes return done, or failed with the reason. The executor maps
// those to restore states for undo.

// Whether the task is enabled.
bool task_enabled(const std::wstring& path, bool& enabled, ItemStatus& failure);
// Turns it on or off (IRegisteredTask::put_Enabled). Windows' own tasks (\Microsoft\...) are
// skipped as protected.
ItemStatus set_task_enabled(const std::wstring& path, bool enabled);

// How the service starts. Drivers are skipped as protected: their start is never Avel's to change.
bool service_start(const std::wstring& name, leftovers::ServiceStart& start, ItemStatus& failure);
// Changes the start type from `current` to `target`'s (ChangeServiceConfigW, SERVICE_NO_CHANGE for
// everything else), and the delayed flag only where it differs, so nothing else in the service's
// key changes: a DelayedAutostart value is never added where there was none.
ItemStatus set_service_start(const std::wstring& name, const leftovers::ServiceStart& target,
                             const leftovers::ServiceStart& current);

// Removes a package registered for the current user (`user_sid`); other users' are skipped.
// Packages can't be put back: the caller must have allowed that.
ItemStatus remove_package_item(const std::wstring& full_name, const std::wstring& user_sid);
bool package_exists(const std::wstring& full_name);

Json to_json(const ServiceBackup& backup);
bool from_json(const Json& json, ServiceBackup& backup);
Json to_json(const TaskBackup& backup);
bool from_json(const Json& json, TaskBackup& backup);
Json to_json(const TaskFolderBackup& backup);
bool from_json(const Json& json, TaskFolderBackup& backup);

}  // namespace avel::changes
