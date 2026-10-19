#include "cli/undo_command.h"

#include <nlohmann/json.hpp>

#include <cwchar>
#include <format>
#include <optional>
#include <string>

#include "cli/change_output.h"
#include "cli/console.h"
#include "cli/leftover_output.h"
#include "core/platform/text.h"

namespace avel::cli {

namespace {

using Json = nlohmann::ordered_json;
using platform::to_utf8;

constexpr std::string_view kUsage =
    "Usage: avel-cli undo --list | --show <id> | <id> [--item <n>]... | --purge | --recover [--json]\n"
    "  --list        the change sets kept (30 days), newest first\n"
    "  --show <id>   what a set removed, item by item\n"
    "  <id>          put back what the set removed; --item limits it to some items\n"
    "  --purge       delete sets older than 30 days, with their backups\n"
    "  --recover     settle sets an interrupted run left unfinished (also done on every run)\n"
    "  --json        machine-readable output\n"
    "Run as administrator.\n";

enum class Action { None, List, Show, Restore, Purge, Recover };

}  // namespace

int run_undo_command(const std::vector<std::wstring_view>& arguments) {
    Action action = Action::None;
    std::wstring id;
    std::vector<std::size_t> items;
    bool json = false;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        if (argument == L"--list") {
            action = Action::List;
        } else if (argument == L"--show" && i + 1 < arguments.size()) {
            action = Action::Show;
            id = arguments[++i];
        } else if (argument == L"--item" && i + 1 < arguments.size()) {
            items.push_back(std::wcstoul(std::wstring(arguments[++i]).c_str(), nullptr, 10));
        } else if (argument == L"--purge") {
            action = Action::Purge;
        } else if (argument == L"--recover") {
            action = Action::Recover;
        } else if (argument == L"--json") {
            json = true;
        } else if (!argument.starts_with(L"--") && id.empty()) {
            action = Action::Restore;
            id = argument;
        } else {
            print_error("undo: unexpected argument " + to_utf8(argument) + "\n" + std::string(kUsage));
            return 2;
        }
    }
    if (action == Action::None) {
        print_error(kUsage);
        return 2;
    }

    const auto store = open_change_store();  // Also recovers and purges
    if (!store) {
        return 1;
    }
    const auto guard = store->lock();
    if (!guard) {
        print_error("undo: another Avel process is changing the system; try again when it's done\n");
        return 1;
    }

    switch (action) {
        case Action::List: {
            const auto sets = store->list();
            if (json) {
                Json root = Json::array();
                for (const auto& set : sets) {
                    root.push_back(summary_to_json(set));
                }
                print(root.dump(2) + "\n");
            } else if (sets.empty()) {
                print("No change sets.\n");
            } else {
                for (const auto& set : sets) {
                    print(std::format("{}  {:<15} {:>3} removed, {:>3} restored  {}\n", to_utf8(set.id),
                                      changes::to_string(set.state), set.done, set.restored, to_utf8(set.description)));
                }
            }
            return 0;
        }
        case Action::Show: {
            const auto set = store->open_set(id);
            if (!set) {
                print_error("undo: no such change set: " + to_utf8(id) + "\n");
                return 1;
            }
            const auto& plan = set->plan();
            if (json) {
                Json root = summary_to_json(set->summary().value_or(changes::SetSummary{set->id()}));
                root["items"] = Json::array();
                for (std::size_t i = 0; i < plan.items.size(); ++i) {
                    Json entry = item_status_to_json(set->statuses()[i]);
                    entry["index"] = i;
                    entry["location"] = location_to_json(plan.items[i].location);
                    root["items"].push_back(std::move(entry));
                }
                print(root.dump(2) + "\n");
            } else {
                print(to_utf8(plan.description) + "\n");
                for (std::size_t i = 0; i < plan.items.size(); ++i) {
                    print(std::format("  {:>3}  {:<16} {:<15} {}\n", i, changes::to_string(set->statuses()[i].state),
                                      kind_of(plan.items[i].location), to_utf8(describe_location(plan.items[i].location))));
                }
            }
            return 0;
        }
        case Action::Restore: {
            const auto result = changes::restore(*store, id, items, cli_execute_options(false));
            if (!result.found) {
                print_error("undo: no such change set, or it's in use: " + to_utf8(id) + "\n");
                return 1;
            }
            bool all_restored = true;
            for (const auto& [index, item] : result.items) {
                all_restored = all_restored && item.status.state == changes::ItemState::Restored;
            }
            if (json) {
                Json root;
                root["set_id"] = to_utf8(id);
                root["state"] = changes::to_string(result.state);
                root["items"] = Json::array();
                for (const auto& [index, item] : result.items) {
                    Json entry = item_status_to_json(item.status);
                    entry["index"] = index;
                    root["items"].push_back(std::move(entry));
                }
                print(root.dump(2) + "\n");
            } else {
                for (const auto& [index, item] : result.items) {
                    print(std::format("  {:>3}  {}{}\n", index, changes::to_string(item.status.state),
                                      item.status.reason == changes::Reason::None
                                          ? ""
                                          : " (" + std::string(changes::to_string(item.status.reason)) + ")"));
                }
                print(std::format("Change set {}: {}\n", to_utf8(id), changes::to_string(result.state)));
            }
            return all_restored ? 0 : 1;
        }
        case Action::Purge:
            print(std::format("{} change set(s) older than 30 days deleted.\n", store->purge()));
            return 0;
        case Action::Recover:
            print("Interrupted change sets are settled.\n");  // open_change_store() did it
            return 0;
        case Action::None:
            break;
    }
    return 2;
}

}  // namespace avel::cli
