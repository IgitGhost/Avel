#include "cli/force_command.h"

#include <nlohmann/json.hpp>

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
#include "core/changes/change_set.h"
#include "core/force/force.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/scan.h"
#include "core/platform/elevation.h"
#include "core/platform/processes.h"
#include "core/platform/text.h"
#include "core/tracker/trace_source.h"
#include "core/version.h"

namespace avel::cli {

namespace {

using leftovers::Confidence;
using platform::to_utf8;
using Json = nlohmann::ordered_json;

constexpr std::string_view kUsage =
    "Usage: avel-cli force <program>... | --folder <path> [--name <name>] | --exe <path> | --trace <id>\n"
    "                      [--dry-run] [--yes] [--end-processes] [--deep] [--min-level <level>]\n"
    "                      [--allow-irreversible] [--no-trace] [--json]\n"
    "  <program>             an id from `avel-cli list --json`, or an exact display name\n"
    "  --folder <path>       a folder no Uninstall entry describes; --name is what its other\n"
    "                        leftovers are named after (the folder's name by default)\n"
    "  --exe <path>          a program file: the installed program it belongs to, or else its folder\n"
    "  --trace <id>          revert what an install trace recorded (`avel-cli trace list`), also\n"
    "                        when its program's entry is gone or the trace was imported\n"
    "  --dry-run             show what would be removed, change nothing\n"
    "  --yes                 don't ask for confirmation\n"
    "  --end-processes       end the programs' running processes (needed with --yes when any run)\n"
    "  --deep                also search every fixed drive for folders named after the programs;\n"
    "                        what it finds alone is questionable and only shown\n"
    "  --min-level <level>   the least likely items to remove: good (default) or very-good; what\n"
    "                        a trace's review excludes is never removed this way\n"
    "  --allow-irreversible  also remove items undo can't put back (packages)\n"
    "  --no-trace            don't use a program's install trace\n"
    "  --json                machine-readable output (needs --yes or --dry-run)\n"
    "Removes programs whose uninstaller is missing or broken without running it: ends their\n"
    "running processes once you agree, then removes their folder, their Uninstall entry, Windows\n"
    "Installer's registration and what they left elsewhere, backed up so `avel-cli undo` can put\n"
    "it back. A program that was installed with a trace is reverted by it: what the installer\n"
    "created, and its edits to Path and SharedDLLs. Run as administrator.\n";

struct StoppedProcess {
    platform::ProcessInfo process;
    std::optional<platform::StopResult> result;
};

bool ask(const std::string& question) {
    print(question + " [y/N] ");
    std::string answer;
    std::getline(std::cin, answer);
    return !answer.empty() && (answer.front() == 'y' || answer.front() == 'Y');
}

std::string folder_error_text(const force::FolderTarget& target) {
    if (target.error == force::FolderError::Protected) {
        return std::format("it is protected ({})", leftovers::to_string(target.protection));
    }
    return std::string(force::to_string(target.error));
}

// Done, or left to go with the folder or key that contains it.
bool removed(const changes::ItemStatus& status) {
    return status.state == changes::ItemState::Done ||
           (status.state == changes::ItemState::Skipped && status.reason == changes::Reason::Contained);
}

}  // namespace

int run_force_command(const std::vector<std::wstring_view>& arguments) {
    ForceCommandOptions options;
    if (const auto error = parse_force_options(arguments, options)) {
        print_error(error->message + (error->usage ? std::string(kUsage) : std::string()));
        return 2;
    }
    const bool elevated = platform::is_process_elevated();
    if (!options.trace_id.empty() && !elevated) {
        print_error("force: --trace needs administrator rights (the trace store is administrators' only)\n");
        return 1;
    }
    std::unique_ptr<changes::ChangeStore> store;
    if (!options.dry_run && !(store = open_change_store())) {
        return 1;
    }

    const auto inventory = inventory::scan_inventory();
    const leftovers::LiveEnvironment live;
    const auto& environment = live.environment();

    // Reverting by the trace (docs/force.md): what a program's install trace saw the installer
    // create, and its value edits, join the plan. The trace store is administrators' only.
    std::unique_ptr<tracker::TraceStore> traces;
    std::optional<tracker::StoreTraceSource> trace_source;
    if (options.trace && elevated && (traces = open_trace_store_for("force"))) {
        trace_source.emplace(*traces, inventory);
    }

    std::vector<leftovers::ScanTarget> targets;
    const auto add_program = [&](const inventory::Program& program) {
        if (program.source == inventory::Source::Appx) {
            print_error(std::format("force: {} is a packaged app; remove it with `avel-cli uninstall`\n",
                                    name_of(program)));
            return false;
        }
        targets.push_back(leftovers::make_scan_target(program));
        if (trace_source) {
            trace_source->add_trace(program, targets.back());
        }
        return true;
    };
    const auto add_folder = [&](const std::wstring& folder, const std::wstring& name,
                                const std::wstring& publisher) {
        auto target = force::folder_target(environment, folder, name, publisher);
        if (target.error != force::FolderError::None) {
            print_error(
                std::format("force: can't remove {}: {}\n", to_utf8(folder), folder_error_text(target)));
            return false;
        }
        targets.push_back(std::move(target.target));
        return true;
    };
    if (!options.programs.empty()) {
        std::vector<const inventory::Program*> programs;
        if (!resolve(inventory, options.programs, programs)) {
            return 2;
        }
        for (const auto* program : programs) {
            if (!add_program(*program)) {
                return 2;
            }
        }
    } else if (!options.folder.empty()) {
        if (!add_folder(options.folder, options.name, {})) {
            return 2;
        }
    } else if (!options.trace_id.empty()) {
        const std::string id = to_utf8(options.trace_id);
        if (!trace_source) {
            print_error(std::format("force: no install trace {} (nothing was ever traced here)\n", id));
            return 2;
        }
        tracker::TraceError error = tracker::TraceError::None;
        auto target = trace_source->trace_target(options.trace_id, error);
        if (!target) {
            switch (error) {
                case tracker::TraceError::NotFound:
                    print_error(std::format("force: no install trace {}; see `avel-cli trace list`\n", id));
                    return 2;
                case tracker::TraceError::NotFinished:
                    print_error(std::format(
                        "force: trace {} isn't finished; `avel-cli trace stop` finishes it\n", id));
                    return 2;
                default:
                    print_error(std::format("force: the result of trace {} can't be read\n", id));
                    return 1;
            }
        }
        targets.push_back(std::move(*target));
    } else {
        const auto owner = force::owner_of(environment, inventory.programs, options.exe);
        if (owner.program) {
            if (!options.json) {
                print(std::format("{} belongs to {} ({}).\n", to_utf8(options.exe), name_of(*owner.program),
                                  to_utf8(owner.program->id)));
            }
            if (!add_program(*owner.program)) {
                return 2;
            }
        } else if (!add_folder(owner.folder, owner.name, owner.publisher)) {
            return 2;
        }
    }

    // The scan's targets know an MSI program's folder even when its entry records none.
    const auto scan =
        leftovers::scan_leftovers(environment, targets, inventory.programs, {.deep = options.deep});
    targets = scan.targets;
    std::vector<StoppedProcess> processes;
    for (auto& process : force::processes_in(environment, targets, platform::running_processes(),
                                             platform::service_process_ids())) {
        processes.push_back({std::move(process), std::nullopt});
    }
    const auto program_name = [&](const std::wstring& id) {
        for (const auto& target : targets) {
            if (target.program_id == id) {
                return target.display_name;
            }
        }
        return id;
    };
    const std::wstring description = targets.size() == 1
                                         ? L"Forced removal of " + targets.front().display_name
                                         : std::format(L"Forced removal of {} programs", targets.size());
    const auto plan = changes::make_plan(scan.leftovers, options.min_level, program_name, description);

    if (!options.json) {
        print(options.dry_run ? "Dry run, nothing will be changed.\n" : "");
        for (const auto& target : targets) {
            print(std::format(
                "Forced removal of {}{}\n", to_utf8(target.display_name),
                target.install_location.empty() ? "" : " (" + to_utf8(target.install_location) + ")"));
            if (!target.trace_id.empty()) {
                print(std::format("  reverted by install trace {}\n", to_utf8(target.trace_id)));
            }
        }
        if (!processes.empty()) {
            print("\nRunning from its folder:\n");
            for (const auto& [process, result] : processes) {
                print(std::format("  {:>6}  {}\n", process.pid, to_utf8(process.image)));
            }
        }
        print("\nFound:\n");
        print_leftovers(scan, Confidence::Questionable);
        print(std::format("\n{} item{} at or above {} will be removed.\n", plan.items.size(),
                          plan.items.size() == 1 ? "" : "s", leftovers::to_string(options.min_level)));
        print_held_back(scan, options.min_level);
    }

    std::optional<changes::ExecuteResult> removal;
    int exit_code = 0;
    const bool act = !options.dry_run && !plan.items.empty();
    if (act && options.yes && !processes.empty() && !options.end_processes) {
        print_error(
            std::format("force: {} process{} running from the program's folder; close {} or pass "
                        "--end-processes\n",
                        processes.size(), processes.size() == 1 ? " is" : "es are",
                        processes.size() == 1 ? "it" : "them"));
        exit_code = 1;
    } else if (act) {
        const std::string question =
            processes.empty()
                ? std::format("Remove {} item{}?", plan.items.size(), plan.items.size() == 1 ? "" : "s")
                : std::format("End {} process{} and remove {} item{}?", processes.size(),
                              processes.size() == 1 ? "" : "es", plan.items.size(),
                              plan.items.size() == 1 ? "" : "s");
        if (!options.yes && !ask("\n" + question)) {
            print("Cancelled.\n");
            return 1;
        }
        for (auto& [process, result] : processes) {
            result = platform::stop_process(process.pid);
            if (!options.json) {
                print(std::format("Ending {} ({}) ... {}\n", to_utf8(process.name), process.pid,
                                  platform::to_string(result->outcome)));
            }
        }
        if (const auto guard = store->lock()) {
            removal = changes::execute(*store, plan, cli_execute_options(options.allow_irreversible));
            if (!std::all_of(removal->items.begin(), removal->items.end(),
                             [](const auto& item) { return removed(item.status); })) {
                exit_code = 1;
            }
        } else {
            print_error("force: another Avel process is changing the system; nothing was removed\n");
            exit_code = 1;
        }
    } else if (!options.dry_run && !options.json) {
        print("Nothing to remove.\n");
    }
    // The removed programs' traces are kept 30 days from now.
    if (traces && removal) {
        apply_trace_retention(*traces);
    }

    if (options.json) {
        Json root;
        root["avel_version"] = kVersionString;
        root["elevated"] = elevated;
        root["dry_run"] = options.dry_run;
        Json scanned = Json::array();
        for (const auto& target : targets) {
            scanned.push_back({{"id", to_utf8(target.program_id)},
                               {"name", to_utf8(target.display_name)},
                               {"install_location", to_utf8(target.install_location)},
                               {"trace", trace_id_json(target)}});
        }
        root["programs"] = std::move(scanned);
        Json running = Json::array();
        for (const auto& [process, result] : processes) {
            running.push_back(
                {{"pid", process.pid},
                 {"name", to_utf8(process.name)},
                 {"image", to_utf8(process.image)},
                 {"outcome", result ? Json(platform::to_string(result->outcome)) : Json(nullptr)},
                 {"error", result ? result->error : 0}});
        }
        root["processes"] = std::move(running);
        root["leftover_scan"] = leftovers_to_json(scan, Confidence::Bad);
        root["leftover_removal"] = removal ? execute_result_to_json(plan, *removal) : Json(nullptr);
        print(root.dump(2) + "\n");
        return exit_code;
    }
    if (removal) {
        print("\nRemoving:\n");
        print_execute_result(plan, *removal);
    }
    return exit_code;
}

}  // namespace avel::cli
