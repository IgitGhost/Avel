#include "cli/orphans_command.h"

#include <nlohmann/json.hpp>

#include <format>
#include <string>

#include "cli/console.h"
#include "cli/leftover_output.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/orphans.h"
#include "core/platform/elevation.h"
#include "core/version.h"

namespace avel::cli {

namespace {

using leftovers::Confidence;
using Json = nlohmann::ordered_json;

constexpr std::string_view kUsage =
    "Usage: avel-cli orphans [--json] [--all-levels] [--timing]\n"
    "  --json        machine-readable output\n"
    "  --all-levels  also show other users' folders\n"
    "  --timing      show how long each stage took\n"
    "Shows the folders in Program Files, ProgramData and the users' AppData\\Local\\Programs that\n"
    "no installed program owns. Read-only: remove one with `avel-cli force --folder <path>`.\n";

}  // namespace

int run_orphans_command(const std::vector<std::wstring_view>& arguments) {
    bool json = false;
    bool timing = false;
    Confidence min_level = Confidence::Questionable;
    for (const auto argument : arguments) {
        if (argument == L"--json") {
            json = true;
        } else if (argument == L"--all-levels") {
            min_level = Confidence::Bad;
        } else if (argument == L"--timing") {
            timing = true;
        } else {
            print_error(kUsage);
            return 2;
        }
    }

    const auto inventory = inventory::scan_inventory();
    const leftovers::LiveEnvironment environment;
    const auto result = leftovers::find_orphans(environment.environment(), inventory.programs);

    if (json) {
        Json root;
        root["avel_version"] = kVersionString;
        root["elevated"] = platform::is_process_elevated();
        root.update(leftovers_to_json(result, min_level));
        print(root.dump(2) + "\n");
        return 0;
    }
    print("Folders no installed program owns:\n\n");
    print_leftovers(result, min_level);
    if (timing) {
        for (const auto& stage : result.timings) {
            print(std::format("  {:<12} {:>6} ms\n", stage.stage, stage.elapsed.count()));
        }
    }
    return 0;
}

}  // namespace avel::cli
