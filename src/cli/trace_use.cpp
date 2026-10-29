#include "cli/trace_use.h"

#include <windows.h>

#include <format>

#include "cli/console.h"
#include "core/inventory/inventory.h"
#include "core/platform/secure_folder.h"
#include "core/platform/text.h"
#include "core/tracker/linking.h"

namespace avel::cli {

std::unique_ptr<tracker::TraceStore> open_trace_store_for(std::string_view command) {
    auto options = tracker::default_trace_store_options();
    if (::GetFileAttributesW(options.root.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return nullptr;  // No trace was ever taken here
    }
    platform::FolderStatus status{};
    auto store = tracker::TraceStore::open(std::move(options), status);
    if (!store) {
        print_error(
            std::format("warning: {}: the trace store can't be trusted ({}); install traces aren't used\n",
                        command, platform::to_string(status)));
    }
    return store;
}

void apply_trace_retention(tracker::TraceStore& store) {
    inventory::ScanOptions scan;
    scan.appx = false;  // Avel doesn't trace packaged apps
    const auto inventory = inventory::scan_inventory(scan);
    if (!inventory.programs.empty()) {
        store.purge(
            [&inventory](tracker::TraceInfo& info) { return tracker::refresh_links(info, inventory); });
    }
}

nlohmann::ordered_json trace_id_json(const leftovers::ScanTarget& target) {
    return target.trace_id.empty() ? nlohmann::ordered_json(nullptr)
                                   : nlohmann::ordered_json(platform::to_utf8(target.trace_id));
}

}  // namespace avel::cli
