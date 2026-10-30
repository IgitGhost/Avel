// The temporary folder with 8.3 short names expanded, for fixtures whose paths are compared with
// what the engine reports. The engine expands short names (platform::long_path), and TEMP may hold
// them: on GitHub's runners it is C:\Users\RUNNER~1\AppData\Local\Temp.
#pragma once

#include <filesystem>

#include "core/platform/paths.h"

namespace avel::testing {

inline std::filesystem::path long_temp_directory() {
    return platform::long_path(std::filesystem::temp_directory_path().wstring());
}

}  // namespace avel::testing
