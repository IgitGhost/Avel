#include "cli/uninstall_command.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <format>
#include <iostream>
#include <optional>
#include <string>

#include "cli/change_output.h"
#include "cli/command_options.h"
#include "cli/console.h"
#include "cli/leftover_output.h"
#include "cli/program_lookup.h"
#include "cli/trace_use.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/post_uninstall.h"
#include "core/platform/elevation.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "core/tracker/trace_source.h"
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
    "                          [--no-leftover-scan | --remove-leftovers [--min-level <level>]\n"
    "                          [--allow-irreversible]] [--no-trace] [--deep] [--json]\n"
    "  <program>             an id from `avel-cli list --json`, or an exact display name\n"
    "  --quiet               use silent uninstall switches where known (interactive otherwise)\n"
    "  --dry-run             show what would run, change nothing\n"
    "  --yes                 don't ask for confirmation\n"
    "  --no-restore-point    skip the System Restore point\n"
    "  --no-leftover-scan    don't look for what the uninstallers left behind\n"
    "  --remove-leftovers    remove what they left behind, backed up so `avel-cli undo` can put\n"
    "                        it back\n"
    "  --min-level <level>   the least likely leftovers to remove: good (default) or very-good\n"
    "  --allow-irreversible  also remove leftovers undo can't put back (packages)\n"
    "  --no-trace            don't use the programs' install traces (by default, what a trace saw\n"
    "                        the installer create, and its edits to Path and SharedDLLs, join the\n"
    "                        leftovers)\n"
    "  --deep                also search every fixed drive for folders named after the programs\n"
    "  --json                machine-readable output (needs --yes or --dry-run)\n";

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
    UninstallCommandOptions options;
    if (const auto error = parse_uninstall_options(arguments, options)) {
        print_error(error->message + (error->usage ? std::string(kUsage) : std::string()));
        return 2;
    }
    // Checked before anything is uninstalled: removing leftovers needs a trustworthy change store.
    std::unique_ptr<changes::ChangeStore> store;
    if (options.remove_leftovers && !(store = open_change_store())) {
        return 1;
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

    // Uninstalling by the trace, the default mode: each program's uninstaller runs, then what its
    // install trace saw the installer create joins the leftovers (docs/trace.md). The trace store is
    // administrators' only, so an unelevated run doesn't see it.
    const bool scan_leftovers = options.leftover_scan && !options.dry_run;
    std::unique_ptr<tracker::TraceStore> traces;
    std::optional<tracker::StoreTraceSource> trace_source;
    if (options.trace && options.leftover_scan && context.elevated &&
        (traces = open_trace_store_for("uninstall"))) {
        trace_source.emplace(*traces, inventory);
    }
    std::vector<std::optional<std::wstring>> trace_ids;
    for (const auto* program : programs) {
        trace_ids.push_back(trace_source ? trace_source->trace_of(*program) : std::nullopt);
    }

    if (!options.json) {
        print(options.dry_run ? "Dry run, nothing will be changed:\n" : "Plan:\n");
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            print(std::format("  {}\n      {}\n", name_of(*item.program), describe_plan(item.plan)));
            if (trace_ids[i]) {
                print(std::format("      install trace {} (its items join the leftover scan)\n",
                                  to_utf8(*trace_ids[i])));
            }
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
    // The traces' items are tied to their programs now, before the uninstallers run.
    leftovers::TraceSource* const source = trace_source ? &*trace_source : nullptr;
    const auto targets =
        scan_leftovers ? leftovers::capture_targets(programs, source) : std::vector<leftovers::ScanTarget>();

    ::SetConsoleCtrlHandler(on_console_control, TRUE);
    const auto batch = uninstall::run_batch(items, batch_options, events, g_cancellation);
    ::SetConsoleCtrlHandler(on_console_control, FALSE);

    const bool all_succeeded = std::all_of(batch.results.begin(), batch.results.end(),
                                           [](const auto& result) { return succeeded(result.outcome); });
    std::optional<leftovers::ScanResult> leftover_scan;
    if (scan_leftovers) {
        leftover_scan = leftovers::scan_after_uninstall(targets, batch.results, {.deep = options.deep});
    }
    std::optional<changes::ChangePlan> removal_plan;
    std::optional<changes::ExecuteResult> removal;
    if (store && leftover_scan && !leftover_scan->leftovers.empty()) {
        const auto program_name = [&](const std::wstring& id) {
            for (const auto& target : targets) {
                if (target.program_id == id) {
                    return target.display_name;
                }
            }
            return id;
        };
        removal_plan = changes::make_plan(leftover_scan->leftovers, options.min_level, program_name,
                                          programs.size() == 1 ? L"Leftovers of " + programs.front()->display_name
                                                               : std::format(L"Leftovers of {} programs", programs.size()));
        if (!removal_plan->items.empty()) {
            if (const auto guard = store->lock()) {
                removal = changes::execute(*store, *removal_plan, cli_execute_options(options.allow_irreversible));
            } else {
                print_error("warning: another Avel process is changing the system; leftovers were left in place\n");
            }
        }
    }
    // The uninstalled programs' traces are kept 30 days from now.
    if (traces && !options.dry_run) {
        apply_trace_retention(*traces);
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
            Json result = to_json(items[i], batch.results[i]);
            // The install trace whose items joined the leftover scan (for a dry run, would join).
            result["trace"] = i < targets.size() ? trace_id_json(targets[i])
                              : trace_ids[i]     ? Json(to_utf8(*trace_ids[i]))
                                                 : Json(nullptr);
            root["results"].push_back(std::move(result));
        }
        root["success"] = all_succeeded;
        root["leftover_scan"] =
            leftover_scan ? leftovers_to_json(*leftover_scan, leftovers::Confidence::Bad) : Json(nullptr);
        root["leftover_removal"] = removal ? execute_result_to_json(*removal_plan, *removal) : Json(nullptr);
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
    if (removal) {
        print("\nRemoving leftovers:\n");
        print_execute_result(*removal_plan, *removal);
    }
    return all_succeeded ? 0 : 1;
}

}  // namespace avel::cli
