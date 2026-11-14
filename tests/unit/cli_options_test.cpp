// avel-cli's options for uninstall, force, scan, startup and apps (src/cli/command_options.cpp): the
// install trace switches (--no-trace, force --trace), startup's and apps' subcommands and options, and
// the usage errors around them.
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "cli/command_options.h"

namespace avel::cli {
namespace {

using Arguments = std::vector<std::wstring_view>;

TEST(CliOptionsTest, UninstallUsesTracesUnlessNoTrace) {
    UninstallCommandOptions options;
    EXPECT_FALSE(parse_uninstall_options({L"Contoso Fixture", L"--quiet", L"--yes"}, options));
    EXPECT_TRUE(options.trace);
    EXPECT_EQ(options.programs, std::vector<std::wstring>{L"Contoso Fixture"});

    UninstallCommandOptions without;
    EXPECT_FALSE(parse_uninstall_options({L"Contoso Fixture", L"--no-trace", L"--remove-leftovers",
                                          L"--min-level", L"very-good", L"--yes", L"--json"},
                                         without));
    EXPECT_FALSE(without.trace);
    EXPECT_TRUE(without.remove_leftovers);
    EXPECT_EQ(without.min_level, leftovers::Confidence::VeryGood);
    EXPECT_TRUE(without.json);
}

TEST(CliOptionsTest, UninstallUsageErrorsStayAsTheyWere) {
    UninstallCommandOptions options;
    auto error = parse_uninstall_options({L"--no-trace"}, options);  // No program
    ASSERT_TRUE(error);
    EXPECT_TRUE(error->message.empty());
    EXPECT_TRUE(error->usage);

    options = {};
    error = parse_uninstall_options({L"x", L"--trace", L"id"}, options);  // Only force takes --trace
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "uninstall: unknown option --trace\n");
    EXPECT_TRUE(error->usage);

    options = {};
    error = parse_uninstall_options({L"x", L"--json"}, options);
    ASSERT_TRUE(error);
    EXPECT_FALSE(error->usage);

    options = {};
    error = parse_uninstall_options({L"x", L"--remove-leftovers", L"--dry-run"}, options);
    ASSERT_TRUE(error);
    EXPECT_FALSE(error->usage);

    options = {};
    error = parse_uninstall_options({L"x", L"--min-level", L"bad"}, options);
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "uninstall: --min-level is good or very-good\n");
}

TEST(CliOptionsTest, ForceTakesATraceAsItsTarget) {
    ForceCommandOptions options;
    EXPECT_FALSE(parse_force_options({L"--trace", L"20261025-100000-0000c0de", L"--dry-run"}, options));
    EXPECT_EQ(options.trace_id, L"20261025-100000-0000c0de");
    EXPECT_TRUE(options.programs.empty());
    EXPECT_TRUE(options.trace);
    EXPECT_TRUE(options.dry_run);

    ForceCommandOptions program;
    EXPECT_FALSE(parse_force_options({L"Contoso Fixture", L"--no-trace", L"--yes"}, program));
    EXPECT_FALSE(program.trace);
    EXPECT_TRUE(program.trace_id.empty());
}

