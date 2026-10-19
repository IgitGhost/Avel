#pragma once

#include <string_view>
#include <vector>

namespace avel::cli {

// `avel-cli list [--all] [--json] [--timing]`. Returns the process exit code.
int run_list_command(const std::vector<std::wstring_view>& arguments);

}  // namespace avel::cli
