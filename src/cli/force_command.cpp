#include "cli/force_command.h"

#include <nlohmann/json.hpp>

#include <format>
#include <iostream>
#include <optional>
#include <string>

#include "cli/change_output.h"
#include "cli/console.h"
#include "cli/leftover_output.h"
#include "cli/program_lookup.h"
#include "core/changes/change_set.h"
#include "core/force/force.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/scan.h"
#include "core/platform/elevation.h"
#include "core/platform/processes.h"
#include "core/platform/text.h"
#include "core/version.h"

namespace avel::cli {

namespace {

using leftovers::Confidence;
using platform::to_utf8;
using Json = nlohmann::ordered_json;

constexpr std::string_view kUsage =
    "Usage: avel-cli force <program>... | --folder <path> [--name <name>] | --exe <path>\n"
    "                      [--dry-run] [--yes] [--end-processes] [--min-level <level>]\n"
    "                      [--allow-irreversible] [--json]\n"
    "  <program>             an id from `avel-cli list --json`, or an exact display name\n"
    "  --folder <path>       a folder no Uninstall entry describes; --name is what its other\n"
    "                        leftovers are named after (the folder's name by default)\n"
    "  --exe <path>          a program file: the installed program it belongs to, or else its folder\n"
    "  --dry-run             show what would be removed, change nothing\n"
    "  --yes                 don't ask for confirmation\n"
    "  --end-processes       end the programs' running processes (needed with --yes when any run)\n"
    "  --min-level <level>   the least likely items to remove: good (default) or very-good\n"
    "  --allow-irreversible  also remove items undo can't put back (packages)\n"
    "  --json                machine-readable output (needs --yes or --dry-run)\n"
    "Removes programs whose uninstaller is missing or broken without running it: ends their\n"
    "running processes once you agree, then removes their folder, their Uninstall entry, Windows\n"
    "Installer's registration and what they left elsewhere, backed up so `avel-cli undo` can put\n"
    "it back. Run as administrator.\n";

struct Options {
    std::vector<std::wstring> programs;
    std::wstring folder;
    std::wstring name;
    std::wstring exe;
    bool dry_run = false;
    bool yes = false;
    bool end_processes = false;
    Confidence min_level = Confidence::Good;
    bool allow_irreversible = false;
    bool json = false;
};

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
    Options options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        const bool has_value = i + 1 < arguments.size();
        if (argument == L"--folder" && has_value) {
            options.folder = arguments[++i];
        } else if (argument == L"--name" && has_value) {
            options.name = arguments[++i];
        } else if (argument == L"--exe" && has_value) {
            options.exe = arguments[++i];
        } else if (argument == L"--dry-run") {
            options.dry_run = true;
        } else if (argument == L"--yes" || argument == L"-y") {
            options.yes = true;
        } else if (argument == L"--end-processes") {
            options.end_processes = true;
        } else if (argument == L"--allow-irreversible") {
            options.allow_irreversible = true;
        } else if (argument == L"--json") {
            options.json = true;
        } else if (argument == L"--min-level" && has_value) {
            const auto level = arguments[++i];
            if (level == L"good") {
                options.min_level = Confidence::Good;
            } else if (level == L"very-good") {
                options.min_level = Confidence::VeryGood;
            } else {
                print_error("force: --min-level is good or very-good\n");
                return 2;
            }
        } else if (argument.starts_with(L"--")) {
            print_error("force: unknown option " + to_utf8(argument) + "\n" + std::string(kUsage));
            return 2;
        } else {
            options.programs.emplace_back(argument);
        }
    }
    const int sources = !options.programs.empty() + !options.folder.empty() + !options.exe.empty();
    if (sources != 1 || (!options.name.empty() && options.folder.empty())) {
        print_error(kUsage);
        return 2;
    }
    if (options.json && !options.yes && !options.dry_run) {
        print_error("force: --json needs --yes or --dry-run (it can't ask for confirmation)\n");
        return 2;
    }
    std::unique_ptr<changes::ChangeStore> store;
    if (!options.dry_run && !(store = open_change_store())) {
        return 1;
    }

    const auto inventory = inventory::scan_inventory();
    const leftovers::LiveEnvironment live;
    const auto& environment = live.environment();

    std::vector<leftovers::ScanTarget> targets;
    const auto add_program = [&](const inventory::Program& program) {
        if (program.source == inventory::Source::Appx) {
            print_error(std::format("force: {} is a packaged app; remove it with `avel-cli uninstall`\n",
                                    name_of(program)));
            return false;
        }
        targets.push_back(leftovers::make_scan_target(program));
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
    const auto scan = leftovers::scan_leftovers(environment, targets, inventory.programs);
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

    if (options.json) {
        Json root;
        root["avel_version"] = kVersionString;
        root["elevated"] = platform::is_process_elevated();
        root["dry_run"] = options.dry_run;
        Json scanned = Json::array();
        for (const auto& target : targets) {
            scanned.push_back({{"id", to_utf8(target.program_id)},
                               {"name", to_utf8(target.display_name)},
                               {"install_location", to_utf8(target.install_location)}});
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
