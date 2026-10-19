#pragma once

namespace avel::platform {

// Returns true when the current process token is elevated ("Run as administrator").
bool is_process_elevated();

}  // namespace avel::platform
