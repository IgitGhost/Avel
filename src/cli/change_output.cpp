#include "cli/change_output.h"

#include <windows.h>

#include <cstdlib>
#include <format>

#include "cli/console.h"
#include "cli/leftover_output.h"
#include "core/platform/elevation.h"
#include "core/platform/text.h"

namespace avel::cli {

namespace {

using Json = nlohmann::ordered_json;
using platform::to_utf8;

std::string status_text(const changes::ItemStatus& status) {
    std::string text(changes::to_string(status.state));
    if (status.reason != changes::Reason::None) {
        text += std::format(" ({})", changes::to_string(status.reason));
    }
    if (status.code != 0) {
        text += std::format(", error {}", status.code);
    }
    return text;
}

}  // namespace

std::unique_ptr<changes::ChangeStore> open_change_store() {
    if (!platform::is_process_elevated()) {
        print_error("Removing and undoing changes needs administrator rights; run avel-cli as administrator.\n");
        return nullptr;
    }
    platform::FolderStatus status{};
    auto store = changes::ChangeStore::open(changes::default_store_options(), status);
    if (!store) {
        print_error(std::format("error: the change store can't be trusted ({}); nothing was changed\n",
                                platform::to_string(status)));
        return nullptr;
    }
    if (const auto guard = store->lock()) {
        for (const auto& id : changes::recover(*store, cli_execute_options(false))) {
            print_error(std::format("note: settled change set {}, which an earlier run left unfinished\n", to_utf8(id)));
        }
        store->purge();
    }
    return store;
}

changes::ExecuteOptions cli_execute_options(bool allow_irreversible) {
    changes::ExecuteOptions options;
    options.allow_irreversible = allow_irreversible;
    wchar_t buffer[32] = {};
    if (::GetEnvironmentVariableW(L"AVEL_TEST_FAULT_AFTER", buffer, 32) > 0) {
        options.stop_after = static_cast<std::size_t>(std::wcstoul(buffer, nullptr, 10));
        options.on_stop = [] { ::ExitProcess(99); };
    }
    return options;
}

Json item_status_to_json(const changes::ItemStatus& status) {
    Json json;
    json["state"] = changes::to_string(status.state);
    json["reason"] = changes::to_string(status.reason);
    json["code"] = status.code;
    return json;
}

Json execute_result_to_json(const changes::ChangePlan& plan, const changes::ExecuteResult& result) {
    Json json;
    json["set_id"] = to_utf8(result.set_id);
    json["quarantined_bytes"] = result.quarantined_bytes;
    json["items"] = Json::array();
    for (std::size_t i = 0; i < plan.items.size() && i < result.items.size(); ++i) {
        const auto& item = result.items[i];
        Json entry = item_status_to_json(item.status);
        entry["index"] = i;
        entry["location"] = location_to_json(plan.items[i].location);
        entry["confidence"] = leftovers::to_string(plan.items[i].confidence);
        entry["left_behind"] = Json::array();
        for (const auto& file : item.left_behind) {
            entry["left_behind"].push_back(to_utf8(file));
        }
        entry["holders"] = Json::array();
        for (const auto& holder : item.holders) {
            entry["holders"].push_back({{"pid", holder.pid}, {"name", to_utf8(holder.name)}});
        }
        json["items"].push_back(std::move(entry));
    }
    return json;
}

void print_execute_result(const changes::ChangePlan& plan, const changes::ExecuteResult& result,
                          ResultWording wording) {
    const bool removal = wording == ResultWording::Removal;
    if (result.set_id.empty()) {
        print_error(removal ? "error: couldn't start a change set; nothing was removed\n"
                            : "error: couldn't start a change set; nothing was changed\n");
        return;
    }
    std::size_t removed = 0;
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        const auto& item = result.items[i];
        removed += item.status.state == changes::ItemState::Done || item.status.state == changes::ItemState::Partial;
        print(std::format("  {:<36} {:<15} {}\n", status_text(item.status), kind_of(plan.items[i].location),
                          to_utf8(describe_location(plan.items[i].location))));
        for (const auto& holder : item.holders) {
            print(std::format("      in use by {} (process {})\n", to_utf8(holder.name), holder.pid));
        }
    }
    if (removal) {
        print(std::format("\n{} of {} removed, {} bytes in quarantine. Undo with: avel-cli undo {}\n",
                          removed, plan.items.size(), result.quarantined_bytes, to_utf8(result.set_id)));
    } else {
        print(std::format("\n{} of {} changed. Undo with: avel-cli undo {}\n", removed, plan.items.size(),
                          to_utf8(result.set_id)));
    }
}

Json summary_to_json(const changes::SetSummary& summary) {
    Json json;
    json["id"] = to_utf8(summary.id);
    json["created"] = std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(summary.created));
    json["description"] = to_utf8(summary.description);
    json["state"] = changes::to_string(summary.state);
    json["items"] = summary.items;
    json["done"] = summary.done;
    json["skipped"] = summary.skipped;
    json["restored"] = summary.restored;
    json["quarantined_bytes"] = summary.quarantined_bytes;
    return json;
}

}  // namespace avel::cli
