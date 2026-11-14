#include "cli/extensions_command.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <map>
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
#include "core/leftovers/protection.h"
#include "core/platform/elevation.h"
#include "core/platform/text.h"
#include "core/tools/browser_extensions.h"
#include "core/tools/browser_extensions_changes.h"
#include "core/version.h"

namespace avel::cli {

namespace {

using platform::to_utf8;
using tools::BrowserExtension;
using tools::ExtensionSource;
using Json = nlohmann::ordered_json;

constexpr std::string_view kUsage =
    "Usage: avel-cli extensions list [--all-users] [--removable] [--browser <name>] [--json]\n"
    "       avel-cli extensions remove <id>... [--dry-run] [--yes] [--json]\n"
    "  list                   the extensions of every Chromium-family (Chrome, Edge, Brave, Vivaldi,\n"
    "                         Opera, Chromium) and Firefox profile: yours (--all-users: every\n"
    "                         user's), whether each is enabled and comes from the browser's store,\n"
    "                         how it was installed, its permissions, and the policies and registry\n"
    "                         registrations that install it from outside the profile\n"
    "  --removable            only the extensions Avel can remove\n"
    "  --browser <name>       one browser's: chrome, chrome-beta, chrome-dev, chrome-canary, edge,\n"
    "                         edge-beta, edge-dev, edge-canary, brave, vivaldi, opera, opera-gx,\n"
    "                         chromium or firefox\n"
    "  remove <id>...         take out the policy entries and registrations that install extensions,\n"
    "                         so the browser uninstalls them at its next start; a policy list loses\n"
    "                         only that entry and is renumbered without a gap\n"
    "  --dry-run              show what would change, change nothing\n"
    "  --yes                  don't ask for confirmation\n"
    "  --json                 machine-readable output (with remove: needs --yes or --dry-run)\n"
    "An <id> comes from `extensions list`, or is an extension id (all its entries). Only extensions\n"
    "that a policy or a registration installs can be removed; the others are removed in the\n"
    "browser's own extensions page. Profiles' files and Firefox's policies.json are never changed.\n"
    "Every change is backed up, so `avel-cli undo` can put it back. list runs without administrator\n"
    "rights (other users' profiles are then out of reach); remove needs them, except with --dry-run.\n";

// Installed programs' names, by id.
class ProgramNames {
public:
    explicit ProgramNames(const std::vector<inventory::Program>& programs) : programs_(programs) {}

