#include "core/platform/scheduled_tasks.h"

#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>

#include <wil/com.h>
#include <wil/resource.h>

#include "core/platform/text.h"

namespace avel::platform {

namespace {

wil::com_ptr_nothrow<ITaskService> connect(HRESULT& result) {
    wil::com_ptr_nothrow<ITaskService> service;
    const VARIANT empty{};
    result =
        ::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(service.put()));
    if (SUCCEEDED(result)) {
        result = service->Connect(empty, empty, empty, empty);
    }
    return SUCCEEDED(result) ? service : nullptr;
}

void read_actions(IActionCollection* actions, ScheduledTask& entry) {
    LONG count = 0;
    actions->get_Count(&count);
    for (LONG i = 1; i <= count; ++i) {
        wil::com_ptr_nothrow<IAction> action;
        TASK_ACTION_TYPE type{};
        if (FAILED(actions->get_Item(i, action.put())) || FAILED(action->get_Type(&type))) {
            continue;
        }
        if (type != TASK_ACTION_EXEC) {
            entry.other_actions = true;
            continue;
        }
        const auto exec = action.try_query<IExecAction>();
        wil::unique_bstr program;
        wil::unique_bstr arguments;
        if (!exec || FAILED(exec->get_Path(&program)) || !program) {
            continue;
        }
        exec->get_Arguments(&arguments);
        entry.actions.push_back(exec_command(program.get(), arguments ? arguments.get() : L""));
    }
}

void read_triggers(ITaskDefinition* definition, ScheduledTask& entry) {
    wil::com_ptr_nothrow<ITriggerCollection> triggers;
    LONG count = 0;
    if (FAILED(definition->get_Triggers(triggers.put())) || FAILED(triggers->get_Count(&count))) {
        return;
    }
    for (LONG i = 1; i <= count; ++i) {
        wil::com_ptr_nothrow<ITrigger> trigger;
        VARIANT_BOOL enabled = VARIANT_TRUE;
        TASK_TRIGGER_TYPE2 type{};
        if (FAILED(triggers->get_Item(i, trigger.put())) || FAILED(trigger->get_Type(&type)) ||
            (SUCCEEDED(trigger->get_Enabled(&enabled)) && enabled == VARIANT_FALSE)) {
            continue;
        }
        entry.triggers.push_back(type == TASK_TRIGGER_LOGON  ? TaskTrigger::Logon
                                 : type == TASK_TRIGGER_BOOT ? TaskTrigger::Boot
                                                             : TaskTrigger::Other);
    }
}

std::optional<ScheduledTask> read_task(IRegisteredTask* task) {
    wil::unique_bstr path;
    wil::com_ptr_nothrow<ITaskDefinition> definition;
    wil::com_ptr_nothrow<IActionCollection> actions;
    if (FAILED(task->get_Path(&path)) || FAILED(task->get_Definition(definition.put())) ||
        FAILED(definition->get_Actions(actions.put()))) {
        return std::nullopt;  // Tasks this account may not read
    }
    ScheduledTask entry;
    entry.path = path ? path.get() : L"";
    read_actions(actions.get(), entry);
    read_triggers(definition.get(), entry);
    VARIANT_BOOL enabled = VARIANT_TRUE;
    if (SUCCEEDED(task->get_Enabled(&enabled))) {
        entry.enabled = enabled != VARIANT_FALSE;
    }
    wil::com_ptr_nothrow<IRegistrationInfo> registration;
    wil::unique_bstr author;
    if (SUCCEEDED(definition->get_RegistrationInfo(registration.put())) && registration &&
        SUCCEEDED(registration->get_Author(&author)) && author) {
        entry.author = author.get();
    }
    return entry;
}

// Collects the tasks of `folder` and its subfolders.
void collect_tasks(ITaskFolder* folder, bool skip_windows, std::vector<ScheduledTask>& tasks) {
    wil::unique_bstr folder_path;
    if (skip_windows && SUCCEEDED(folder->get_Path(&folder_path)) && folder_path &&
        is_windows_task(folder_path.get())) {
        return;
    }
    wil::com_ptr_nothrow<IRegisteredTaskCollection> collection;
    LONG count = 0;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, collection.put())) &&
        SUCCEEDED(collection->get_Count(&count))) {
        for (LONG i = 1; i <= count; ++i) {
            VARIANT index{};
            index.vt = VT_I4;
            index.lVal = i;
            wil::com_ptr_nothrow<IRegisteredTask> task;
            if (FAILED(collection->get_Item(index, task.put()))) {
                continue;
            }
            if (auto entry = read_task(task.get())) {
                tasks.push_back(std::move(*entry));
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
                collect_tasks(subfolder.get(), skip_windows, tasks);
            }
        }
    }
}

}  // namespace

bool is_windows_task(std::wstring_view path) {
    constexpr std::wstring_view kWindows = LR"(\Microsoft)";
    return istarts_with(path, kWindows) && (path.size() == kWindows.size() || path[kWindows.size()] == L'\\');
}

HRESULT scheduled_tasks(bool skip_windows, std::vector<ScheduledTask>& tasks) {
    tasks.clear();
    HRESULT result = S_OK;
    const auto service = connect(result);
    if (!service) {
        return result;
    }
    wil::com_ptr_nothrow<ITaskFolder> root;
    const auto root_path = wil::make_bstr_nothrow(L"\\");
    if (!root_path) {
        return E_OUTOFMEMORY;
    }
    result = service->GetFolder(root_path.get(), root.put());
    if (FAILED(result)) {
        return result;
    }
    collect_tasks(root.get(), skip_windows, tasks);
    return S_OK;
}

std::optional<ScheduledTask> scheduled_task(const std::wstring& path) {
    HRESULT result = S_OK;
    const auto service = connect(result);
    wil::com_ptr_nothrow<ITaskFolder> root;
    wil::com_ptr_nothrow<IRegisteredTask> task;
    const auto root_path = wil::make_bstr_nothrow(L"\\");
    const auto task_path = wil::make_bstr_nothrow(path.c_str());
    if (!service || !root_path || !task_path || FAILED(service->GetFolder(root_path.get(), root.put())) ||
        FAILED(root->GetTask(task_path.get(), task.put()))) {
        return std::nullopt;
    }
    return read_task(task.get());
}

std::wstring exec_command(std::wstring_view program, std::wstring_view arguments) {
    std::wstring command(trim(program));
    if (command.find(L' ') != std::wstring::npos && command.front() != L'"') {
        command = L"\"" + command + L"\"";
    }
    if (!arguments.empty()) {
        command += L" " + std::wstring(arguments);
    }
    return command;
}

}  // namespace avel::platform
