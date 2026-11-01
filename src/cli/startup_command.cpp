#include "cli/startup_command.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <iostream>
#include <optional>
#include <string>

#include "cli/change_output.h"
#include "cli/command_options.h"
#include "cli/console.h"
#include "cli/leftover_output.h"
#include "cli/program_lookup.h"
#include "core/changes/serialization.h"
#include "core/inventory/inventory.h"
#include "core/leftovers/environment.h"
#include "core/platform/elevation.h"
#include "core/platform/text.h"
#include "core/tools/startup.h"
#include "core/tools/startup_changes.h"
#include "core/version.h"

namespace avel::cli {

namespace {

using platform::to_utf8;
using tools::StartupEntry;
using Json = nlohmann::ordered_json;

constexpr std::string_view kUsage =
    "Usage: avel-cli startup list [--all-users] [--json]\n"
    "       avel-cli startup disable <id>... [--service-start manual|disabled] [--dry-run] [--yes] [--json]\n"
    "       avel-cli startup enable <id>... [--dry-run] [--yes] [--json]\n"
    "       avel-cli startup delete <id>... [--dry-run] [--yes] [--json]\n"
    "  list                   what starts with Windows: Run and RunOnce values, Startup folders,\n"
    "                         scheduled tasks that start at log on or boot, and automatic\n"
    "                         services; the machine's and yours (--all-users: every user's)\n"
    "  disable <id>...        keep entries from starting: Run values and Startup folder items\n"
    "                         as Task Manager does it, a task is disabled, a service starts\n"
    "                         manually (--service-start disabled: never)\n"
    "  enable <id>...         let disabled entries start again (a service: automatically)\n"
    "  delete <id>...         delete entries: the value, file, task or service\n"
    "  --dry-run              show what would change, change nothing\n"
    "  --yes                  don't ask for confirmation\n"
    "  --json                 machine-readable output (with an action: needs --yes or --dry-run)\n"
    "The ids come from `startup list`; actions find entries of every user. Windows' own entries\n"
    "are listed but never changed. Every change is backed up, so `avel-cli undo` can put it\n"
    "back. list runs without administrator rights (other users' entries and some services are\n"
    "then out of reach); the actions need them, except with --dry-run.\n";

// How long reading the change store waits for another Avel process that is changing the system.
constexpr std::chrono::seconds kStoreReadTimeout{10};

// Installed programs' names, by id.
class ProgramNames {
public:
    explicit ProgramNames(const std::vector<inventory::Program>& programs) : programs_(programs) {}

    const inventory::Program* find(const std::wstring& id) const {
        if (id.empty()) {
            return nullptr;
        }
        const auto found = std::find_if(programs_.begin(), programs_.end(),
                                        [&](const inventory::Program& program) { return program.id == id; });
        return found != programs_.end() ? &*found : nullptr;
    }

    std::string text(const std::wstring& id) const {
        const auto* program = find(id);
        return program ? name_of(*program) : to_utf8(id);
    }

