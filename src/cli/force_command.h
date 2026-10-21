#pragma once

#include <string_view>
#include <vector>

namespace avel::cli {

// avel-cli force: removes programs whose uninstaller is missing or broken, without running it.
int run_force_command(const std::vector<std::wstring_view>& arguments);

}  // namespace avel::cli
