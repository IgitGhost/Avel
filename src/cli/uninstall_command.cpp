#include "cli/uninstall_command.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <format>
#include <iostream>
#include <optional>
#include <string>

#include "cli/console.h"
#include "cli/leftover_output.h"
#include "cli/program_lookup.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/post_uninstall.h"
#include "core/platform/elevation.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "core/uninstall/batch.h"
#include "core/uninstall/installer_detection.h"
#include "core/version.h"

namespace avel::cli {

namespace {

using inventory::Program;
using platform::to_utf8;
using uninstall::Outcome;
using Json = nlohmann::ordered_json;

constexpr std::string_view kUsage =
    "Usage: avel-cli uninstall <program>... [--quiet] [--dry-run] [--yes] [--no-restore-point]\n"
    "                          [--no-leftover-scan] [--json]\n"
    "  <program>           an id from `avel-cli list --json`, or an exact display name\n"
    "  --quiet             use silent uninstall switches where known (interactive otherwise)\n"
    "  --dry-run           show what would run, change nothing\n"
    "  --yes               don't ask for confirmation\n"
    "  --no-restore-point  skip the System Restore point\n"
    "  --no-leftover-scan  don't look for what the uninstallers left behind\n"
    "  --json              machine-readable output (needs --yes or --dry-run)\n";

struct Options {
    std::vector<std::wstring> programs;
    bool quiet = false;
    bool dry_run = false;
    bool yes = false;
    bool restore_point = true;
    bool leftover_scan = true;
    bool json = false;
};

// Ctrl+C stops the batch after the running uninstaller; a second Ctrl+C terminates that one too.
uninstall::Cancellation g_cancellation;

BOOL WINAPI on_console_control(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) {
        return FALSE;
    }
    if (g_cancellation.stop_batch.exchange(true)) {
        g_cancellation.terminate_current = true;
        print_error("\nTerminating the running uninstaller...\n");
    } else {
        print_error("\nStopping after the running uninstaller (Ctrl+C again to terminate it)...\n");
    }
    return TRUE;
}

std::string describe_plan(const uninstall::UninstallPlan& plan) {
    if (!plan.possible()) {
        return std::format("cannot uninstall: {}", uninstall::to_string(plan.error));
    }
    if (plan.method == uninstall::Method::Package) {
        return "remove package " + to_utf8(plan.package_full_name);
    }
    return std::format("{}: {}", plan.quiet ? "quiet" : "interactive", to_utf8(plan.command_line()));
}

std::string describe_result(const uninstall::UninstallResult& result) {
    std::string text(uninstall::to_string(result.outcome));
    if (result.outcome == Outcome::NotPossible) {
        text += std::format(" ({})", uninstall::to_string(result.plan_error));
    }
    if (result.exit_code) {
        text += std::format(", exit code {}", *result.exit_code);
    }
    if (result.outcome == Outcome::Failed && result.error != 0) {
        text += std::format(", error 0x{:08X}", result.error);
    }
    if (result.elapsed.count() > 0) {
        text += std::format(", {:.1f} s", result.elapsed.count() / 1000.0);
    }
    if (result.install_location_remains) {
        text += ", install folder remains";
    }
    return text;
}

bool succeeded(Outcome outcome) {
    return outcome == Outcome::Removed || outcome == Outcome::RebootRequired || outcome == Outcome::Simulated;
}

Json to_json(const uninstall::BatchItem& item, const uninstall::UninstallResult& result) {
    const auto& plan = item.plan;
    Json json;
    json["id"] = to_utf8(item.program->id);
    json["name"] = to_utf8(item.program->display_name);
    json["plan"] = {{"possible", plan.possible()},
                    {"error", uninstall::to_string(plan.error)},
                    {"method", uninstall::to_string(plan.method)},
                    {"quiet", plan.quiet},
                    {"installer_type", inventory::to_string(plan.installer_type)},
                    {"command", to_utf8(plan.command_line())},
                    {"package_full_name", to_utf8(plan.package_full_name)}};
    json["outcome"] = uninstall::to_string(result.outcome);
    json["exit_code"] = result.exit_code ? Json(*result.exit_code) : Json(nullptr);
    json["error"] = result.error;
    json["elapsed_ms"] = result.elapsed.count();
    json["install_location_remains"] = result.install_location_remains;
    return json;
}

bool confirm(std::size_t count) {
    print(std::format("\nUninstall {} program{}? [y/N] ", count, count == 1 ? "" : "s"));
    std::string answer;
    std::getline(std::cin, answer);
    return !answer.empty() && (answer.front() == 'y' || answer.front() == 'Y');
}

}  // namespace

