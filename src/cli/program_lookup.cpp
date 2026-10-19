#include "cli/program_lookup.h"

#include "cli/console.h"
#include "core/platform/text.h"

namespace avel::cli {

using platform::to_utf8;

std::string name_of(const inventory::Program& program) {
    if (!program.display_name.empty()) {
        return to_utf8(program.display_name);
    }
    return program.registry ? "(" + to_utf8(program.registry->key_name) + ")" : to_utf8(program.id);
}

bool resolve(const inventory::Inventory& inventory, const std::vector<std::wstring>& queries,
             std::vector<const inventory::Program*>& found) {
    bool ok = true;
    for (const auto& query : queries) {
        std::vector<const inventory::Program*> matches;
        for (const auto& program : inventory.programs) {
            if (platform::iequals(program.id, query)) {
                matches = {&program};
                break;
            }
            if (platform::iequals(program.display_name, query)) {
                matches.push_back(&program);
            }
        }
        if (matches.size() == 1) {
            found.push_back(matches.front());
            continue;
        }
        ok = false;
        if (matches.empty()) {
            print_error("No installed program matches \"" + to_utf8(query) + "\". See `avel-cli list --all`.\n");
        } else {
            print_error("\"" + to_utf8(query) + "\" matches several programs; use an id instead:\n");
            for (const auto* match : matches) {
                print_error("  " + to_utf8(match->id) + "\n");
            }
        }
    }
    return ok;
}

}  // namespace avel::cli
