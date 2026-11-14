#pragma once

#include <string_view>
#include <vector>

namespace avel::cli {

// avel-cli extensions: lists every browser profile's extensions, and removes the ones that policies or
// external registrations install.
int run_extensions_command(const std::vector<std::wstring_view>& arguments);

}  // namespace avel::cli
