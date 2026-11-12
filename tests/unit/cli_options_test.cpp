// avel-cli's options for uninstall, force, scan and startup (src/cli/command_options.cpp): the
// install trace switches (--no-trace, force --trace), startup's subcommands and options, and the
// usage errors around them.
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

}  // namespace
}  // namespace avel::cli
