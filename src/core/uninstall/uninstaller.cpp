#include "core/uninstall/uninstaller.h"

#include <windows.h>

#include <thread>

#include "core/uninstall/registration.h"

namespace avel::uninstall {

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr auto kMsiBusyRetryFor = 5min;
constexpr auto kMsiBusyRetryEvery = 5s;
// Some uninstallers drop their registration a moment after their processes end.
constexpr auto kRegistrationSettleTime = 3s;
constexpr auto kPollInterval = 250ms;

// Sleeps up to `duration`; returns false if the batch was cancelled meanwhile.
bool wait(std::chrono::milliseconds duration, const Cancellation& cancellation) {
    for (auto waited = 0ms; waited < duration; waited += kPollInterval) {
        if (cancellation.stop_batch || cancellation.terminate_current) {
            return false;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return true;
}

bool still_registered_after_settling(const inventory::Program& program) {
    const auto deadline = Clock::now() + kRegistrationSettleTime;
    for (;;) {
        if (!is_still_registered(program)) {
            return false;
        }
        if (Clock::now() >= deadline) {
            return true;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

}  // namespace

Outcome classify(std::uint32_t exit_code, bool still_registered) {
    if (exit_code == kRebootRequired || exit_code == kRebootInitiated) {
        return Outcome::RebootRequired;
    }
    return still_registered ? Outcome::StillInstalled : Outcome::Removed;
}

UninstallResult uninstall(const inventory::Program& program, const UninstallPlan& plan,
                          const Cancellation& cancellation, bool dry_run) {
    UninstallResult result;
    if (!plan.possible()) {
        result.outcome = Outcome::NotPossible;
        result.plan_error = plan.error;
        return result;
    }
    if (dry_run) {
        result.outcome = Outcome::Simulated;
        return result;
    }

    const auto started = Clock::now();
    const auto finish = [&](Outcome outcome) {
        result.outcome = outcome;
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        result.install_location_remains = install_location_exists(program);
        return result;
    };

    if (plan.method == Method::Package) {
        const std::int32_t hresult = remove_package(plan.package_full_name);
        if (hresult < 0) {
            result.error = static_cast<std::uint32_t>(hresult);
            return finish(is_package_installed(plan.package_full_name) ? Outcome::Failed : Outcome::Removed);
        }
        return finish(is_package_installed(plan.package_full_name) ? Outcome::StillInstalled
                                                                   : Outcome::Removed);
    }

    ProcessResult process;
    const auto retry_until = Clock::now() + kMsiBusyRetryFor;
    for (;;) {
        process = run_and_wait(plan.executable, plan.arguments, cancellation);
        const bool busy = plan.method == Method::Msi && process.started() && !process.terminated &&
                          process.exit_code == kMsiBusy;
        if (!busy || Clock::now() >= retry_until || !wait(kMsiBusyRetryEvery, cancellation)) {
            break;
        }
    }
    if (!process.started()) {
        result.error = process.error;
        return finish(Outcome::Failed);
    }
    result.exit_code = process.exit_code;
    if (plan.method == Method::Msi && process.exit_code == kMsiBusy) {
        result.error = kMsiBusy;
        return finish(Outcome::Failed);
    }
    const bool registered = still_registered_after_settling(program);
    if (process.terminated) {
        return finish(registered ? Outcome::Cancelled : Outcome::Removed);
    }
    return finish(classify(process.exit_code, registered));
}

std::string_view to_string(Outcome outcome) {
    switch (outcome) {
        case Outcome::Removed:
            return "removed";
        case Outcome::RebootRequired:
            return "reboot-required";
        case Outcome::StillInstalled:
            return "still-installed";
        case Outcome::Cancelled:
            return "cancelled";
        case Outcome::Failed:
            return "failed";
        case Outcome::NotPossible:
            return "not-possible";
        case Outcome::Simulated:
            return "simulated";
    }
    return "unknown";
}

}  // namespace avel::uninstall
