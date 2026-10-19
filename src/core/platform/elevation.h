#pragma once

#include <string_view>

namespace avel::platform {

// Returns true when the current process token is elevated ("Run as administrator").
bool is_process_elevated();

// Enables a privilege the process token holds but has disabled (SeRestorePrivilege, for
// instance). Returns false if the token doesn't hold it. The privilege stays enabled for the
// rest of the process's life.
bool enable_privilege(std::wstring_view name);

}  // namespace avel::platform
