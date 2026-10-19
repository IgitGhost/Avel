#include "cli/scan_command.h"

#include <nlohmann/json.hpp>

#include <format>
#include <string>

#include "cli/console.h"
#include "cli/leftover_output.h"
#include "cli/program_lookup.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/scan.h"
#include "core/platform/elevation.h"
#include "core/platform/text.h"
#include "core/version.h"

namespace avel::cli {

namespace {

using leftovers::Confidence;
using platform::to_utf8;
using Json = nlohmann::ordered_json;

constexpr std::string_view kUsage =
    "Usage: avel-cli scan <program>... | --all [--json] [--min-level <level>] [--all-levels] [--timing]\n"
    "  <program>            an id from `avel-cli list --json`, or an exact display name\n"
    "  --all                every program Settings > Apps lists\n"
    "  --json               machine-readable output\n"
    "  --min-level <level>  questionable (default), good or very-good\n"
    "  --all-levels         also show unlikely matches (confidence \"bad\")\n"
    "  --timing             show how long each stage took\n"
    "Shows what the programs would leave behind if their uninstallers removed nothing.\n"
    "Read-only: nothing is changed. Run as administrator to include other users' data.\n";

struct Options {
    std::vector<std::wstring> programs;
    bool all = false;
    bool json = false;
    bool timing = false;
    Confidence min_level = Confidence::Questionable;
};

bool parse_level(std::wstring_view text, Confidence& level) {
    if (text == L"questionable") {
        level = Confidence::Questionable;
    } else if (text == L"good") {
        level = Confidence::Good;
    } else if (text == L"very-good") {
        level = Confidence::VeryGood;
    } else {
        return false;
    }
    return true;
}

}  // namespace

int run_scan_command(const std::vector<std::wstring_view>& arguments) {
    Options options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        if (argument == L"--all") {
            options.all = true;
        } else if (argument == L"--json") {
            options.json = true;
        } else if (argument == L"--timing") {
            options.timing = true;
        } else if (argument == L"--all-levels") {
            options.min_level = Confidence::Bad;
        } else if (argument == L"--min-level" && i + 1 < arguments.size() &&
                   parse_level(arguments[i + 1], options.min_level)) {
            ++i;
        } else if (argument.starts_with(L"--")) {
            print_error(kUsage);
            return 2;
        } else {
            options.programs.emplace_back(argument);
        }
    }
    if (options.all == !options.programs.empty()) {
        print_error(kUsage);
        return 2;
    }

    const auto inventory = inventory::scan_inventory();
    std::vector<const inventory::Program*> programs;
    if (options.all) {
        for (const auto& program : inventory.programs) {
            if (inventory::is_listed_in_settings(program)) {
                programs.push_back(&program);
            }
        }
    } else if (!resolve(inventory, options.programs, programs)) {
        return 2;
    }

    std::vector<leftovers::ScanTarget> targets;
    for (const auto* program : programs) {
        targets.push_back(leftovers::make_scan_target(*program));
    }
    const leftovers::LiveEnvironment environment;
    const auto result = leftovers::scan_leftovers(environment.environment(), targets, inventory.programs);

    if (options.json) {
        Json root;
        root["avel_version"] = kVersionString;
        root["elevated"] = platform::is_process_elevated();
        Json scanned = Json::array();
        for (const auto* program : programs) {
            scanned.push_back({{"id", to_utf8(program->id)}, {"name", to_utf8(program->display_name)}});
        }
        root["programs"] = std::move(scanned);
        root.update(leftovers_to_json(result, options.min_level));
        print(root.dump(2) + "\n");
        return 0;
    }
    if (programs.size() == 1) {
        print(std::format("Leftovers of {} if its uninstaller removed nothing:\n\n", name_of(*programs.front())));
    } else {
        print(std::format("Leftovers of {} programs if their uninstallers removed nothing:\n\n", programs.size()));
    }
    print_leftovers(result, options.min_level);
    if (options.timing) {
        for (const auto& timing : result.timings) {
            print(std::format("  {:<12} {:>6} ms\n", timing.stage, timing.elapsed.count()));
        }
    }
    return 0;
}

}  // namespace avel::cli
