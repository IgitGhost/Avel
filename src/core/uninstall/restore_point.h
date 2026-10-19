#pragma once

#include <cstdint>
#include <string_view>

namespace avel::uninstall {

struct RestorePointResult {
    // 0 on success. ERROR_SERVICE_DISABLED (1058) means System Protection is off for the system drive.
    std::uint32_t error = 0;
    std::int64_t sequence = 0;

    bool created() const { return error == 0; }
};

// Starts an "application uninstall" System Restore point. `description` is shown in System Restore
// (up to 255 characters). Can take a while; call it from a worker thread. The process should have
// called platform::initialize_com_security() first.
// Windows creates at most one such restore point per 24 hours by default; within that window the
// call still succeeds without creating a new one.
RestorePointResult begin_restore_point(std::wstring_view description);

// Completes a restore point started by begin_restore_point(). Without changes it's discarded.
void end_restore_point(std::int64_t sequence, bool changes_made);

}  // namespace avel::uninstall
