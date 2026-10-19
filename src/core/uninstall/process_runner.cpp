#include "core/uninstall/process_runner.h"

#include <windows.h>

#include <wil/resource.h>

#include <filesystem>
#include <system_error>

namespace avel::uninstall {

namespace {

constexpr DWORD kPollMilliseconds = 250;

std::wstring working_directory_for(const std::wstring& executable) {
    const std::filesystem::path path(executable);
    if (!path.is_absolute()) {
        return {};
    }
    std::error_code error;
    const auto parent = path.parent_path();
    return std::filesystem::is_directory(parent, error) ? parent.wstring() : std::wstring();
}

bool job_is_empty(HANDLE job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    return ::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting,
                                       sizeof(accounting), nullptr) &&
           accounting.ActiveProcesses == 0;
}

}  // namespace

ProcessResult run_and_wait(const std::wstring& executable, const std::wstring& arguments,
                           const Cancellation& cancellation) {
    ProcessResult result;
    const auto fail = [&result] {
        result.error = ::GetLastError();
        return result;
    };

    const wil::unique_handle job(::CreateJobObjectW(nullptr, nullptr));
    const wil::unique_handle port(::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!job || !port) {
        return fail();
    }
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
    association.CompletionKey = job.get();
    association.CompletionPort = port.get();
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!::SetInformationJobObject(job.get(), JobObjectAssociateCompletionPortInformation, &association,
                                   sizeof(association)) ||
        !::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return fail();
    }

    // Quoting the executable stops CreateProcess from guessing at unquoted paths with spaces.
    std::wstring command_line = L'"' + executable + L'"';
    if (!arguments.empty()) {
        command_line += L' ';
        command_line += arguments;
    }
    const std::wstring directory = working_directory_for(executable);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    wil::unique_process_information process;
    // Suspended until it's in the job, so it can't start children outside of it.
    if (!::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                          directory.empty() ? nullptr : directory.c_str(), &startup, &process)) {
        return fail();
    }
    if (!::AssignProcessToJobObject(job.get(), process.hProcess)) {
        result.error = ::GetLastError();
        ::TerminateProcess(process.hProcess, ERROR_CANCELLED);
        return result;
    }
    ::ResumeThread(process.hThread);

    for (;;) {
        if (cancellation.terminate_current && !result.terminated) {
            ::TerminateJobObject(job.get(), ERROR_CANCELLED);
            result.terminated = true;
        }
        DWORD message = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;
        if (::GetQueuedCompletionStatus(port.get(), &message, &key, &overlapped, kPollMilliseconds)) {
            if (key == reinterpret_cast<ULONG_PTR>(job.get()) &&
                message == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) {
                break;
            }
            continue;
        }
        // Job notifications aren't guaranteed to arrive, so also ask the job directly.
        if (job_is_empty(job.get())) {
            break;
        }
    }

    ::WaitForSingleObject(process.hProcess, INFINITE);  // Already exited; makes the exit code final
    DWORD exit_code = 0;
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = exit_code;
    return result;
}

}  // namespace avel::uninstall
