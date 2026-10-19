#pragma once

#include <string_view>
#include <vector>

namespace avel::cli {

// avel-cli undo --list | --show <id> | <id> [--item <n>]... | --purge | --recover [--json]
int run_undo_command(const std::vector<std::wstring_view>& arguments);

}  // namespace avel::cli
