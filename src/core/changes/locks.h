#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace avel::changes {

struct LockingProcess {
    std::uint32_t pid = 0;
    std::wstring name;  // The application's display name, as Restart Manager reports it
    bool service = false;
    bool critical = false;  // Restart Manager can't shut it down (a system process)
};

// The processes holding any of `files` open, asked of Restart Manager (at most the first 1000
// files). Empty when nobody does, or when Windows can't tell (a folder held open by Explorer
// isn't always attributed to it).
std::vector<LockingProcess> processes_using(const std::vector<std::wstring>& files);

}  // namespace avel::changes