int run_uninstall_command(const std::vector<std::wstring_view>& arguments) {
    Options options;
    for (const auto argument : arguments) {
        if (argument == L"--quiet") {
            options.quiet = true;
        } else if (argument == L"--dry-run") {
            options.dry_run = true;
        } else if (argument == L"--yes" || argument == L"-y") {
            options.yes = true;
        } else if (argument == L"--no-restore-point") {
            options.restore_point = false;
        } else if (argument == L"--no-leftover-scan") {
            options.leftover_scan = false;
        } else if (argument == L"--json") {
            options.json = true;
        } else if (argument.starts_with(L"--")) {
            print_error("uninstall: unknown option " + to_utf8(argument) + "\n" + std::string(kUsage));
            return 2;
        } else {
            options.programs.emplace_back(argument);
        }
    }
    if (options.programs.empty()) {
        print_error(kUsage);
        return 2;
    }
    if (options.json && !options.yes && !options.dry_run) {
        print_error("uninstall: --json needs --yes or --dry-run (it can't ask for confirmation)\n");
        return 2;
    }

    const auto inventory = inventory::scan_inventory();
    std::vector<const Program*> programs;
    if (!resolve(inventory, options.programs, programs)) {
        return 2;
    }

    uninstall::PlanContext context;
    context.current_user_sid = platform::current_user_sid();
    context.elevated = platform::is_process_elevated();
    context.detect_installer = uninstall::detect_installer_type;
    const auto mode = options.quiet ? uninstall::Mode::Quiet : uninstall::Mode::Interactive;

    std::vector<uninstall::BatchItem> items;
    for (const auto* program : programs) {
        items.push_back({program, uninstall::make_uninstall_plan(*program, mode, context)});
    }

    if (!options.json) {
        print(options.dry_run ? "Dry run, nothing will be changed:\n" : "Plan:\n");
        for (const auto& item : items) {
            print(std::format("  {}\n      {}\n", name_of(*item.program), describe_plan(item.plan)));
        }
        if (!context.elevated) {
            print(
                "Note: not running as administrator; programs installed for all users can't be "
                "uninstalled.\n");
        }
    }
    if (!options.dry_run && !options.yes && !confirm(items.size())) {
        print("Cancelled.\n");
        return 1;
    }

    uninstall::BatchOptions batch_options;
    batch_options.dry_run = options.dry_run;
    batch_options.restore_point = options.restore_point;
    batch_options.restore_point_description =
        programs.size() == 1 ? L"Avel: uninstall " + programs.front()->display_name
                             : std::format(L"Avel: uninstall {} programs", programs.size());

    uninstall::BatchEvents events;
    if (!options.json) {
        events.restore_point_done = [](uninstall::RestorePointStatus status, std::uint32_t error) {
            if (status == uninstall::RestorePointStatus::Created) {
                print("System Restore point created.\n");
            } else {
                print_error(std::format(
                    "warning: no System Restore point (error {}); continuing without one\n", error));
            }
        };
        events.item_started = [&](std::size_t index) {
            print(std::format("[{}/{}] {} ... ", index + 1, items.size(), name_of(*items[index].program)));
        };
        events.item_finished = [&](std::size_t index, const uninstall::UninstallResult& result) {
            if (result.outcome == Outcome::Cancelled && !result.exit_code) {
                print(
                    std::format("[{}/{}] {} ... ", index + 1, items.size(), name_of(*items[index].program)));
            }
            print(describe_result(result) + "\n");
        };
    }

    // A simulated run removes nothing, so there would be nothing to find but the programs themselves.
    const bool scan_leftovers = options.leftover_scan && !options.dry_run;
    const auto targets = scan_leftovers ? leftovers::capture_targets(programs) : std::vector<leftovers::ScanTarget>();

    ::SetConsoleCtrlHandler(on_console_control, TRUE);
    const auto batch = uninstall::run_batch(items, batch_options, events, g_cancellation);
    ::SetConsoleCtrlHandler(on_console_control, FALSE);

    const bool all_succeeded = std::all_of(batch.results.begin(), batch.results.end(),
                                           [](const auto& result) { return succeeded(result.outcome); });
    std::optional<leftovers::ScanResult> leftover_scan;
    if (scan_leftovers) {
        leftover_scan = leftovers::scan_after_uninstall(targets, batch.results);
    }
    if (options.json) {
        Json root;
        root["avel_version"] = kVersionString;
        root["elevated"] = context.elevated;
        root["dry_run"] = options.dry_run;
        root["mode"] = uninstall::to_string(mode);
        root["restore_point"] = {{"status", uninstall::to_string(batch.restore_point)},
                                 {"error", batch.restore_point_error}};
        root["results"] = Json::array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            root["results"].push_back(to_json(items[i], batch.results[i]));
        }
        root["success"] = all_succeeded;
        root["leftover_scan"] =
            leftover_scan ? leftovers_to_json(*leftover_scan, leftovers::Confidence::Bad) : Json(nullptr);
        print(root.dump(2) + "\n");
        return all_succeeded ? 0 : 1;
    }
    if (std::any_of(batch.results.begin(), batch.results.end(),
                    [](const auto& result) { return result.outcome == Outcome::RebootRequired; })) {
        print("Restart Windows to complete the removal.\n");
    }
    if (leftover_scan && !leftover_scan->leftovers.empty()) {
        print("\nLeft behind:\n");
        print_leftovers(*leftover_scan, leftovers::Confidence::Questionable);
    }
    return all_succeeded ? 0 : 1;
}

}  // namespace avel::cli
