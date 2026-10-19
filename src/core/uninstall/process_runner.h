#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace avel::uninstall {

// Lets another thread stop an uninstall run.
struct Cancellation {
    std::atomic<bool> stop_batch{false};         // Don't start any further programs
    std::atomic<bool> terminate_current{false};  // Kill the running uninstaller and all its processes
};

struct ProcessResult {
    std::uint32_t error = 0;      // Win32 error when the process couldn't be started
    std::uint32_t exit_code = 0;  // Exit code of the process Avel started (not of its children)
    bool terminated = false;      // Killed on request (Cancellation::terminate_current)

    bool started() const { return error == 0; }
};

// Starts `"executable" arguments` in a new job object and waits until every process in the job
// has exited. Uninstallers commonly hand off to a copy of themselves and exit at once (NSIS runs
// %TEMP%\~nsu.tmp\Au_.exe, Inno Setup a _iu*.tmp copy); waiting for the whole job rather than the
// first process catches that. The working directory is the executable's folder.
//
// The job doesn't kill its processes when Avel exits, and processes that explicitly ask to break
// away from it may do so (blocking them would make some uninstallers fail).
ProcessResult run_and_wait(const std::wstring& executable, const std::wstring& arguments,
                           const Cancellation& cancellation);

}  // namespace avel::uninstall
