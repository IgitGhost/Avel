// The Task Scheduler's tasks, read through its COM API. Reading needs no administrator rights,
// but a task the account may not read is left out.
#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace avel::platform {

enum class TaskTrigger : std::uint8_t {
    Logon,  // At log on (TASK_TRIGGER_LOGON)
    Boot,   // At startup (TASK_TRIGGER_BOOT)
    Other,  // On a schedule, on an event, at idle, ...
};

struct ScheduledTask {
    std::wstring path;                  // e.g. \Contoso\Updater
    std::vector<std::wstring> actions;  // Command lines of its Exec actions (see exec_command)
    bool other_actions = false;         // It has actions that run no command line (COM handlers, ...)
    std::vector<TaskTrigger> triggers;  // The kinds of its enabled triggers
    bool enabled = true;
    std::wstring author;
};

// A task of Windows' own: one in the \Microsoft folder or below it.
bool is_windows_task(std::wstring_view path);

// Every task this account may read, from the root folder down; with `skip_windows`, without
// the \Microsoft folder. Needs COM on the calling thread. Returns S_OK, or the error from reaching
// the Task Scheduler (`tasks` is then left empty).
HRESULT scheduled_tasks(bool skip_windows, std::vector<ScheduledTask>& tasks);

// One task by its path; nullopt if there is no such task or it can't be read. Needs COM.
std::optional<ScheduledTask> scheduled_task(const std::wstring& path);

// An Exec action as one command line: the program, in quotes when it holds a space, then its
// arguments.
std::wstring exec_command(std::wstring_view program, std::wstring_view arguments);

}  // namespace avel::platform