TEST(CliOptionsTest, ForceTraceUsageErrors) {
    const auto parse = [](const Arguments& arguments) {
        ForceCommandOptions options;
        return parse_force_options(arguments, options);
    };
    // One target only: a trace and a program, folder or file at once is a usage error.
    for (const Arguments& arguments : {Arguments{L"--trace", L"id", L"Contoso Fixture"},
                                       Arguments{L"--trace", L"id", L"--folder", LR"(C:\X)"},
                                       Arguments{L"--trace", L"id", L"--exe", LR"(C:\X\x.exe)"},
                                       Arguments{L"--trace", L"id", L"--name", L"X"}}) {
        const auto error = parse(arguments);
        ASSERT_TRUE(error);
        EXPECT_TRUE(error->usage);
    }
    // --trace without its id is an unknown option, as --folder without a path is.
    auto error = parse({L"--trace"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "force: unknown option --trace\n");
    EXPECT_TRUE(error->usage);

    error = parse({L"--trace", L"id", L"--no-trace"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "force: --trace can't go with --no-trace\n");
    EXPECT_FALSE(error->usage);

    error = parse({L"--trace", L"id", L"--json"});
    ASSERT_TRUE(error);
    EXPECT_FALSE(error->usage);
}

TEST(CliOptionsTest, ScanShowsTracesUnlessNoTrace) {
    ScanCommandOptions options;
    EXPECT_FALSE(parse_scan_options({L"Contoso Fixture", L"--json"}, options));
    EXPECT_TRUE(options.trace);

    ScanCommandOptions without;
    EXPECT_FALSE(parse_scan_options({L"--all", L"--no-trace", L"--min-level", L"good"}, without));
    EXPECT_FALSE(without.trace);
    EXPECT_TRUE(without.all);
    EXPECT_EQ(without.min_level, leftovers::Confidence::Good);

    ScanCommandOptions both;
    EXPECT_TRUE(parse_scan_options({L"--all", L"x"}, both));
    ScanCommandOptions unknown;
    EXPECT_TRUE(parse_scan_options({L"x", L"--trace"}, unknown));
}

TEST(CliOptionsTest, StartupListAndItsOptions) {
    StartupCommandOptions options;
    EXPECT_FALSE(parse_startup_options({L"list"}, options));
    EXPECT_EQ(options.command, StartupCommand::List);
    EXPECT_FALSE(options.all_users);
    EXPECT_FALSE(options.json);
    EXPECT_FALSE(startup_action(options.command));

    StartupCommandOptions all;
    EXPECT_FALSE(parse_startup_options({L"list", L"--all-users", L"--json"}, all));
    EXPECT_TRUE(all.all_users);
    EXPECT_TRUE(all.json);  // list never asks: --json goes alone
}

TEST(CliOptionsTest, StartupActionsTakeIds) {
    StartupCommandOptions disable;
    EXPECT_FALSE(parse_startup_options(
        {L"disable", L"0123456789", L"abcdef0123", L"--service-start", L"disabled", L"--yes"}, disable));
    EXPECT_EQ(disable.command, StartupCommand::Disable);
    EXPECT_EQ(startup_action(disable.command), tools::StartupAction::Disable);
    EXPECT_EQ(disable.ids, (std::vector<std::wstring>{L"0123456789", L"abcdef0123"}));
    EXPECT_EQ(disable.service_start, tools::ServiceDisable::Disabled);
    EXPECT_TRUE(disable.yes);

    StartupCommandOptions manual;
    EXPECT_FALSE(
        parse_startup_options({L"disable", L"x", L"--service-start", L"manual", L"--dry-run"}, manual));
    EXPECT_EQ(manual.service_start, tools::ServiceDisable::Manual);
    EXPECT_TRUE(manual.dry_run);
    StartupCommandOptions by_default;
    EXPECT_FALSE(parse_startup_options({L"disable", L"x"}, by_default));
    EXPECT_EQ(by_default.service_start, tools::ServiceDisable::Manual);

    StartupCommandOptions enable;
    EXPECT_FALSE(parse_startup_options({L"enable", L"x", L"-y", L"--json"}, enable));
    EXPECT_EQ(startup_action(enable.command), tools::StartupAction::Enable);
    EXPECT_TRUE(enable.yes);
    EXPECT_TRUE(enable.json);

    StartupCommandOptions remove;
    EXPECT_FALSE(parse_startup_options({L"delete", L"x", L"--dry-run", L"--json"}, remove));
    EXPECT_EQ(startup_action(remove.command), tools::StartupAction::Delete);
}

TEST(CliOptionsTest, StartupUsageErrors) {
    const auto parse = [](Arguments arguments) {
        StartupCommandOptions options;
        return parse_startup_options(arguments, options);
    };
    auto error = parse({});
    ASSERT_TRUE(error);
    EXPECT_TRUE(error->usage);

    error = parse({L"stop"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "startup: unknown command stop\n");
    EXPECT_TRUE(error->usage);

    // An action needs ids; list takes none.
    error = parse({L"disable", L"--yes"});
    ASSERT_TRUE(error);
    EXPECT_TRUE(error->usage);
    error = parse({L"list", L"0123456789"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "startup: list takes no ids\n");

    // --service-start only with disable, and only manual or disabled.
    error = parse({L"enable", L"x", L"--service-start", L"manual"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "startup: --service-start doesn't go with enable\n");
    error = parse({L"delete", L"x", L"--service-start", L"disabled"});
    ASSERT_TRUE(error);
    error = parse({L"disable", L"x", L"--service-start", L"automatic"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "startup: --service-start is manual or disabled\n");
    EXPECT_FALSE(error->usage);
    error = parse({L"disable", L"x", L"--service-start"});
    ASSERT_TRUE(error);
    EXPECT_FALSE(error->usage);

    // Options of the other subcommands, and unknown ones.
    error = parse({L"list", L"--yes"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "startup: --yes doesn't go with list\n");
    error = parse({L"list", L"--dry-run"});
    ASSERT_TRUE(error);
    error = parse({L"disable", L"x", L"--all-users"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "startup: --all-users doesn't go with disable\n");
    error = parse({L"list", L"--deep"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "startup: unknown option --deep\n");
    EXPECT_TRUE(error->usage);

    // An action's --json can't ask for confirmation.
    error = parse({L"disable", L"x", L"--json"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "startup: --json needs --yes or --dry-run (it can't ask for confirmation)\n");
    EXPECT_FALSE(error->usage);
}

TEST(CliOptionsTest, AppsListAndItsOptions) {
    AppsCommandOptions options;
    EXPECT_FALSE(parse_apps_options({L"list"}, options));
    EXPECT_EQ(options.command, AppsCommand::List);
    EXPECT_FALSE(options.all);
    EXPECT_FALSE(options.json);

    AppsCommandOptions all;
    EXPECT_FALSE(parse_apps_options({L"list", L"--all", L"--json"}, all));
    EXPECT_TRUE(all.all);
    EXPECT_TRUE(all.json);  // list never asks: --json goes alone
}

TEST(CliOptionsTest, AppsActionsTakeIdsOrFamilies) {
    AppsCommandOptions remove;
    EXPECT_FALSE(parse_apps_options({L"remove", L"0123456789", L"Microsoft.BingNews_8wekyb3d8bbwe", L"--yes"},
                                    remove));
    EXPECT_EQ(remove.command, AppsCommand::Remove);
    EXPECT_EQ(remove.ids, (std::vector<std::wstring>{L"0123456789", L"Microsoft.BingNews_8wekyb3d8bbwe"}));
    EXPECT_TRUE(remove.yes);
    EXPECT_FALSE(remove.allow_irreversible);

    AppsCommandOptions deprovision;
    EXPECT_FALSE(parse_apps_options({L"deprovision", L"x", L"--dry-run", L"--json"}, deprovision));
    EXPECT_EQ(deprovision.command, AppsCommand::Deprovision);
    EXPECT_TRUE(deprovision.dry_run);
    EXPECT_TRUE(deprovision.json);

    AppsCommandOptions everywhere;
    EXPECT_FALSE(parse_apps_options({L"remove-everywhere", L"x", L"--allow-irreversible", L"-y", L"--json"},
                                    everywhere));
    EXPECT_EQ(everywhere.command, AppsCommand::RemoveEverywhere);
    EXPECT_TRUE(everywhere.allow_irreversible);
    EXPECT_TRUE(everywhere.yes);

    // Without the confirmation it parses: the plan refuses it per app (irreversible-needs-confirmation).
    AppsCommandOptions unconfirmed;
    EXPECT_FALSE(parse_apps_options({L"remove-everywhere", L"x", L"--dry-run"}, unconfirmed));
    EXPECT_FALSE(unconfirmed.allow_irreversible);
}

TEST(CliOptionsTest, AppsUsageErrors) {
    const auto parse = [](Arguments arguments) {
        AppsCommandOptions options;
        return parse_apps_options(arguments, options);
    };
    auto error = parse({});
    ASSERT_TRUE(error);
    EXPECT_TRUE(error->usage);

    error = parse({L"uninstall", L"x"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "apps: unknown command uninstall\n");
    EXPECT_TRUE(error->usage);

    // An action needs ids; list takes none.
    error = parse({L"remove", L"--yes"});
    ASSERT_TRUE(error);
    EXPECT_TRUE(error->usage);
    error = parse({L"list", L"0123456789"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "apps: list takes no ids\n");

    // --allow-irreversible only with remove-everywhere; --all only with list.
    error = parse({L"remove", L"x", L"--allow-irreversible"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "apps: --allow-irreversible doesn't go with remove\n");
    error = parse({L"deprovision", L"x", L"--all"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "apps: --all doesn't go with deprovision\n");
    error = parse({L"list", L"--dry-run"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "apps: --dry-run doesn't go with list\n");
    error = parse({L"list", L"--all-users"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "apps: unknown option --all-users\n");
    EXPECT_TRUE(error->usage);

    // An action's --json can't ask for confirmation.
    error = parse({L"remove", L"x", L"--json"});
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "apps: --json needs --yes or --dry-run (it can't ask for confirmation)\n");
    EXPECT_FALSE(error->usage);
}

TEST(CliOptionsTest, ExtensionsListAndItsFilters) {
    ExtensionsCommandOptions options;
    EXPECT_FALSE(parse_extensions_options({L"list"}, options));
    EXPECT_EQ(options.command, ExtensionsCommand::List);
    EXPECT_FALSE(options.all_users);
    EXPECT_FALSE(options.removable);
    EXPECT_FALSE(options.browser);

    ExtensionsCommandOptions filtered;
    EXPECT_FALSE(parse_extensions_options(
        {L"list", L"--all-users", L"--removable", L"--browser", L"opera-gx", L"--json"}, filtered));
    EXPECT_TRUE(filtered.all_users);
    EXPECT_TRUE(filtered.removable);
    EXPECT_EQ(filtered.browser, tools::BrowserKind::OperaGx);
    EXPECT_TRUE(filtered.json);
}

TEST(CliOptionsTest, ExtensionsRemoveTakesIds) {
    ExtensionsCommandOptions remove;
    EXPECT_FALSE(parse_extensions_options(
        {L"remove", L"0123456789", L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", L"--dry-run", L"--json"}, remove));
    EXPECT_EQ(remove.command, ExtensionsCommand::Remove);
    EXPECT_EQ(remove.ids, (std::vector<std::wstring>{L"0123456789", L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}));
    EXPECT_TRUE(remove.dry_run);
    EXPECT_TRUE(remove.json);

    const auto error = [](std::vector<std::wstring_view> arguments) {
        ExtensionsCommandOptions options;
        return parse_extensions_options(arguments, options).has_value();
    };
    EXPECT_TRUE(error({}));
    EXPECT_TRUE(error({L"delete", L"x"}));
    EXPECT_TRUE(error({L"remove"}));                        // No ids
    EXPECT_TRUE(error({L"remove", L"x", L"--json"}));       // Can't ask
    EXPECT_TRUE(error({L"remove", L"x", L"--removable"}));  // A list filter
    EXPECT_TRUE(error({L"list", L"x"}));
    EXPECT_TRUE(error({L"list", L"--browser"}));
    EXPECT_TRUE(error({L"list", L"--browser", L"netscape"}));
    EXPECT_TRUE(error({L"list", L"--dry-run"}));
    EXPECT_FALSE(error({L"remove", L"x", L"--yes", L"--json"}));
}

}  // namespace
}  // namespace avel::cli
