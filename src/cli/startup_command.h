#pragma once

#include <string_view>
#include <vector>

namespace avel::cli {

// avel-cli startup: lists what starts with Windows, and disables, enables or deletes entries.
int run_startup_command(const std::vector<std::wstring_view>& arguments);

}  // namespace avel::cli
