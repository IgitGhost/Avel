#include "core/platform/processes.h"

#include <windows.h>
#include <tlhelp32.h>

#include <wil/resource.h>

#include "core/platform/paths.h"

namespace avel::platform {

namespace {

std::wstring image_of(HANDLE process) {
    std::wstring path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD size = static_cast<DWORD>(path.size());
        if (::QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
            path.resize(size);
            return normalize_path(path);
        }
        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
        path.resize(path.size() * 4);
    }
    return {};
}

struct CloseRequest {
    DWORD pid = 0;
    bool posted = false;
};

BOOL CALLBACK close_window(HWND window, LPARAM parameter) {
    auto* request = reinterpret_cast<CloseRequest*>(parameter);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    // The process's main windows: visible, top-level and not owned by another window.
    if (pid == request->pid && ::IsWindowVisible(window) && ::GetWindow(window, GW_OWNER) == nullptr) {
        request->posted |= ::PostMessageW(window, WM_CLOSE, 0, 0) != FALSE;
    }
    return TRUE;
}

}  // namespace

std::vector<ProcessInfo> running_processes() {
    std::vector<ProcessInfo> processes;
    wil::unique_hfile snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return processes;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = ::Process32FirstW(snapshot.get(), &entry); more;
         more = ::Process32NextW(snapshot.get(), &entry)) {
        ProcessInfo info;
        info.pid = entry.th32ProcessID;
        info.parent_pid = entry.th32ParentProcessID;
        info.name = entry.szExeFile;
        DWORD session = 0;
        if (::ProcessIdToSessionId(info.pid, &session)) {
            info.session = session;
        }
        if (info.pid != 0) {
            wil::unique_handle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, info.pid));
            if (process) {
                info.image = image_of(process.get());
                BOOL critical = FALSE;
                info.critical = ::IsProcessCritical(process.get(), &critical) && critical;
            }
        }
        processes.push_back(std::move(info));
    }
    return processes;
}

std::vector<std::uint32_t> service_process_ids() {
    std::vector<std::uint32_t> pids;
    wil::unique_schandle manager(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE));
    if (!manager) {
        return pids;
    }
    std::vector<BYTE> buffer(64 * 1024);
    DWORD resume = 0;
    while (true) {
        DWORD needed = 0;
        DWORD count = 0;
        const BOOL ok = ::EnumServicesStatusExW(
            manager.get(), SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, buffer.data(),
            static_cast<DWORD>(buffer.size()), &needed, &count, &resume, nullptr);
        if (!ok && ::GetLastError() != ERROR_MORE_DATA) {
            break;
        }
        const auto* services = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
        for (DWORD i = 0; i < count; ++i) {
            if (services[i].ServiceStatusProcess.dwProcessId != 0) {
                pids.push_back(services[i].ServiceStatusProcess.dwProcessId);
            }
        }
        if (ok) {
            break;
        }
        if (needed > buffer.size()) {
            buffer.resize(needed);
        }
    }
    return pids;
}

StopResult stop_process(std::uint32_t pid, std::chrono::milliseconds grace) {
    wil::unique_handle process(
        ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_INVALID_PARAMETER) {
            return {StopOutcome::Gone, 0};
        }
        return {error == ERROR_ACCESS_DENIED ? StopOutcome::AccessDenied : StopOutcome::Failed, error};
    }
    if (::WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
        return {StopOutcome::Gone, 0};
    }
    CloseRequest request{pid};
    ::EnumWindows(close_window, reinterpret_cast<LPARAM>(&request));
    if (request.posted &&
        ::WaitForSingleObject(process.get(), static_cast<DWORD>(grace.count())) == WAIT_OBJECT_0) {
        return {StopOutcome::Closed, 0};
    }
    if (!::TerminateProcess(process.get(), 1)) {
        const DWORD error = ::GetLastError();
        if (::WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
            return {StopOutcome::Gone, 0};
        }
        return {error == ERROR_ACCESS_DENIED ? StopOutcome::AccessDenied : StopOutcome::Failed, error};
    }
    if (::WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0) {
        return {StopOutcome::Failed, WAIT_TIMEOUT};
    }
    return {StopOutcome::Terminated, 0};
}

std::string_view to_string(StopOutcome outcome) {
    switch (outcome) {
        case StopOutcome::Closed:
            return "closed";
        case StopOutcome::Terminated:
            return "terminated";
        case StopOutcome::Gone:
            return "gone";
        case StopOutcome::AccessDenied:
            return "access-denied";
        case StopOutcome::Failed:
            return "failed";
    }
    return "unknown";
}

}  // namespace avel::platform
