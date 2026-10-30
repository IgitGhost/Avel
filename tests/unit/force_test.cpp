// Avel Force's targets: a folder no Uninstall entry describes, the program a file belongs to, and
// the processes running from a program's folder (one of them a real helper process it ends).
#include <gtest/gtest.h>

#include <windows.h>

#include <wil/resource.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/force/force.h"
#include "core/platform/known_folders.h"
#include "core/platform/paths.h"
#include "core/platform/processes.h"
#include "temp_root.h"
#include "test_spawner.h"

namespace avel::force {
namespace {

namespace fs = std::filesystem;
using leftovers::ProtectionReason;
using leftovers::ScanEnvironment;
using leftovers::ScanTarget;

class ForceTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = testing::long_temp_directory() /
                (L"avel-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-force");
        fs::remove_all(root_);
        fs::create_directories(root_ / LR"(Contoso\Fixture\bin)");
        fs::create_directories(root_ / LR"(Elsewhere\tool)");
        std::ofstream(root_ / LR"(Contoso\Fixture\bin\app.exe)") << "fixture";
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    std::wstring path(std::wstring_view relative) const {
        return platform::normalize_path((root_ / relative).wstring());
    }

    static inventory::Program program(std::wstring id, std::wstring location) {
        inventory::Program program;
        program.id = std::move(id);
        program.display_name = program.id;
        program.install_location = std::move(location);
        return program;
    }

    ScanEnvironment environment_;
    fs::path root_;
};

TEST_F(ForceTest, FolderTargetsAFolderAndWhatIsNamedAfterIt) {
    const auto result = folder_target(environment_, path(LR"(Contoso\Fixture)"));
    ASSERT_EQ(result.error, FolderError::None);
    EXPECT_EQ(result.target.program_id, L"folder:" + path(LR"(Contoso\Fixture)"));
    EXPECT_EQ(result.target.display_name, L"Fixture");
    EXPECT_EQ(result.target.install_location, path(LR"(Contoso\Fixture)"));
    EXPECT_EQ(result.target.scope, inventory::Scope::Machine);

    const auto named =
        folder_target(environment_, path(LR"(Contoso\Fixture)"), L"Contoso Fixture", L"Contoso");
    EXPECT_EQ(named.target.display_name, L"Contoso Fixture");
    EXPECT_EQ(named.target.names.publisher, L"Contoso");
}

TEST_F(ForceTest, FolderTargetRefusesWhatIsntAProgramsFolder) {
    EXPECT_EQ(folder_target(environment_, L"Contoso").error, FolderError::NotAbsolute);
    EXPECT_EQ(folder_target(environment_, path(L"Missing")).error, FolderError::NotFound);
    EXPECT_EQ(folder_target(environment_, path(LR"(Contoso\Fixture\bin\app.exe)")).error,
              FolderError::NotAFolder);
    const auto machine = platform::machine_folders();
    const auto program_files = folder_target(environment_, machine.program_files);
    EXPECT_EQ(program_files.error, FolderError::Protected);
    EXPECT_EQ(program_files.protection, ProtectionReason::ProgramFiles);
    EXPECT_EQ(folder_target(environment_, machine.windows + LR"(\System32)").error, FolderError::Protected);
}

TEST_F(ForceTest, OwnerIsTheProgramWithTheDeepestFolder) {
    const std::vector<inventory::Program> programs = {
        program(L"publisher", path(L"Contoso")),
        program(L"fixture", path(LR"(Contoso\Fixture)")),
        program(L"broken", platform::machine_folders().program_files),  // Records Program Files itself
    };
    const auto owner = owner_of(environment_, programs, path(LR"(Contoso\Fixture\bin\app.exe)"));
    ASSERT_NE(owner.program, nullptr);
    EXPECT_EQ(owner.program->id, L"fixture");

    // A protected folder never makes a program the owner.
    const auto none =
        owner_of(environment_, programs, platform::machine_folders().program_files + LR"(\Zzz\x.exe)");
    EXPECT_EQ(none.program, nullptr);
}

TEST_F(ForceTest, OwnerPrefersTheEntrySettingsLists) {
    auto hidden = program(L"hidden", path(LR"(Contoso\Fixture)"));
    hidden.system_component = true;
    const std::vector<inventory::Program> programs = {hidden,
                                                      program(L"listed", path(LR"(Contoso\Fixture)"))};
    const auto owner = owner_of(environment_, programs, path(LR"(Contoso\Fixture\bin\app.exe)"));
    ASSERT_NE(owner.program, nullptr);
    EXPECT_EQ(owner.program->id, L"listed");
}

TEST_F(ForceTest, WithoutAnOwnerTheFilesFolderIsTheTarget) {
    const auto owner = owner_of(environment_, {}, path(LR"(Elsewhere\tool\x.exe)"));
    EXPECT_EQ(owner.program, nullptr);
    EXPECT_EQ(owner.folder, path(LR"(Elsewhere\tool)"));
    EXPECT_EQ(owner.name, L"tool");  // No version resource to name it
}

TEST_F(ForceTest, ProcessesRunningFromTheTargetsFolders) {
    ScanTarget fixture;
    fixture.install_location = path(LR"(Contoso\Fixture)");
    ScanTarget broken;
    broken.install_location = platform::machine_folders().program_files;  // Protected: matches nothing
    const std::vector<ScanTarget> targets = {fixture, broken};

    const auto inside = path(LR"(Contoso\Fixture\bin\app.exe)");
    const std::vector<platform::ProcessInfo> processes = {
        {100, 1, 1, L"app.exe", inside, false},
        {101, 1, 1, L"other.exe", path(LR"(Elsewhere\tool\other.exe)"), false},
        {102, 1, 1, L"app.exe", inside, true},   // Critical
        {103, 1, 0, L"app.exe", inside, false},  // A service host
        {static_cast<std::uint32_t>(::GetCurrentProcessId()), 1, 1, L"avel.exe", inside, false},
        {104, 1, 1, L"x.exe", platform::machine_folders().program_files + LR"(\Zzz\x.exe)", false},
        {105, 1, 1, L"System", L"", false},
    };
    const std::vector<std::uint32_t> services = {103};
    const auto found = processes_in(environment_, targets, processes, services);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found.front().pid, 100u);
}

TEST_F(ForceTest, FindsAndEndsARealProcess) {
    const std::wstring spawner = testing::spawner_path();
    const std::wstring copy = path(LR"(Contoso\Fixture\bin\fixture-app.exe)");
    ASSERT_TRUE(::CopyFileW(spawner.c_str(), copy.c_str(), FALSE));
    std::wstring command = L"\"" + copy + L"\" child 60000 \"" + path(L"marker") + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    ASSERT_TRUE(::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &startup, &info));
    wil::unique_handle process(info.hProcess);
    wil::unique_handle thread(info.hThread);

    ScanTarget target;
    target.install_location = path(LR"(Contoso\Fixture)");
    const auto found = processes_in(environment_, std::vector<ScanTarget>{target},
                                    platform::running_processes(), platform::service_process_ids());
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found.front().pid, info.dwProcessId);
    EXPECT_TRUE(platform::paths_equal(found.front().image, copy));

    const auto stopped = platform::stop_process(info.dwProcessId, std::chrono::milliseconds(500));
    EXPECT_EQ(stopped.outcome, platform::StopOutcome::Terminated);
    EXPECT_EQ(::WaitForSingleObject(process.get(), 0), WAIT_OBJECT_0);
    EXPECT_EQ(platform::stop_process(info.dwProcessId).outcome, platform::StopOutcome::Gone);
}

}  // namespace
}  // namespace avel::force
