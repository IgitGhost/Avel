// Running processes: what runs from a program's folder, and ending it on the user's say-so.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace avel::platform {

struct ProcessInfo {
    std::uint32_t pid = 0;
    std::uint32_t parent_pid = 0;
    std::uint32_t session = 0;
    std::wstring name;      // The executable's file name
    std::wstring image;     // Its full path, normalized; empty when Windows won't tell (protected processes)
    bool critical = false;  // Ending it would stop Windows (a bug check)
};

// Every running process, from one snapshot.
std::vector<ProcessInfo> running_processes();

// The processes that host running services. Services are stopped through the Service Control
// Manager, never ended as processes.
std::vector<std::uint32_t> service_process_ids();

enum class StopOutcome : std::uint8_t {
    Closed,        // It closed its windows and exited
    Terminated,    // It was ended
    Gone,          // It had exited already
    AccessDenied,  // Windows refused (a protected or more privileged process)
    Failed,        // It is still running; see the error
};

struct StopResult {
    StopOutcome outcome = StopOutcome::Failed;
    std::uint32_t error = 0;
};

// Asks the process to close (WM_CLOSE to its top-level windows) and waits up to `grace`; then
// ends it. Never call it for critical processes or service hosts.
StopResult stop_process(std::uint32_t pid, std::chrono::milliseconds grace = std::chrono::seconds(5));

std::string_view to_string(StopOutcome outcome);

}  // namespace avel::platform
