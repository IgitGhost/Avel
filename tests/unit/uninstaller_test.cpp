// Uninstall logic that runs without uninstalling anything: outcome classification, registration
// lookups for programs that don't exist, dry-run batches, packaged apps' soft removal on injected
// facts and installer detection.
#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

#include "core/leftovers/leftover.h"
#include "core/uninstall/batch.h"
#include "core/uninstall/installer_detection.h"
#include "core/uninstall/registration.h"
#include "core/uninstall/uninstaller.h"
#include "test_spawner.h"

namespace avel::uninstall {
namespace {

using inventory::InstallerType;
using inventory::Program;
using inventory::RegistryLocation;
using inventory::RegistryView;

TEST(UninstallerTest, ClassifiesExitCodes) {
    EXPECT_EQ(classify(0, false), Outcome::Removed);
    EXPECT_EQ(classify(0, true), Outcome::StillInstalled);
    EXPECT_EQ(classify(kMsiUserCancelled, true), Outcome::StillInstalled);
    EXPECT_EQ(classify(kMsiUnknownProduct, false), Outcome::Removed);
    EXPECT_EQ(classify(kRebootRequired, false), Outcome::RebootRequired);
    EXPECT_EQ(classify(kRebootRequired, true), Outcome::RebootRequired);
    EXPECT_EQ(classify(kRebootInitiated, false), Outcome::RebootRequired);
    EXPECT_EQ(classify(1, false), Outcome::Removed);  // What counts is that the registration is gone
}

TEST(UninstallerTest, UninstallKeyAddresses) {
    const auto machine64 = uninstall_key_address({RegistryView::Machine64, L"7-Zip", L""}, L"");
    ASSERT_TRUE(machine64.has_value());
    EXPECT_EQ(machine64->root, HKEY_LOCAL_MACHINE);
    EXPECT_EQ(machine64->path, LR"(Software\Microsoft\Windows\CurrentVersion\Uninstall\7-Zip)");
    EXPECT_EQ(machine64->view, static_cast<REGSAM>(KEY_WOW64_64KEY));

    const auto machine32 = uninstall_key_address({RegistryView::Machine32, L"App", L""}, L"");
    ASSERT_TRUE(machine32.has_value());
    EXPECT_EQ(machine32->path, LR"(Software\Microsoft\Windows\CurrentVersion\Uninstall\App)");
    EXPECT_EQ(machine32->view, static_cast<REGSAM>(KEY_WOW64_32KEY));

    const auto user = uninstall_key_address({RegistryView::User, L"Discord", L""}, L"S-1-5-21-1-2-3-1001");
    ASSERT_TRUE(user.has_value());
    EXPECT_EQ(user->root, HKEY_USERS);
    EXPECT_EQ(user->path,
              LR"(S-1-5-21-1-2-3-1001\Software\Microsoft\Windows\CurrentVersion\Uninstall\Discord)");

    const auto user32 = uninstall_key_address({RegistryView::User32, L"Old", L""}, L"S-1-5-21-1-2-3-1001");
    ASSERT_TRUE(user32.has_value());
    EXPECT_EQ(user32->path,
              LR"(S-1-5-21-1-2-3-1001\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Old)");

    EXPECT_FALSE(uninstall_key_address({RegistryView::User, L"Discord", L""}, L"").has_value());
}

TEST(UninstallerTest, ProgramsThatDontExistAreNotRegistered) {
    Program msi;
    msi.source = inventory::Source::Msi;
    msi.msi_product_code = L"{A7E1A7E1-0000-4000-8000-0000000000A1}";
    EXPECT_FALSE(is_still_registered(msi));

    Program registry;
    registry.registry =
        RegistryLocation{RegistryView::Machine64, L"Avel.Tests.{A7E1A7E1-0000-4000-8000-0000000000A2}", L""};
    EXPECT_FALSE(is_still_registered(registry));

    EXPECT_FALSE(is_package_installed(L"Avel.Tests.Nonexistent_1.0.0.0_x64__8wekyb3d8bbwe"));

    Program no_folder;
    no_folder.install_location = LR"(C:\Avel.Tests\does-not-exist)";
    EXPECT_FALSE(install_location_exists(no_folder));
}

TEST(UninstallerTest, DryRunBatchRunsNothing) {
    Program possible;
    possible.display_name = L"Possible";
    Program impossible;
    impossible.display_name = L"Impossible";

    std::vector<BatchItem> items(2);
    items[0].program = &possible;
    items[0].plan.executable = LR"(C:\Avel.Tests\would-run.exe)";
    items[1].program = &impossible;
    items[1].plan.error = PlanError::RemovalBlocked;

    BatchOptions options;
    options.dry_run = true;
    options.restore_point = true;  // Never created in a dry run
    std::vector<std::string> events;
    BatchEvents callbacks;
    callbacks.restore_point_done = [&](RestorePointStatus, std::uint32_t) {
        events.push_back("restore point");
    };
    callbacks.item_started = [&](std::size_t index) { events.push_back("start " + std::to_string(index)); };
    callbacks.item_finished = [&](std::size_t index, const UninstallResult&) {
        events.push_back("finish " + std::to_string(index));
    };
    Cancellation cancellation;
    const BatchResult result = run_batch(items, options, callbacks, cancellation);

    EXPECT_EQ(result.restore_point, RestorePointStatus::NotRequested);
    ASSERT_EQ(result.results.size(), 2u);
    EXPECT_EQ(result.results[0].outcome, Outcome::Simulated);
    EXPECT_EQ(result.results[1].outcome, Outcome::NotPossible);
    EXPECT_EQ(result.results[1].plan_error, PlanError::RemovalBlocked);
    EXPECT_EQ(events, (std::vector<std::string>{"start 0", "finish 0", "start 1", "finish 1"}));
}

TEST(UninstallerTest, StoppedBatchSkipsTheRest) {
    Program program;
    std::vector<BatchItem> items(3);
    for (auto& item : items) {
        item.program = &program;
        item.plan.executable = LR"(C:\Avel.Tests\would-run.exe)";
    }
    BatchOptions options;
    options.dry_run = true;
    Cancellation cancellation;
    BatchEvents callbacks;
    callbacks.item_finished = [&](std::size_t index, const UninstallResult&) {
        if (index == 0) {
            cancellation.stop_batch = true;
        }
    };
    const BatchResult result = run_batch(items, options, callbacks, cancellation);
    EXPECT_EQ(result.results[0].outcome, Outcome::Simulated);
    EXPECT_EQ(result.results[1].outcome, Outcome::Cancelled);
    EXPECT_EQ(result.results[2].outcome, Outcome::Cancelled);
}

// A packaged app's uninstall becomes a soft removal through a ChangeSet only when undo can bring it
// back (docs/uninstall.md, "Packaged apps"). Nothing here touches a real package.
constexpr wchar_t kMe[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kOther[] = L"S-1-5-21-1-2-3-1002";

Program packaged_app() {
    Program program;
    program.source = inventory::Source::Appx;
    program.display_name = L"Contoso News";
    program.package_full_name = L"Avel.Tests.News_1.2.3.0_x64__8wekyb3d8bbwe";
    program.package_family_name = L"Avel.Tests.News_8wekyb3d8bbwe";
    return program;
}

UninstallPlan package_plan(const Program& program) {
    UninstallPlan plan;
    plan.method = Method::Package;
    plan.quiet = true;
    plan.package_full_name = program.package_full_name;
    return plan;
}

TEST(UninstallerTest, PackagedAppIsRemovedSoftlyOnlyWhenUndoable) {
    const Program program = packaged_app();
    const UninstallPlan plan = package_plan(program);

    // Another user keeps it on this PC.
    const auto shared = soft_removal_plan(program, plan, {kMe, {kMe, kOther}, false});
    ASSERT_TRUE(shared);
    ASSERT_EQ(shared->items.size(), 1u);
    const auto* item = std::get_if<leftovers::PackageUserLocation>(&shared->items[0].location);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->family_name, L"Avel.Tests.News_8wekyb3d8bbwe");
    EXPECT_EQ(item->full_name, L"Avel.Tests.News_1.2.3.0_x64__8wekyb3d8bbwe");
    EXPECT_EQ(item->user_sid, kMe);
    EXPECT_TRUE(item->before);
    EXPECT_FALSE(item->after);
    EXPECT_EQ(shared->items[0].programs, std::vector<std::wstring>{L"Contoso News"});
    EXPECT_TRUE(shared->description.empty());  // The front end's to set

    // Provisioned for new users keeps it too; SIDs compare ignoring case.
    EXPECT_TRUE(soft_removal_plan(program, plan, {kMe, {L"s-1-5-21-1-2-3-1001"}, true}));

    // The current user alone, not provisioned: Windows would delete the package.
    EXPECT_FALSE(soft_removal_plan(program, plan, {kMe, {kMe}, false}));
    // The current user doesn't have it, or isn't known.
    EXPECT_FALSE(soft_removal_plan(program, plan, {kMe, {kOther}, true}));
    EXPECT_FALSE(soft_removal_plan(program, plan, {L"", {kMe, kOther}, true}));

    // Not a package removal, or not possible.
    UninstallPlan command = plan;
    command.method = Method::Command;
    EXPECT_FALSE(soft_removal_plan(program, command, {kMe, {kMe, kOther}, true}));
    UninstallPlan blocked = plan;
    blocked.error = PlanError::RemovalBlocked;
    EXPECT_FALSE(soft_removal_plan(program, blocked, {kMe, {kMe, kOther}, true}));

    // The family comes from the full name when the inventory had none.
    Program unnamed = program;
    unnamed.package_family_name.clear();
    const auto from_full_name = soft_removal_plan(unnamed, plan, {kMe, {kMe, kOther}, false});
    ASSERT_TRUE(from_full_name);
    EXPECT_EQ(std::get<leftovers::PackageUserLocation>(from_full_name->items[0].location).family_name,
              L"Avel.Tests.News_8wekyb3d8bbwe");
}

TEST(UninstallerTest, BatchRunsASoftRemovalAsAChangeSet) {
    const Program program = packaged_app();
    std::vector<BatchItem> items(1);
    items[0].program = &program;
    items[0].plan = package_plan(program);

    BatchOptions options;
    options.restore_point = false;
    std::vector<std::wstring> asked;
    options.package_facts = [&](const std::wstring& family) {
        asked.push_back(family);
        return PackageFacts{kMe, {kMe, kOther}, false};
    };
    std::vector<changes::ChangePlan> ran;
    options.run_change_set = [&](changes::ChangePlan plan, const Program& of) {
        EXPECT_EQ(&of, &program);
        ran.push_back(std::move(plan));
        changes::ExecuteResult result;
        result.set_id = L"20261114-120000-abcd";
        result.items.resize(1);
        result.items[0].status.state = changes::ItemState::Done;
        return result;
    };
    Cancellation cancellation;
    const BatchResult batch = run_batch(items, options, {}, cancellation);

    EXPECT_EQ(asked, std::vector<std::wstring>{L"Avel.Tests.News_8wekyb3d8bbwe"});
    ASSERT_EQ(ran.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<leftovers::PackageUserLocation>(ran[0].items[0].location));
    ASSERT_EQ(batch.results.size(), 1u);
    EXPECT_EQ(batch.results[0].outcome, Outcome::Removed);
    EXPECT_EQ(batch.results[0].change_set, L"20261114-120000-abcd");

    // A set that couldn't be written (another Avel process held the store) changed nothing.
    options.run_change_set = [](changes::ChangePlan, const Program&) { return changes::ExecuteResult{}; };
    const BatchResult busy = run_batch(items, options, {}, cancellation);
    EXPECT_EQ(busy.results[0].outcome, Outcome::Failed);
    EXPECT_TRUE(busy.results[0].change_set.empty());

    // A dry run asks nothing and runs no set.
    options.dry_run = true;
    asked.clear();
    ran.clear();
    options.run_change_set = [&](changes::ChangePlan plan, const Program&) {
        ran.push_back(std::move(plan));
        return changes::ExecuteResult{};
    };
    const BatchResult simulated = run_batch(items, options, {}, cancellation);
    EXPECT_EQ(simulated.results[0].outcome, Outcome::Simulated);
    EXPECT_TRUE(asked.empty());
    EXPECT_TRUE(ran.empty());
}

TEST(InstallerDetectionTest, RecognizesInstallerManifests) {
    EXPECT_EQ(installer_type_from_manifest(
                  R"(<assembly><assemblyIdentity version="1.0.0.0" processorArchitecture="*" )"
                  R"(name="Nullsoft.NSIS.exehead" type="win32"/><description>Nullsoft Install System v3.10)"
                  R"(</description></assembly>)"),
              InstallerType::Nsis);
    EXPECT_EQ(
        installer_type_from_manifest(
            R"(<assembly><assemblyIdentity type="win32" name="JR.Inno.Setup" version="1.0.0.0"/></assembly>)"),
        InstallerType::InnoSetup);
    EXPECT_EQ(installer_type_from_manifest(R"(<assembly><assemblyIdentity name="Contoso.App"/></assembly>)"),
              InstallerType::Unknown);
    EXPECT_EQ(installer_type_from_manifest(""), InstallerType::Unknown);
}

TEST(InstallerDetectionTest, ReadsTheManifestOfARealExecutable) {
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    const auto manifest = read_embedded_manifest(self);
    ASSERT_TRUE(manifest.has_value());
    EXPECT_NE(manifest->find("assembly"), std::string::npos);
    // Not this test binary: it contains the NSIS signature as a string literal.
    const std::wstring spawner = testing::spawner_path();
    EXPECT_EQ(detect_installer_type(spawner), InstallerType::Unknown);

    EXPECT_FALSE(read_embedded_manifest(LR"(C:\Avel.Tests\does-not-exist.exe)").has_value());
}

TEST(InstallerDetectionTest, FindsTheNsisSignatureWithoutAManifest) {
    const auto path = std::filesystem::temp_directory_path() /
                      (L"avel-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-nsis.exe");
    {
        // 3 MB of filler with the signature straddling the 1 MB read chunks.
        std::string data(3 * 1024 * 1024, 'x');
        data.replace((1 << 20) - 5, 12, "NullsoftInst");
        std::ofstream(path, std::ios::binary) << data;
    }
    EXPECT_EQ(detect_installer_type(path.wstring()), InstallerType::Nsis);
    {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << std::string(4096, 'x');
    }
    EXPECT_EQ(detect_installer_type(path.wstring()), InstallerType::Unknown);
    std::filesystem::remove(path);
}

}  // namespace
}  // namespace avel::uninstall