    std::wstring wide(const std::wstring& id) const {
        const auto* program = find(id);
        return program && !program->display_name.empty() ? program->display_name : id;
    }

private:
    const std::vector<inventory::Program>& programs_;
};

// A diagnostic of the listing, its environment's included.
struct Note {
    std::string source;  // "environment", "startup", "change-store"
    std::string code;
    std::wstring subject;
    std::int32_t error = 0;
};

struct Listing {
    std::vector<StartupEntry> entries;
    std::vector<Note> notes;
    std::chrono::milliseconds elapsed{0};
};

// The services Avel disabled, from the change store, read without settling or purging anything.
std::vector<std::wstring> read_avel_disabled(changes::ChangeStore& store, std::vector<Note>& notes) {
    const auto guard = store.lock(kStoreReadTimeout);
    if (!guard) {
        notes.push_back({"change-store", "busy", L"", 0});
        return {};
    }
    return tools::services_disabled_by_avel(store);
}

// The same, opening the store only to read it (list and dry runs). Administrators only.
std::vector<std::wstring> read_avel_disabled(std::vector<Note>& notes) {
    platform::FolderStatus status{};
    const auto store = changes::ChangeStore::open(changes::default_store_options(), status);
    if (!store) {
        notes.push_back({"change-store", std::string(platform::to_string(status)), L"", 0});
        return {};
    }
    return read_avel_disabled(*store, notes);
}

// Lists the entries of the machine and the current user, or of every user. The LiveEnvironment
// lives only inside: it keeps signed-out users' hives loaded, privately, and a hive can be loaded
// so only once per process, so it must be gone before a ChangeSet writes to one of them.
Listing list_entries(bool all_users, std::vector<std::wstring> avel_disabled,
                     const std::vector<inventory::Program>& programs, const tools::SignerFunction& signer,
                     std::vector<Note> notes) {
    Listing listing;
    listing.notes = std::move(notes);
    const auto started = std::chrono::steady_clock::now();
    {
        const leftovers::LiveEnvironment live;
        tools::StartupEnvironment environment;
        environment.scan = live.environment();
        auto& scan = environment.scan;
        if (!all_users) {
            const auto other = [&](const std::wstring& sid) {
                return !sid.empty() && !platform::iequals(sid, scan.current_user_sid);
            };
            std::erase_if(scan.users,
                          [&](const leftovers::UserScope& user) { return other(user.profile.sid); });
            std::erase_if(scan.diagnostics, [&](const leftovers::ScanDiagnostic& diagnostic) {
                return other(diagnostic.subject);
            });
        }
        environment.programs = programs;
        environment.signer = signer;
        environment.avel_disabled_services = std::move(avel_disabled);
        auto list = tools::list_startup(environment);
        listing.entries = std::move(list.entries);
        for (const auto& diagnostic : scan.diagnostics) {
            listing.notes.push_back({"environment", std::string(leftovers::to_string(diagnostic.code)),
                                     diagnostic.subject, diagnostic.error});
        }
        for (const auto& diagnostic : list.diagnostics) {
            listing.notes.push_back(
                {"startup", std::string(tools::to_string(diagnostic.code)), L"", diagnostic.error});
        }
    }
    listing.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    return listing;
}

std::string user_text(const StartupEntry& entry) {
    if (entry.user_sid.empty()) {
        return "machine";
    }
    return to_utf8(entry.user_name.empty() ? entry.user_sid : entry.user_name);
}

std::string start_text(std::uint32_t start, bool delayed) {
    switch (start) {
        case SERVICE_AUTO_START:
            return delayed ? "automatic (delayed)" : "automatic";
        case SERVICE_DEMAND_START:
            return "manual";
        case SERVICE_DISABLED:
            return "disabled";
        default:
            return std::to_string(start);
    }
}

std::string entry_line(const StartupEntry& entry, const ProgramNames& names) {
    std::string name = to_utf8(entry.name);
    if (!entry.display_name.empty() && entry.display_name != entry.name) {
        name += " (" + to_utf8(entry.display_name) + ")";
    }
    std::string marks;
    if (entry.wow64) {
        marks += " [32-bit]";
    }
    if (entry.kind == tools::StartupKind::Service) {
        marks += " [start: " + start_text(entry.service_start, entry.service_delayed) + "]";
    }
    if (entry.target_missing) {
        marks += " [target missing]";
    }
    if (entry.protection == tools::StartupProtection::WindowsOwn) {
        marks += " [Windows' own: protected]";
    }
    if (entry.disabled_by_avel) {
        marks += " [disabled by Avel]";
    }
    if (!entry.program_id.empty()) {
        marks += " [program: " + names.text(entry.program_id) + "]";
    }
    return std::format("{}  {:<8}  {:<14}  {:<20}  {}: {}{}\n", to_utf8(entry.id),
                       tools::to_string(entry.state), tools::to_string(entry.kind), user_text(entry), name,
                       entry.command.empty() ? std::string("(no command)") : to_utf8(entry.command), marks);
}

Json raw_value_to_json(const std::optional<platform::RawValue>& value) {
    if (!value) {
        return nullptr;
    }
    Json json;
    json["type"] = value->type;
    json["data"] = changes::to_hex(value->data);
    return json;
}

Json entry_to_json(const StartupEntry& entry, const ProgramNames& names) {
    Json json;
    json["id"] = to_utf8(entry.id);
    json["kind"] = tools::to_string(entry.kind);
    json["user_sid"] = to_utf8(entry.user_sid);
    json["user_name"] = to_utf8(entry.user_name);
    json["wow64"] = entry.wow64;
    json["name"] = to_utf8(entry.name);
    json["display_name"] = to_utf8(entry.display_name);
    json["command"] = to_utf8(entry.command);
    json["value_type"] = entry.value_type;
    json["target"] = to_utf8(entry.target);
    json["target_missing"] = entry.target_missing;
    json["product"] = {{"product_name", to_utf8(entry.product.product_name)},
                       {"company_name", to_utf8(entry.product.company_name)}};
    json["signer"] = {{"checked", entry.signer.checked},
                      {"valid", entry.signer.valid},
                      {"windows", entry.signer.windows},
                      {"name", to_utf8(entry.signer.name)}};
    json["program_id"] = to_utf8(entry.program_id);
    json["program_name"] = entry.program_id.empty() ? std::string() : names.text(entry.program_id);
    json["state"] = tools::to_string(entry.state);
    json["disabled_by_avel"] = entry.disabled_by_avel;
    if (entry.approval) {
        json["approval"] = {{"key", to_utf8(platform::display_path(entry.approval->key))},
                            {"value_name", to_utf8(entry.approval->value_name)},
                            {"value", raw_value_to_json(entry.approval->value)}};
    } else {
        json["approval"] = nullptr;
    }
    json["task_enabled"] = entry.task_enabled;
    json["service_start"] = entry.service_start;
    json["service_delayed"] = entry.service_delayed;
    Json locations = Json::array();
    for (const auto& location : entry.delete_locations) {
        locations.push_back(location_to_json(location));
    }
    json["delete_locations"] = std::move(locations);
    json["protection"] = tools::to_string(entry.protection);
    return json;
}

Json notes_to_json(const std::vector<Note>& notes) {
    Json json = Json::array();
    for (const auto& note : notes) {
        json.push_back({{"source", note.source},
                        {"code", note.code},
                        {"subject", to_utf8(note.subject)},
                        {"error", note.error}});
    }
    return json;
}

void print_notes(const std::vector<Note>& notes) {
    for (const auto& note : notes) {
        std::string text;
        if (note.source == "change-store") {
            text = note.code == "busy"
                       ? "Avel's change sets are in use by another Avel process; services Avel disabled "
                         "may be missing"
                       : "Avel's change sets can't be read (" + note.code +
                             "); services Avel disabled may be missing";
        } else if (note.code == "not-elevated") {
            text = "not running as administrator: other users' entries and some services may be missing";
        } else if (note.code == "user-hive-unavailable") {
            text = "the registry of user " + to_utf8(note.subject) + " couldn't be read";
        } else if (note.code == "tasks-unavailable") {
            text = "the Task Scheduler couldn't be read: no tasks listed";
        } else if (note.code == "services-unavailable") {
            text = "the services couldn't be read: no services listed";
        } else {
            text = note.code + (note.subject.empty() ? "" : " " + to_utf8(note.subject));
        }
        if (note.error != 0) {
            text += std::format(" (error {})", note.error);
        }
        print_error("note: " + text + "\n");
    }
}

int list_command(const StartupCommandOptions& options) {
    const bool elevated = platform::is_process_elevated();
    std::vector<Note> notes;
    // The change store is administrators' only; without it no service counts as disabled by Avel.
    auto avel_disabled = elevated ? read_avel_disabled(notes) : std::vector<std::wstring>();
    const auto inventory = inventory::scan_inventory();
    const ProgramNames names(inventory.programs);
    const Listing listing = list_entries(options.all_users, std::move(avel_disabled), inventory.programs,
                                         tools::live_signer(), std::move(notes));

    if (options.json) {
        Json root;
        root["avel_version"] = kVersionString;
        root["elevated"] = elevated;
        root["all_users"] = options.all_users;
        root["elapsed_ms"] = listing.elapsed.count();
        Json entries = Json::array();
        for (const auto& entry : listing.entries) {
            entries.push_back(entry_to_json(entry, names));
        }
        root["entries"] = std::move(entries);
        root["diagnostics"] = notes_to_json(listing.notes);
        print(root.dump(2) + "\n");
        return 0;
    }
    std::size_t disabled = 0;
    std::size_t protected_count = 0;
    for (const auto& entry : listing.entries) {
        print(entry_line(entry, names));
        disabled += entry.state == tools::StartupState::Disabled;
        protected_count += entry.protection != tools::StartupProtection::None;
    }
    print(std::format("\n{} entr{}: {} disabled, {} Windows' own (protected). Listed in {:.1f} s.\n",
                      listing.entries.size(), listing.entries.size() == 1 ? "y" : "ies", disabled,
                      protected_count, static_cast<double>(listing.elapsed.count()) / 1000.0));
    print_notes(listing.notes);
    return 0;
}

std::string issue_text(const tools::StartupPlanIssue& issue, tools::StartupAction action) {
    const std::string id = to_utf8(issue.id);
    switch (issue.code) {
        case tools::StartupPlanIssueCode::UnknownId:
            return id + ": no such entry; see `avel-cli startup list`";
        case tools::StartupPlanIssueCode::Protected:
            return id + ": Windows' own entry; it is never changed";
        case tools::StartupPlanIssueCode::NotSupported:
            return id +
                   ": can only be deleted (RunOnce, RunOnceEx and a user's 32-bit Run values have no "
                   "disabled state)";
        case tools::StartupPlanIssueCode::AlreadyInState:
            return id + (action == tools::StartupAction::Enable ? ": already enabled" : ": already disabled");
    }
    return id + ": " + std::string(tools::to_string(issue.code));
}

std::wstring entry_label(const StartupEntry& entry) {
    return entry.display_name.empty() ? entry.name : entry.display_name;
}

const StartupEntry* entry_with_id(const std::vector<StartupEntry>& entries, const std::wstring& id) {
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const StartupEntry& entry) {
        return platform::iequals(entry.id, id);
    });
    return found != entries.end() ? &*found : nullptr;
}

