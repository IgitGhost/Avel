#pragma once

#include <string_view>
#include <vector>

namespace avel::cli {

// avel-cli apps: lists the packaged apps on this PC, and removes them for the current user, stops
// installing them for new users, or removes them for everyone.
int run_apps_command(const std::vector<std::wstring_view>& arguments);

}  // namespace avel::cli
