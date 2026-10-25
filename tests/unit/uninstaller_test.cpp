// Uninstall logic that runs without uninstalling anything: outcome classification, registration
// lookups for programs that don't exist, dry-run batches and installer detection.
#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