    std::string text(const std::wstring& id) const {
        const auto found = std::find_if(programs_.begin(), programs_.end(),
                                        [&](const inventory::Program& program) { return program.id == id; });
        return found != programs_.end() ? name_of(*found) : to_utf8(id);
    }

private:
    const std::vector<inventory::Program>& programs_;
};

// A diagnostic of the listing, its environment's included.
struct Note {
    std::string source;  // "environment", "extensions"
    std::string code;
    std::wstring subject;
    std::int32_t error = 0;
};

struct Listing {
    tools::BrowserExtensionsList list;
    std::vector<Note> notes;
    std::chrono::milliseconds elapsed{0};
};

// Lists the current user's profiles, or every user's, with the machine's sources and theirs. The
// LiveEnvironment lives only inside: it keeps signed-out users' hives loaded, privately, and a hive can
// be loaded so only once per process, so it must be gone before a ChangeSet writes to one of them.
Listing list_extensions(bool all_users, const std::vector<inventory::Program>& programs) {
    Listing listing;
    const auto started = std::chrono::steady_clock::now();
    {
        const leftovers::LiveEnvironment live;
        tools::BrowserExtensionsEnvironment environment;
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
        listing.list = tools::list_browser_extensions(environment);
        for (const auto& diagnostic : scan.diagnostics) {
            listing.notes.push_back({"environment", std::string(leftovers::to_string(diagnostic.code)),
                                     diagnostic.subject, diagnostic.error});
        }
        for (const auto& diagnostic : listing.list.diagnostics) {
            listing.notes.push_back({"extensions", std::string(tools::to_string(diagnostic.code)),
                                     diagnostic.subject, diagnostic.error});
        }
    }
    listing.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    return listing;
}

std::wstring quoted(const std::wstring& argument) {
    return argument.find_first_of(L" \t") != std::wstring::npos ? L"\"" + argument + L"\"" : argument;
}

std::wstring command_line(const tools::ExtensionsPage& page) {
    if (page.executable.empty()) {
        return {};
    }
    std::wstring line = L"\"" + page.executable + L"\"";
    for (const auto& argument : page.arguments) {
        line += L" " + quoted(argument);
    }
    return line;
}

Json strings_json(const std::vector<std::wstring>& list) {
    Json json = Json::array();
    for (const auto& item : list) {
        json.push_back(to_utf8(item));
    }
    return json;
}

Json page_json(const tools::ExtensionsPage& page) {
    return {{"executable", to_utf8(page.executable)},
            {"arguments", strings_json(page.arguments)},
            {"url", to_utf8(page.url)},
            {"command_line", to_utf8(command_line(page))}};
}

Json source_json(const ExtensionSource& source, const ProgramNames& names) {
    Json json;
    json["kind"] = tools::to_string(source.kind);
    Json browsers = Json::array();
    for (const auto browser : source.browsers) {
        browsers.push_back(tools::to_string(browser));
    }
    json["browsers"] = std::move(browsers);
    json["user_sid"] = to_utf8(source.user_sid);
    json["user_name"] = to_utf8(source.user_name);
    json["wow64"] = source.wow64;
    json["key"] = source.kind == tools::ExtensionSourceKind::PoliciesJson
                      ? Json(nullptr)
                      : Json(to_utf8(platform::display_path(source.key)));
    json["value_name"] = to_utf8(source.value_name);
    json["value"] = source.value
                        ? Json{{"type", source.value->type}, {"data", changes::to_hex(source.value->data)}}
                        : Json(nullptr);
    json["extension_id"] = to_utf8(source.extension_id);
    json["data"] = to_utf8(source.data);
    json["update_url"] = to_utf8(source.update_url);
    json["path"] = to_utf8(source.path);
    json["version"] = to_utf8(source.version);
    json["installation_mode"] = to_utf8(source.installation_mode);
    json["settings_key"] = to_utf8(source.settings_key);
    json["settings_subkey"] = source.settings_subkey;
    json["file"] = to_utf8(source.file);
    json["program_id"] = to_utf8(source.program_id);
    json["program_name"] = source.program_id.empty() ? std::string() : names.text(source.program_id);
    json["ignored_by_browser"] = source.ignored_by_browser;
    return json;
}

Json extension_json(const BrowserExtension& entry, const tools::BrowserExtensionsList& list,
                    const ProgramNames& names) {
    Json json;
    json["id"] = to_utf8(entry.id);
    json["browser"] = tools::to_string(entry.browser);
    json["user_sid"] = to_utf8(entry.user_sid);
    json["user_name"] = to_utf8(entry.user_name);
    json["profile_directory"] = to_utf8(entry.profile_directory);
    json["profile_name"] = to_utf8(entry.profile_name);
    json["profile_path"] = to_utf8(entry.profile_path);
    json["in_profile"] = entry.in_profile;
    json["extension_id"] = to_utf8(entry.extension_id);
    json["name"] = to_utf8(entry.name);
    json["description"] = to_utf8(entry.description);
    json["version"] = to_utf8(entry.version);
    json["folder"] = to_utf8(entry.folder);
    json["enabled"] = entry.enabled;
    json["disable_reasons"] = entry.disable_reasons;
    json["install"] = tools::to_string(entry.install);
    json["location"] = to_utf8(entry.location);
    json["installed_by_default"] = entry.installed_by_default;
    json["store"] = tools::to_string(entry.store);
    json["from_store"] = entry.from_store;
    json["update_url"] = to_utf8(entry.update_url);
    json["install_time"] = entry.install_time ? Json(*entry.install_time) : Json(nullptr);
    json["permissions"] = strings_json(entry.permissions);
    json["host_permissions"] = strings_json(entry.host_permissions);
    json["optional_permissions"] = strings_json(entry.optional_permissions);
    json["removal"] = tools::to_string(entry.removal);
    json["removable"] = entry.removal == tools::ExtensionRemoval::Removable;
    json["keeps_user_copy"] = entry.keeps_user_copy;
    Json sources = Json::array();
    for (const std::size_t s : entry.sources) {
        sources.push_back(source_json(list.sources[s], names));
    }
    json["sources"] = std::move(sources);
    json["page"] = page_json(entry.page);
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

void print_notes(const std::vector<Note>& notes, bool managed) {
    for (const auto& note : notes) {
        std::string text;
        if (note.code == "not-elevated") {
            text = "not running as administrator: other users' profiles and registry are out of reach";
        } else if (note.code == "user-hive-unavailable") {
            text = "the registry of user " + to_utf8(note.subject) + " couldn't be read";
        } else if (note.source == "extensions") {
            text = note.code + ": " + to_utf8(note.subject);
        } else {
            text = note.code + (note.subject.empty() ? "" : " " + to_utf8(note.subject));
        }
        if (note.error != 0) {
            text += std::format(" (error {})", note.error);
        }
        print_error("note: " + text + "\n");
    }
    if (managed) {
        print_error(
            "note: this PC is joined to a domain or enrolled in device management: removed policy entries "
            "may be written again\n");
    }
}

std::string user_text(const std::wstring& sid, const std::wstring& name) {
    if (sid.empty()) {
        return "machine";
    }
    return to_utf8(name.empty() ? sid : name);
}

std::string source_line(const ExtensionSource& source, const ProgramNames& names) {
    std::string where;
    if (source.kind == tools::ExtensionSourceKind::PoliciesJson) {
        where = to_utf8(source.file);
    } else {
        where = to_utf8(platform::display_path(source.key));
        if (!source.value_name.empty()) {
            where += " : " + to_utf8(source.value_name);
        }
    }
    std::string text = std::format("      {:<17} {}", tools::to_string(source.kind), where);
    if (!source.data.empty()) {
        text += " = " + to_utf8(source.data);
    }
    if (!source.installation_mode.empty()) {
        text += " [" + to_utf8(source.installation_mode) + "]";
    }
    if (!source.program_id.empty()) {
        text += " [program: " + names.text(source.program_id) + "]";
    }
    if (source.ignored_by_browser) {
        text += " [not read by the browser]";
    }
    return text + "\n";
}

std::string extension_line(const BrowserExtension& entry, const tools::BrowserExtensionsList& list,
                           const ProgramNames& names) {
    std::string name = to_utf8(entry.name.empty() ? entry.extension_id : entry.name);
    if (!entry.version.empty()) {
        name += " " + to_utf8(entry.version);
    }
    std::string marks;
    if (!entry.extension_id.empty() && !entry.name.empty()) {
        marks += " (" + to_utf8(entry.extension_id) + ")";
    }
    if (entry.from_store) {
        marks += std::format(" [store: {}]", tools::to_string(entry.store));
    }
    if (!entry.in_profile) {
        marks += " [not in a profile]";
    }
    if (entry.installed_by_default) {
        marks += " [installed by default]";
    }
    if (entry.keeps_user_copy) {
        marks += " [your own copy stays]";
    }
    marks += std::format(" [{}]", tools::to_string(entry.removal));
    const std::string profile =
        entry.in_profile ? to_utf8(entry.profile_name.empty() ? entry.profile_directory : entry.profile_name)
                         : std::string("-");
    std::string text =
        std::format("{}  {:<9}  {:<20}  {:<12}  {:<8}  {:<17}  {}{}\n", to_utf8(entry.id),
                    tools::to_string(entry.browser), user_text(entry.user_sid, entry.user_name), profile,
                    entry.in_profile ? (entry.enabled ? "enabled" : "disabled") : "-",
                    tools::to_string(entry.install), name, marks);
    for (const std::size_t s : entry.sources) {
        text += source_line(list.sources[s], names);
    }
    return text;
}

bool shown(const BrowserExtension& entry, const ExtensionsCommandOptions& options) {
    return (!options.removable || entry.removal == tools::ExtensionRemoval::Removable) &&
           (!options.browser || entry.browser == *options.browser);
}

int list_command(const ExtensionsCommandOptions& options) {
    const bool elevated = platform::is_process_elevated();
    const auto inventory = inventory::scan_inventory();
    const ProgramNames names(inventory.programs);
    const Listing listing = list_extensions(options.all_users, inventory.programs);
    const auto& list = listing.list;

    if (options.json) {
        Json root;
        root["avel_version"] = kVersionString;
        root["elevated"] = elevated;
        root["all_users"] = options.all_users;
        root["policies_managed"] = list.policies_managed;
        root["elapsed_ms"] = listing.elapsed.count();
        Json profiles = Json::array();
        for (const auto& profile : list.profiles) {
            if (!options.browser || profile.browser == *options.browser) {
                profiles.push_back({{"browser", tools::to_string(profile.browser)},
                                    {"user_sid", to_utf8(profile.user_sid)},
                                    {"user_name", to_utf8(profile.user_name)},
                                    {"directory", to_utf8(profile.directory)},
                                    {"name", to_utf8(profile.name)},
                                    {"path", to_utf8(profile.path)},
                                    {"page", page_json(profile.page)}});
            }
        }
        root["profiles"] = std::move(profiles);
        Json extensions = Json::array();
        for (const auto& entry : list.extensions) {
            if (shown(entry, options)) {
                extensions.push_back(extension_json(entry, list, names));
            }
        }
        root["extensions"] = std::move(extensions);
        root["diagnostics"] = notes_to_json(listing.notes);
        print(root.dump(2) + "\n");
        return 0;
    }
    std::size_t count = 0;
    std::size_t removable = 0;
    std::size_t disabled = 0;
    for (const auto& entry : list.extensions) {
        if (!shown(entry, options)) {
            continue;
        }
        print(extension_line(entry, list, names));
        ++count;
        removable += entry.removal == tools::ExtensionRemoval::Removable;
        disabled += entry.in_profile && !entry.enabled;
    }
    std::size_t profiles = 0;
    for (const auto& profile : list.profiles) {
        profiles += !options.browser || profile.browser == *options.browser;
    }
    print(std::format(
        "\n{} extension{} in {} profile{}: {} disabled, {} removable by Avel. Listed in {:.1f} s.\n", count,
        count == 1 ? "" : "s", profiles, profiles == 1 ? "" : "s", disabled, removable,
        static_cast<double>(listing.elapsed.count()) / 1000.0));
    print_notes(listing.notes, list.policies_managed);
    return 0;
}

std::string issue_text(const tools::ExtensionPlanIssue& issue) {
    const std::string id = to_utf8(issue.id);
    switch (issue.code) {
        case tools::ExtensionPlanIssueCode::UnknownId:
            return id + ": no such extension; see `avel-cli extensions list`";
        case tools::ExtensionPlanIssueCode::NotRemovable:
            return id + ": no policy or registration installs it; remove it in the browser's extensions page";
        case tools::ExtensionPlanIssueCode::ManagedByPoliciesJson:
            return id + ": Firefox's policies.json installs it; Avel doesn't edit that file";
        case tools::ExtensionPlanIssueCode::NotSupported:
            return id + ": a policy key names it together with other extensions";
        case tools::ExtensionPlanIssueCode::Protected:
            return id + ": a protected registry location holds it";
    }
    return id + ": " + std::string(tools::to_string(issue.code));
}

const BrowserExtension* entry_with_id(const tools::BrowserExtensionsList& list, const std::wstring& id) {
    const auto found =
        std::find_if(list.extensions.begin(), list.extensions.end(),
                     [&](const BrowserExtension& entry) { return platform::iequals(entry.id, id); });
    return found != list.extensions.end() ? &*found : nullptr;
}

// The entries an argument names: one entry, or every entry of an extension id.
std::vector<const BrowserExtension*> entries_named(const tools::BrowserExtensionsList& list,
                                                   const std::wstring& id) {
    if (const auto* entry = entry_with_id(list, id)) {
        return {entry};
    }
    std::vector<const BrowserExtension*> entries;
    for (const auto& entry : list.extensions) {
        if (!entry.extension_id.empty() && platform::iequals(entry.extension_id, id)) {
            entries.push_back(&entry);
        }
    }
    return entries;
}

// "Remove browser extension Contoso Helper", "Remove 3 browser extensions": shown in the Undo Center.
std::wstring describe_plan(const tools::ExtensionsPlan& planned, const tools::BrowserExtensionsList& list) {
    std::map<std::wstring, int> names;
    for (const auto& id : planned.planned) {
        if (const auto* entry = entry_with_id(list, id)) {
            names[entry->extension_id.empty() ? entry->name : entry->extension_id]++;
        }
    }
    if (names.size() == 1) {
        if (const auto* entry = entry_with_id(list, planned.planned.front())) {
            return L"Remove browser extension " + (entry->name.empty() ? entry->extension_id : entry->name);
        }
    }
    return std::format(L"Remove {} browser extensions", names.size());
}

bool ask(const std::string& question) {
    print(question + " [y/N] ");
    std::string answer;
    std::getline(std::cin, answer);
    return !answer.empty() && (answer.front() == 'y' || answer.front() == 'Y');
}

// Done, or left to go with the key that contains it.
bool finished(const changes::ItemStatus& status) {
    return status.state == changes::ItemState::Done ||
           (status.state == changes::ItemState::Skipped && status.reason == changes::Reason::Contained);
}

int remove_command(const ExtensionsCommandOptions& options) {
    const bool elevated = platform::is_process_elevated();
    if (!options.dry_run && !elevated) {
        print_error(
            "extensions: removing extensions needs administrator rights; run avel-cli as administrator (or "
            "use --dry-run)\n");
        return 1;
    }
    std::unique_ptr<changes::ChangeStore> store;
    if (!options.dry_run &&
        !(store = open_change_store())) {  // Also settles interrupted sets, purges old ones
        return 1;
    }
    const auto inventory = inventory::scan_inventory();
    const ProgramNames names(inventory.programs);
    // Every user's: an id from `extensions list --all-users` must be found too. The listing's environment
    // is gone once this returns, before the ChangeSet opens any user's hive.
    const Listing listing = list_extensions(true, inventory.programs);
    const auto& list = listing.list;
    auto planned = tools::plan_remove_extensions(list, options.ids, [](const leftovers::Location& location) {
        return leftovers::HardFloor::live().check(location);
    });
    planned.plan.description = describe_plan(planned, list);
    const auto& plan = planned.plan;
    const bool refused = !planned.issues.empty();

    if (!options.json) {
        print(options.dry_run ? "Dry run, nothing will be changed.\n" : "");
        for (const auto& id : options.ids) {
            for (const auto* entry : entries_named(list, id)) {
                print("  " + extension_line(*entry, list, names));
            }
        }
        for (const auto& issue : planned.issues) {
            print("error: " + issue_text(issue) + "\n");
        }
        for (const auto& note : planned.notes) {
            if (note.code == tools::ExtensionPlanNoteCode::KeepsUserCopy) {
                print("note: " + to_utf8(note.id) +
                      ": the copy installed from the store or by the user stays; remove it on the browser's "
                      "extensions page\n");
            }
        }
        if (!refused && !plan.items.empty()) {
            print(std::format("\n{}:\n", to_utf8(plan.description)));
            for (const auto& item : plan.items) {
                print(std::format("  {:<16} {}\n", kind_of(item.location),
                                  to_utf8(describe_location(item.location))));
            }
            std::vector<std::string> others;
            for (const auto& id : planned.affected) {
                if (std::find(planned.planned.begin(), planned.planned.end(), id) == planned.planned.end()) {
                    if (const auto* entry = entry_with_id(list, id)) {
                        others.push_back(std::format(
                            "{} ({}, {}, {})", to_utf8(entry->id), tools::to_string(entry->browser),
                            user_text(entry->user_sid, entry->user_name), to_utf8(entry->profile_name)));
                    }
                }
            }
            if (!others.empty()) {
                print("These sources also install it in:\n");
                for (const auto& other : others) {
                    print("  " + other + "\n");
                }
            }
        }
        print_notes(listing.notes, list.policies_managed);
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
        if (!options.yes &&
            !ask(std::format("\nRemove {} extension entr{}?", count, count == 1 ? "y" : "ies"))) {
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
            print_error("extensions: another Avel process is changing the system; nothing was changed\n");
            exit_code = 1;
        }
    }

    if (options.json) {
        Json root;
        root["avel_version"] = kVersionString;
        root["elevated"] = elevated;
        root["dry_run"] = options.dry_run;
        root["action"] = "remove";
        root["policies_managed"] = list.policies_managed;
        Json extensions = Json::array();
        for (const auto& id : options.ids) {
            for (const auto* entry : entries_named(list, id)) {
                extensions.push_back(extension_json(*entry, list, names));
            }
        }
        root["extensions"] = std::move(extensions);
        Json issues = Json::array();
        for (const auto& issue : planned.issues) {
            issues.push_back({{"id", to_utf8(issue.id)}, {"code", tools::to_string(issue.code)}});
        }
        root["issues"] = std::move(issues);
        root["planned"] = strings_json(planned.planned);
        Json affected = Json::array();
        for (const auto& id : planned.affected) {
            if (const auto* entry = entry_with_id(list, id)) {
                affected.push_back({{"id", to_utf8(entry->id)},
                                    {"browser", tools::to_string(entry->browser)},
                                    {"user_sid", to_utf8(entry->user_sid)},
                                    {"user_name", to_utf8(entry->user_name)},
                                    {"profile_directory", to_utf8(entry->profile_directory)},
                                    {"profile_name", to_utf8(entry->profile_name)},
                                    {"in_profile", entry->in_profile},
                                    {"extension_id", to_utf8(entry->extension_id)},
                                    {"name", to_utf8(entry->name)},
                                    {"keeps_user_copy", entry->keeps_user_copy}});
            }
        }
        root["affected"] = std::move(affected);
        Json notes = Json::array();
        for (const auto& note : planned.notes) {
            notes.push_back({{"id", to_utf8(note.id)}, {"code", tools::to_string(note.code)}});
        }
        root["notes"] = std::move(notes);
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
        print_execute_result(plan, *result, ResultWording::Removal);
    }
    return exit_code;
}

}  // namespace

int run_extensions_command(const std::vector<std::wstring_view>& arguments) {
    if (!arguments.empty()) {
        const auto name = arguments.front();
        if (name == L"help" || name == L"--help" || name == L"-h" || name == L"/?") {
            print(kUsage);
            return 0;
        }
    }
    ExtensionsCommandOptions options;
    if (const auto error = parse_extensions_options(arguments, options)) {
        print_error(error->message + (error->usage ? std::string(kUsage) : std::string()));
        return 2;
    }
    return options.command == ExtensionsCommand::Remove ? remove_command(options) : list_command(options);
}

}  // namespace avel::cli
