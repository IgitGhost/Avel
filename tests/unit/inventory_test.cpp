#include <gtest/gtest.h>

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "core/inventory/inventory.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"
#include "core/uninstall/installer_detection.h"
#include "core/uninstall/uninstall_plan.h"

namespace avel::inventory {
namespace {

constexpr wchar_t kMe[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kSomeoneElse[] = L"S-1-5-21-1-2-3-1002";

Program registry_entry(std::wstring key, std::wstring name, bool windows_installer = false) {
    Program program;
    program.source = Source::Registry;
    program.display_name = std::move(name);
    program.registry = RegistryLocation{RegistryView::Machine64, key, L"HKLM\\...\\" + key};
    program.uninstall_string = L"uninstall " + key;
    program.windows_installer = windows_installer;
    if (windows_installer) {
        program.installer_type = InstallerType::Msi;
        program.msi_product_code = key;
    }
    return program;
}

Program msi_product(std::wstring code, std::wstring name, std::wstring user_sid = {}) {
    Program program;
    program.source = Source::Msi;
    program.installer_type = InstallerType::Msi;
    program.msi_product_code = std::move(code);
    program.display_name = std::move(name);
    program.scope = user_sid.empty() ? Scope::Machine : Scope::User;
    program.user_sid = std::move(user_sid);
    program.uninstall_string = L"MsiExec.exe /X" + program.msi_product_code;
    return program;
}

constexpr wchar_t kCodeA[] = L"{AAAAAAAA-0000-0000-0000-000000000000}";
constexpr wchar_t kCodeB[] = L"{BBBBBBBB-0000-0000-0000-000000000000}";

TEST(MergeMsiProductsTest, MatchesByProductCodeAndFillsGaps) {
    std::vector<Program> programs = {registry_entry(kCodeA, L"Alpha", true)};
    Program product = msi_product(kCodeA, L"Alpha");
    product.install_location = LR"(C:\Alpha)";
    product.publisher = L"Alpha Corp";
    merge_msi_products(programs, {product}, kMe, true);

    ASSERT_EQ(programs.size(), 1u);
    EXPECT_EQ(programs[0].install_location, LR"(C:\Alpha)");
    EXPECT_EQ(programs[0].publisher, L"Alpha Corp");
    EXPECT_FALSE(programs[0].msi_product_unregistered);
}

TEST(MergeMsiProductsTest, NeverOverwritesAnExistingUninstallCommand) {
    std::vector<Program> programs = {registry_entry(L"Bundle", L"Contoso Suite 2.0")};
    programs[0].publisher = L"Contoso Ltd";
    programs[0].display_version = L"2.0.0";
    Program component = msi_product(kCodeB, L"Contoso Suite 2.0");
    component.publisher = L"Contoso Ltd";
    component.display_version = L"2.0.0";
    component.install_location = LR"(C:\Program Files\Contoso Suite 2.0)";
    merge_msi_products(programs, {component}, kMe, true);

    ASSERT_EQ(programs.size(), 1u);  // Related by name, publisher and version
    EXPECT_EQ(programs[0].uninstall_string, L"uninstall Bundle");
    EXPECT_TRUE(programs[0].msi_product_code.empty());
    EXPECT_EQ(programs[0].install_location, LR"(C:\Program Files\Contoso Suite 2.0)");
}

TEST(MergeMsiProductsTest, UnmatchedProductsAreAppendedAndHidden) {
    std::vector<Program> programs = {registry_entry(L"Chrome", L"Google Chrome")};
    merge_msi_products(programs, {msi_product(kCodeB, L"Internal Component")}, kMe, true);

    ASSERT_EQ(programs.size(), 2u);
    EXPECT_EQ(programs[1].source, Source::Msi);
    EXPECT_FALSE(is_listed_in_settings(programs[1]));
}

TEST(MergeMsiProductsTest, FlagsWindowsInstallerEntriesUnknownToWindowsInstaller) {
    std::vector<Program> programs = {registry_entry(kCodeA, L"Installed", true),
                                     registry_entry(kCodeB, L"Gone", true)};
    // The product B is registered, but only for another user.
    merge_msi_products(
        programs, {msi_product(kCodeA, L"Installed"), msi_product(kCodeB, L"Gone", kSomeoneElse)}, kMe, true);

    EXPECT_FALSE(programs[0].msi_product_unregistered);
    EXPECT_TRUE(programs[1].msi_product_unregistered);
    EXPECT_FALSE(is_listed_in_settings(programs[1]));
}

TEST(MergeMsiProductsTest, NoFlagsWhenTheMsiScanFailed) {
    std::vector<Program> programs = {registry_entry(kCodeA, L"Unknown", true)};
    merge_msi_products(programs, {}, kMe, false);
    EXPECT_FALSE(programs[0].msi_product_unregistered);
}

// Reads (never modifies) the real machine: every source must work and find something.
TEST(InventorySmokeTest, ScansThisMachine) {
    const auto inventory = scan_inventory();
    EXPECT_FALSE(inventory.programs.empty());
    ASSERT_EQ(inventory.timings.size(), 3u);
    for (const auto& program : inventory.programs) {
        EXPECT_FALSE(program.id.empty());
    }
    for (const auto& diagnostic : inventory.diagnostics) {
        EXPECT_NE(diagnostic.code, DiagnosticCode::AppxEnumerationFailed);
        EXPECT_NE(diagnostic.code, DiagnosticCode::MsiEnumerationFailed);
    }
}

// Plans (never runs) the removal of every program Settings lists on this machine, as if elevated.
// Each must either get a plan or have a reason; prints how many can be removed silently.
TEST(InventorySmokeTest, PlansTheRemovalOfEveryListedProgram) {
    const auto inventory = scan_inventory();
    uninstall::PlanContext context;
    context.current_user_sid = platform::current_user_sid();
    context.elevated = true;
    context.detect_installer = uninstall::detect_installer_type;

    std::map<std::string, int> summary;
    for (const auto& program : inventory.programs) {
        if (!is_listed_in_settings(program)) {
            continue;
        }
        const auto plan = uninstall::make_uninstall_plan(program, uninstall::Mode::Quiet, context);
        if (!plan.possible()) {
            EXPECT_NE(plan.error, uninstall::PlanError::ElevationRequired);
            summary["not possible: " + std::string(uninstall::to_string(plan.error))]++;
            std::cout << "  not possible (" << uninstall::to_string(plan.error)
                      << "): " << platform::to_utf8(program.display_name) << "\n";
            continue;
        }
        EXPECT_TRUE(plan.method == uninstall::Method::Package || !plan.executable.empty())
            << platform::to_utf8(program.display_name);
        summary[std::string(plan.quiet ? "quiet " : "interactive ") +
                std::string(to_string(plan.installer_type))]++;
        if (!plan.quiet) {
            std::cout << "  interactive only: " << platform::to_utf8(program.display_name) << " | "
                      << platform::to_utf8(plan.command_line()) << "\n";
        }
    }
    for (const auto& [kind, count] : summary) {
        std::cout << "  " << count << " x " << kind << "\n";
    }
}

}  // namespace
}  // namespace avel::inventory
