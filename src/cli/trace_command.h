#pragma once

#include <string_view>
#include <vector>

namespace avel::cli {

// avel-cli trace start | stop | run | list | show | cancel | delete: Avel Trace, which records what
// an installer changes (docs/trace.md).
int run_trace_command(const std::vector<std::wstring_view>& arguments);

}  // namespace avel::cli
