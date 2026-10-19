#pragma once

#include <string_view>
#include <vector>

namespace avel::cli {

// avel-cli scan <program>... | --all [--json] [--min-level questionable|good|very-good] [--all-levels]
int run_scan_command(const std::vector<std::wstring_view>& arguments);

}  // namespace avel::cli
