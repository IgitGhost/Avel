#include "cli/apps_command.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <optional>
#include <string>

#include "cli/change_output.h"
#include "cli/command_options.h"
#include "cli/console.h"
#include "cli/leftover_output.h"
#include "core/platform/elevation.h"
#include "core/platform/text.h"
#include "core/tools/windows_apps.h"
#include "core/tools/windows_apps_changes.h"
#include "core/version.h"

namespace avel::cli {

namespace {

using platform::to_utf8;
using tools::WindowsApp;
using Json = nlohmann::ordered_json;

constexpr std::string_view kUsage =
    "Usage: avel-cli apps list [--all] [--json]\n"
    "       avel-cli apps remove <id>... [--dry-run] [--yes] [--json]\n"
    "       avel-cli apps deprovision <id>... [--dry-run] [--yes] [--json]\n"
    "       avel-cli apps remove-everywhere <id>... --allow-irreversible [--dry-run] [--yes] [--json]\n"
    "  list                   the packaged apps on this PC (Store and MSIX apps, Windows' own\n"
    "                         included), one per package family: who has each, whether it is\n"
    "                         installed for new users, and its category (--all: the protected\n"
    "                         ones too, which are never changed)\n"
    "  remove <id>...         remove apps for you only; they stay on this PC for the other users\n"
    "                         or for new users, so undo can install them for you again (their\n"
    "                         data isn't kept)\n"
    "  deprovision <id>...    stop installing apps for new users; those who have them keep them\n"
    "  remove-everywhere <id>...\n"
    "                         remove apps for every user and delete them from this PC; this can't\n"
    "                         be undone (--allow-irreversible confirms it), they can only be\n"
    "                         installed again from the Store or winget\n"
    "  --dry-run              show what would change, change nothing\n"
    "  --yes                  don't ask for confirmation\n"
    "  --json                 machine-readable output (with an action: needs --yes or --dry-run)\n"
    "An <id> comes from `apps list`, or is a package family name (Microsoft.BingNews_8wekyb3d8bbwe).\n"
    "Frameworks and resource packages aren't listed; apps signed as part of Windows and those\n"
    "Windows needs (the Store, App Installer, the shell's hosts...) are protected, and so are the\n"
    "packages that give a desktop program its identity (they go when it is uninstalled). list runs\n"
    "without administrator rights, but then shows only your apps and can't tell who else has\n"
    "them or whether they are provisioned; the actions need administrator rights.\n";

struct Listing {
    tools::WindowsAppsList list;
    std::chrono::milliseconds elapsed{0};
};

Listing list_apps() {
    Listing listing;
    const auto started = std::chrono::steady_clock::now();
    listing.list = tools::list_windows_apps(tools::live_windows_apps_environment());
    listing.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    return listing;
}

std::string_view signature_text(platform::PackageSignatureKind kind) {
    switch (kind) {
        case platform::PackageSignatureKind::None:
            return "none";
        case platform::PackageSignatureKind::Developer:
            return "developer";
        case platform::PackageSignatureKind::Enterprise:
            return "enterprise";
        case platform::PackageSignatureKind::Store:
            return "store";
        case platform::PackageSignatureKind::System:
            return "system";
    }
    return "unknown";
}

// "you", "you +2", "3 users", with ", new users" when provisioned.
std::string users_text(const WindowsApp& app) {
    const bool current =
        std::any_of(app.users.begin(), app.users.end(), [](const auto& user) { return user.current; });
    const std::size_t others = app.users.size() - (current ? 1 : 0);
    std::string text;
    if (current) {
        text = others > 0 ? std::format("you +{}", others) : "you";
    } else if (others > 0) {
        text = std::format("{} user{}", others, others == 1 ? "" : "s");
    }
    if (app.provisioned) {
        text += text.empty() ? "new users" : ", new users";
    }
    return text.empty() ? "nobody" : text;
}

std::string modes_text(const WindowsApp& app) {
    std::string text;
    const auto add = [&](bool available, std::string_view mode) {
        if (available) {
            text += (text.empty() ? "" : ", ") + std::string(mode);
        }
    };
    add(app.can_remove_for_me, "remove");
    add(app.can_deprovision, "deprovision");
    add(app.can_remove_for_everyone, "remove-everywhere");
    return text;
}

// `complete`: the listing's all_users. An incomplete one offers no mode for any app.
std::string app_line(const WindowsApp& app, bool complete) {
    std::string marks;
    if (app.protection != tools::AppProtection::None) {
        marks += " [protected: " + std::string(tools::to_string(app.protection)) + "]";
        if (app.protection == tools::AppProtection::ExternalLocation) {
            marks += " [part of: " + to_utf8(app.external_location) + "]";
        }
    } else if (!complete) {
        marks += " [needs administrator]";
    } else {
        marks += " [" + modes_text(app) + "]";
    }
    if (app.warning != tools::AppWarning::None) {
        marks += " [warning: " + std::string(tools::to_string(app.warning)) + "]";
    }
    return std::format("{}  {:<13}  {:<18}  {} {} ({}){}\n", to_utf8(app.id), tools::to_string(app.category),
                       users_text(app), to_utf8(app.display_name), to_utf8(app.version),
                       to_utf8(app.family_name), marks);
}

Json app_to_json(const WindowsApp& app) {
    Json json;
    json["id"] = to_utf8(app.id);
    json["family"] = to_utf8(app.family_name);
    json["name"] = to_utf8(app.name);
    json["display_name"] = to_utf8(app.display_name);
    json["publisher"] = to_utf8(app.publisher);
    json["version"] = to_utf8(app.version);
    json["install_location"] = to_utf8(app.install_location);
    json["external_location"] = to_utf8(app.external_location);
    json["signature"] = signature_text(app.signature_kind);
    Json versions = Json::array();
    for (const auto& package : app.packages) {
        Json sids = Json::array();
        for (const auto& sid : package.user_sids) {
            sids.push_back(to_utf8(sid));
        }
        versions.push_back({{"full_name", to_utf8(package.full_name)},
                            {"version", to_utf8(package.version)},
                            {"users", sids}});
    }
    json["versions"] = std::move(versions);
    json["current_user_full_name"] = to_utf8(app.current_user_full_name);
    Json users = Json::array();
    for (const auto& user : app.users) {
        users.push_back({{"sid", to_utf8(user.sid)},
                         {"account_name", to_utf8(user.account_name)},
                         {"current", user.current}});
    }
    json["users"] = std::move(users);
    json["provisioned"] = app.provisioned;
    json["provisioned_full_name"] = to_utf8(app.provisioned_full_name);
    json["category"] = tools::to_string(app.category);
    json["warning"] = tools::to_string(app.warning);
    json["protection"] = tools::to_string(app.protection);
    json["soft_undoable"] = app.soft_undoable;
    json["modes"] = {{"remove", app.can_remove_for_me},
                     {"deprovision", app.can_deprovision},
                     {"remove_everywhere", app.can_remove_for_everyone}};
    return json;
}

// What a listing without administrator rights, or one cut short, leaves out.
void print_listing_notes(const tools::WindowsAppsList& list) {
    if (!list.all_users) {
        print_error(
            "note: not running as administrator: only your apps are listed, who else has them and "
            "whether they are installed for new users aren't known, and none can be changed\n");
    }
    if (list.error != 0) {
        print_error(
            std::format("note: the listing was cut short (error 0x{:08X}); some apps may be missing\n",
                        static_cast<std::uint32_t>(list.error)));
    }
}

int list_command(const AppsCommandOptions& options) {
    const bool elevated = platform::is_process_elevated();
    const Listing listing = list_apps();
    const auto shown = [&](const WindowsApp& app) {
        return options.all || app.protection == tools::AppProtection::None;
    };

    if (options.json) {
        Json root;
        root["avel_version"] = kVersionString;
        root["elevated"] = elevated;
        root["all_users"] = listing.list.all_users;
        root["include_protected"] = options.all;
        root["error"] = listing.list.error;
        root["elapsed_ms"] = listing.elapsed.count();
        Json apps = Json::array();
        for (const auto& app : listing.list.apps) {
            if (shown(app)) {
                apps.push_back(app_to_json(app));
            }
        }
        root["apps"] = std::move(apps);
        print(root.dump(2) + "\n");
        return 0;
    }
    std::size_t count = 0;
    std::size_t hidden = 0;
    std::size_t often_removed = 0;
    for (const auto& app : listing.list.apps) {
        if (!shown(app)) {
            ++hidden;
            continue;
        }
        print(app_line(app, listing.list.all_users));
        ++count;
        often_removed += app.category == tools::AppCategory::OftenRemoved;
    }
    std::string summary =
        std::format("\n{} app{}, {} often removed", count, count == 1 ? "" : "s", often_removed);
    if (hidden > 0) {
        summary += std::format("; {} protected not shown (--all shows them)", hidden);
    }
    print(summary +
          std::format(". Listed in {:.1f} s.\n", static_cast<double>(listing.elapsed.count()) / 1000.0));
    print_listing_notes(listing.list);
    return 0;
}

const WindowsApp* find_app(const tools::WindowsAppsList& list, const std::wstring& id) {
    const auto found = std::find_if(list.apps.begin(), list.apps.end(), [&](const WindowsApp& app) {
        return platform::iequals(app.id, id) || platform::iequals(app.family_name, id);
    });
    return found != list.apps.end() ? &*found : nullptr;
}

std::string issue_text(const tools::WindowsAppsPlanIssue& issue, AppsCommand command,
                       const tools::WindowsAppsList& list) {
    using Code = tools::WindowsAppsPlanIssueCode;
    std::string text;
    switch (issue.code) {
        case Code::UnknownId:
            text = "no such app; see `avel-cli apps list`";
            break;
        case Code::Protected: {
            const auto* app = find_app(list, issue.id);
            if (app && app->protection == tools::AppProtection::System) {
                text = "signed as part of Windows; it is never changed";
            } else if (app && app->protection == tools::AppProtection::ExternalLocation) {
                text = std::format(
                    "part of the desktop program in {}; it goes when that program is uninstalled, "
                    "never alone",
                    to_utf8(app->external_location));
            } else {
                text = "Windows needs it; it is never changed";
            }
            break;
        }
        case Code::IncompleteListing:
            text =
                "without administrator rights, who else has it and whether it is installed for new users "
                "aren't known; run avel-cli as administrator";
            break;
        case Code::NotInstalledForCurrentUser:
            text = "you don't have it (deprovision and remove-everywhere act on the others)";
            break;
        case Code::WouldDeletePackage:
            text =
                command == AppsCommand::Remove
                    ? "nobody else has it and it isn't installed for new users, so Windows would delete it "
                      "and undo couldn't bring it back; only remove-everywhere --allow-irreversible removes "
                      "it"
                    : "no user has it, so Windows may delete it once it isn't installed for new users, "
                      "and undo couldn't bring it back; only remove-everywhere --allow-irreversible removes "
                      "it";
            break;
        case Code::NotProvisioned:
            text = "it isn't installed for new users";
            break;
        case Code::IrreversibleNeedsConfirmation:
            text = "removing it for everyone can't be undone; add --allow-irreversible";
            break;
    }
    return std::format("{}: {} ({})", to_utf8(issue.id), text, tools::to_string(issue.code));
}

std::string_view command_name(AppsCommand command) {
    switch (command) {
        case AppsCommand::Remove:
            return "remove";
        case AppsCommand::Deprovision:
            return "deprovision";
        case AppsCommand::RemoveEverywhere:
            return "remove-everywhere";
        case AppsCommand::List:
            break;
    }
    return "list";
}

// "Remove Microsoft News for me", "Don't install 3 apps for new users": shown in the Undo Center.
std::wstring describe_plan(AppsCommand command, const std::vector<std::wstring>& planned,
                           const tools::WindowsAppsList& list) {
    std::wstring what = std::format(L"{} apps", planned.size());
    if (planned.size() == 1) {
        if (const auto* app = find_app(list, planned.front())) {
            what = app->display_name;
        }
    }
    switch (command) {
        case AppsCommand::Deprovision:
            return L"Don't install " + what + L" for new users";
        case AppsCommand::RemoveEverywhere:
            return L"Remove " + what + L" for everyone";
        default:
            return L"Remove " + what + L" for me";
    }
}

bool ask(const std::string& question) {
    print(question + " [y/N] ");
    std::string answer;
    std::getline(std::cin, answer);
    return !answer.empty() && (answer.front() == 'y' || answer.front() == 'Y');
}

std::string question(AppsCommand command, std::size_t count) {
    const std::string apps = std::format("{} app{}", count, count == 1 ? "" : "s");
    switch (command) {
        case AppsCommand::Deprovision:
            return "\nStop installing " + apps + " for new users?";
        case AppsCommand::RemoveEverywhere:
            return "\nRemove " + apps +
                   " for every user and delete them from this PC? Every user's data in them is lost, and "
                   "this can't be undone.";
        default:
            return "\nRemove " + apps + " for you?";
    }
}

int action_command(const AppsCommandOptions& options) {
    const bool elevated = platform::is_process_elevated();
    if (!options.dry_run && !elevated) {
        print_error(
            "apps: changing apps needs administrator rights; run avel-cli as administrator (or use "
            "--dry-run)\n");
        return 1;
    }
    std::unique_ptr<changes::ChangeStore> store;
    if (!options.dry_run && !(store = open_change_store())) {  // Also settles interrupted sets
        return 1;
    }

    const Listing listing = list_apps();
    const auto& list = listing.list;
    tools::WindowsAppsPlan planned;
    switch (options.command) {
        case AppsCommand::Deprovision:
            planned = tools::plan_deprovision(list, options.ids);
            break;
        case AppsCommand::RemoveEverywhere:
            planned = tools::plan_remove_for_everyone(list, options.ids, options.allow_irreversible);
            break;
        default:
            planned = tools::plan_remove_for_me(list, options.ids);
            break;
    }
    planned.plan.description = describe_plan(options.command, planned.planned, list);
    const auto& plan = planned.plan;
    const bool refused = !planned.issues.empty();

    if (!options.json) {
        print(options.dry_run ? "Dry run, nothing will be changed.\n" : "");
        for (const auto& id : options.ids) {
            if (const auto* app = find_app(list, id)) {
                print("  " + app_line(*app, list.all_users));
            }
        }
        for (const auto& issue : planned.issues) {
            print("error: " + issue_text(issue, options.command, list) + "\n");
        }
        if (!refused && !plan.items.empty()) {
            print(std::format("\n{}:\n", to_utf8(plan.description)));
            for (const auto& item : plan.items) {
                print(std::format("  {:<20} {}\n", kind_of(item.location),
                                  to_utf8(describe_location(item.location))));
            }
            if (options.command == AppsCommand::RemoveEverywhere) {
                print("This can't be undone: every user's data in these apps is lost.\n");
            }
        }
        print_listing_notes(list);
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
        if (!options.yes && !ask(question(options.command, planned.planned.size()))) {
            print("Cancelled.\n");
            return 1;
        }
        if (const auto guard = store->lock()) {
            result = changes::execute(*store, plan,
                                      cli_execute_options(options.command == AppsCommand::RemoveEverywhere));
            if (result->set_id.empty() ||
                !std::all_of(result->items.begin(), result->items.end(), [](const auto& item) {
                    return item.status.state == changes::ItemState::Done;
                })) {
                exit_code = 1;
            }
        } else {
            print_error("apps: another Avel process is changing the system; nothing was changed\n");
            exit_code = 1;
        }
    }

    if (options.json) {
        Json root;
        root["avel_version"] = kVersionString;
        root["elevated"] = elevated;
        root["all_users"] = list.all_users;
        root["dry_run"] = options.dry_run;
        root["action"] = command_name(options.command);
        Json apps = Json::array();
        for (const auto& id : options.ids) {
            if (const auto* app = find_app(list, id)) {
                apps.push_back(app_to_json(*app));
            }
        }
        root["apps"] = std::move(apps);
        Json issues = Json::array();
        for (const auto& issue : planned.issues) {
            issues.push_back({{"id", to_utf8(issue.id)}, {"code", tools::to_string(issue.code)}});
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
        print(root.dump(2) + "\n");
        return exit_code;
    }
    if (result) {
        print("\n");
        print_execute_result(plan, *result,
                             options.command == AppsCommand::RemoveEverywhere ? ResultWording::Irreversible
                                                                              : ResultWording::StateChange);
    }
    return exit_code;
}

}  // namespace

int run_apps_command(const std::vector<std::wstring_view>& arguments) {
    if (!arguments.empty()) {
        const auto name = arguments.front();
        if (name == L"help" || name == L"--help" || name == L"-h" || name == L"/?") {
            print(kUsage);
            return 0;
        }
    }
    AppsCommandOptions options;
    if (const auto error = parse_apps_options(arguments, options)) {
        print_error(error->message + (error->usage ? std::string(kUsage) : std::string()));
        return 2;
    }
    return options.command == AppsCommand::List ? list_command(options) : action_command(options);
}

}  // namespace avel::cli