// "Disable startup entry Contoso", "Delete 3 startup entries": shown in the Undo Center.
std::wstring describe_plan(tools::StartupAction action, const std::vector<std::wstring>& planned,
                           const std::vector<StartupEntry>& entries) {
    const std::wstring verb = action == tools::StartupAction::Disable  ? L"Disable"
                              : action == tools::StartupAction::Enable ? L"Enable"
                                                                       : L"Delete";
    if (planned.size() == 1) {
        if (const auto* entry = entry_with_id(entries, planned.front())) {
            return verb + L" startup entry " + entry_label(*entry);
        }
    }
    return std::format(L"{} {} startup entries", verb, planned.size());
}

bool ask(const std::string& question) {
    print(question + " [y/N] ");
    std::string answer;
    std::getline(std::cin, answer);
    return !answer.empty() && (answer.front() == 'y' || answer.front() == 'Y');
}

// Done, or left to go with the key or folder that contains it.
bool finished(const changes::ItemStatus& status) {
    return status.state == changes::ItemState::Done ||
           (status.state == changes::ItemState::Skipped && status.reason == changes::Reason::Contained);
}

int action_command(const StartupCommandOptions& options, tools::StartupAction action) {
    const bool elevated = platform::is_process_elevated();
    if (!options.dry_run && !elevated) {
        print_error(
            "startup: changing startup entries needs administrator rights; run avel-cli as "
            "administrator (or use --dry-run)\n");
        return 1;
    }
    std::vector<Note> notes;
    std::unique_ptr<changes::ChangeStore> store;
    std::vector<std::wstring> avel_disabled;
    if (!options.dry_run) {
        if (!(store = open_change_store())) {  // Also settles interrupted sets and purges old ones
            return 1;
        }
        avel_disabled = read_avel_disabled(*store, notes);
    } else if (elevated) {
        avel_disabled = read_avel_disabled(notes);
    }

    const auto inventory = inventory::scan_inventory();
    const ProgramNames names(inventory.programs);
    // Every user's: an id from `startup list --all-users` must be found too. The listing's
    // environment is gone once this returns, before the ChangeSet opens any user's hive.
    const Listing listing = list_entries(true, std::move(avel_disabled), inventory.programs,
                                         tools::live_signer(), std::move(notes));
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    auto planned = tools::plan_startup(listing.entries, action, options.ids, options.service_start, now,
                                       [&](const std::wstring& id) { return names.wide(id); });
    planned.plan.description = describe_plan(action, planned.planned, listing.entries);
    const auto& plan = planned.plan;
    const bool refused = std::any_of(planned.issues.begin(), planned.issues.end(),
                                     [](const auto& issue) { return tools::is_error(issue.code); });

    if (!options.json) {
        print(options.dry_run ? "Dry run, nothing will be changed.\n" : "");
        for (const auto& id : options.ids) {
            if (const auto* entry = entry_with_id(listing.entries, id)) {
                print("  " + entry_line(*entry, names));
            }
        }
        for (const auto& issue : planned.issues) {
            print((tools::is_error(issue.code) ? "error: " : "note: ") + issue_text(issue, action) + "\n");
        }
        if (!refused && !plan.items.empty()) {
            print(std::format("\n{}:\n", to_utf8(plan.description)));
            for (const auto& item : plan.items) {
                print(std::format("  {:<16} {}\n", kind_of(item.location),
                                  to_utf8(describe_location(item.location))));
            }
        }
        print_notes(listing.notes);
    }

    std::optional<changes::ExecuteResult> result;
    int exit_code = 0;
    if (refused) {
        if (!options.json) {
            print("Nothing was changed.\n");
        }
        exit_code = 1;
    } else if (plan.items.empty()) {
        if (!options.json) {
            print("Nothing to change.\n");
        }
    } else if (!options.dry_run) {
        const std::size_t count = planned.planned.size();
        const std::string verb = action == tools::StartupAction::Disable  ? "Disable"
                                 : action == tools::StartupAction::Enable ? "Enable"
                                                                          : "Delete";
        if (!options.yes && !ask(std::format("\n{} {} entr{}?", verb, count, count == 1 ? "y" : "ies"))) {
            print("Cancelled.\n");
            return 1;
        }
        if (const auto guard = store->lock()) {
            result = changes::execute(*store, plan, cli_execute_options(false));
            if (result->set_id.empty() ||
                !std::all_of(result->items.begin(), result->items.end(),
                             [](const auto& item) { return finished(item.status); })) {
                exit_code = 1;
            }
        } else {
            print_error("startup: another Avel process is changing the system; nothing was changed\n");
            exit_code = 1;
        }
    }

    if (options.json) {
        Json root;
        root["avel_version"] = kVersionString;
        root["elevated"] = elevated;
        root["dry_run"] = options.dry_run;
        root["action"] = tools::to_string(action);
        root["service_start"] = action == tools::StartupAction::Disable
                                    ? Json(tools::to_string(options.service_start))
                                    : Json(nullptr);
        Json entries = Json::array();
        for (const auto& id : options.ids) {
            if (const auto* entry = entry_with_id(listing.entries, id)) {
                entries.push_back(entry_to_json(*entry, names));
            }
        }
        root["entries"] = std::move(entries);
        Json issues = Json::array();
        for (const auto& issue : planned.issues) {
            issues.push_back({{"id", to_utf8(issue.id)},
                              {"code", tools::to_string(issue.code)},
                              {"error", tools::is_error(issue.code)}});
        }
        root["issues"] = std::move(issues);
        root["description"] = to_utf8(plan.description);
        Json items = Json::array();
        if (!refused) {
            for (const auto& item : plan.items) {
                items.push_back(location_to_json(item.location));
            }
        }
        root["plan"] = std::move(items);
        root["change"] = result ? execute_result_to_json(plan, *result) : Json(nullptr);
        root["diagnostics"] = notes_to_json(listing.notes);
        print(root.dump(2) + "\n");
        return exit_code;
    }
    if (result) {
        print("\n");
        print_execute_result(
            plan, *result,
            action == tools::StartupAction::Delete ? ResultWording::Removal : ResultWording::StateChange);
    }
    return exit_code;
}

}  // namespace

int run_startup_command(const std::vector<std::wstring_view>& arguments) {
    if (!arguments.empty()) {
        const auto name = arguments.front();
        if (name == L"help" || name == L"--help" || name == L"-h" || name == L"/?") {
            print(kUsage);
            return 0;
        }
    }
    StartupCommandOptions options;
    if (const auto error = parse_startup_options(arguments, options)) {
        print_error(error->message + (error->usage ? std::string(kUsage) : std::string()));
        return 2;
    }
    if (const auto action = startup_action(options.command)) {
        return action_command(options, *action);
    }
    return list_command(options);
}

}  // namespace avel::cli
