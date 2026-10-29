#include "cli/scan_command.h"

#include <nlohmann/json.hpp>

#include <format>
#include <string>

#include "cli/command_options.h"
#include "cli/console.h"
#include "cli/leftover_output.h"
#include "cli/program_lookup.h"
#include "cli/trace_use.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/scan.h"
#include "core/platform/elevation.h"
#include "core/platform/text.h"
#include "core/tracker/trace_source.h"
#include "core/version.h"

namespace avel::cli {

namespace {

using leftovers::Confidence;
using platform::to_utf8;
using Json = nlohmann::ordered_json;

constexpr std::string_view kUsage =
    "Usage: avel-cli scan <program>... | --all [--deep] [--json] [--min-level <level>] [--all-levels] [--timing]\n"
    "                     [--no-trace]\n"
    "  <program>            an id from `avel-cli list --json`, or an exact display name\n"
    "  --all                every program Settings > Apps lists\n"
    "  --deep               also search every fixed drive for folders named after them (administrator)\n"
    "  --json               machine-readable output\n"
    "  --min-level <level>  questionable (default), good or very-good\n"
    "  --all-levels         also show unlikely matches (confidence \"bad\")\n"
    "  --timing             show how long each stage took\n"
    "  --no-trace           leave out what the programs' install traces recorded\n"
    "Shows what the programs would leave behind if their uninstallers removed nothing. With\n"
    "administrator rights, a program's install trace adds what it saw the installer create.\n"
    "Read-only: nothing is changed. Run as administrator to include other users' data.\n";

}  // namespace

int run_scan_command(const std::vector<std::wstring_view>& arguments) {
    ScanCommandOptions options;
    if (const auto error = parse_scan_options(arguments, options)) {
        print_error(error->message + (error->usage ? std::string(kUsage) : std::string()));
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

    // A program's install trace shows what its installer created (the trace store is
    // administrators' only, so an unelevated scan doesn't see it).
    std::unique_ptr<tracker::TraceStore> traces;
    std::optional<tracker::StoreTraceSource> trace_source;
    if (options.trace && platform::is_process_elevated() && (traces = open_trace_store_for("scan"))) {
        trace_source.emplace(*traces, inventory);
    }
    std::vector<leftovers::ScanTarget> targets;
    for (const auto* program : programs) {
        targets.push_back(leftovers::make_scan_target(*program));
        if (trace_source) {
            trace_source->add_trace(*program, targets.back());
        }
    }
    const leftovers::LiveEnvironment environment;
    const auto result =
        leftovers::scan_leftovers(environment.environment(), targets, inventory.programs, {.deep = options.deep});

    if (options.json) {
        Json root;
        root["avel_version"] = kVersionString;
        root["elevated"] = platform::is_process_elevated();
        Json scanned = Json::array();
        for (std::size_t i = 0; i < programs.size(); ++i) {
            scanned.push_back({{"id", to_utf8(programs[i]->id)},
                               {"name", to_utf8(programs[i]->display_name)},
                               {"trace", trace_id_json(targets[i])}});
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
