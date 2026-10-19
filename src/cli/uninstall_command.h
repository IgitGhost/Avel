#pragma once

#include <string_view>
#include <vector>

namespace avel::cli {

// `avel-cli uninstall <program>... [--quiet] [--dry-run] [--yes] [--no-restore-point] [--json]`.
// Returns the process exit code: 0 when every program was removed (or would be, with --dry-run),
// 1 when any was not, 2 for usage errors.
int run_uninstall_command(const std::vector<std::wstring_view>& arguments);

}  // namespace avel::cli
