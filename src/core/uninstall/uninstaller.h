#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

#include "core/inventory/program.h"
#include "core/uninstall/process_runner.h"
#include "core/uninstall/uninstall_plan.h"

namespace avel::uninstall {

enum class Outcome : std::uint8_t {
    Removed,         // The program's registration is gone
    RebootRequired,  // Removed, but a restart completes the removal
    StillInstalled,  // The uninstaller finished, but the program is still registered (cancelled or failed)
    Cancelled,       // Stopped by the user, before or while it ran
    Failed,          // The uninstaller couldn't be started, or the package manager reported an error
    NotPossible,     // The plan says it can't be uninstalled; see UninstallResult::plan_error
    Simulated,       // Dry run: nothing was executed
};

struct UninstallResult {
    Outcome outcome = Outcome::Failed;
    PlanError plan_error = PlanError::None;
    std::optional<std::uint32_t> exit_code;  // Of the uninstaller process, when one ran
    std::uint32_t error = 0;                 // Win32 error or HRESULT when Failed
    std::chrono::milliseconds elapsed{};
    bool install_location_remains = false;  // Leftovers on disk (Phase 3 cleans them up)
};

// Windows Installer exit codes that matter here.
inline constexpr std::uint32_t kMsiUserCancelled = 1602;
inline constexpr std::uint32_t kMsiUnknownProduct = 1605;
inline constexpr std::uint32_t kMsiBusy = 1618;  // Another installation is in progress
inline constexpr std::uint32_t kRebootInitiated = 1641;
inline constexpr std::uint32_t kRebootRequired = 3010;

// Outcome from an uninstaller's exit code and whether the program is still registered afterwards.
// Pure.
Outcome classify(std::uint32_t exit_code, bool still_registered);

// Runs `plan` for `program` and checks the result. Blocks until the uninstaller and all its
// processes have exited. msiexec runs that find another installation in progress are retried for
// a few minutes. With `dry_run`, only reports what would happen.
UninstallResult uninstall(const inventory::Program& program, const UninstallPlan& plan,
                          const Cancellation& cancellation, bool dry_run = false);

std::string_view to_string(Outcome outcome);

}  // namespace avel::uninstall
