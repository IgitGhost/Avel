// The orphan finder, on a temporary folder tree standing in for Program Files, ProgramData and two
// users' AppData\Local\Programs, with scratch registry keys for HKLM and a user's hive.
#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/leftovers/orphans.h"
#include "scratch_registry.h"
#include "temp_root.h"

namespace avel::leftovers {
namespace {

namespace fs = std::filesystem;

constexpr wchar_t kMe[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kOther[] = L"S-1-5-21-1-2-3-1002";

class OrphansTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = testing::long_temp_directory() /
                (L"avel-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-orphans");
        fs::remove_all(root_);
        for (const auto* folder : {
                 LR"(PF\Litware Orphan)",                      // Nobody's
                 LR"(PF\Contoso Fixture)",                     // Named like an installed program
                 LR"(PF\Contoso Updates)",                     // Holds its publisher's name
                 LR"(PF\Fabrikam)",                            // Named after an installed program's publisher
                 LR"(PF\Suite\Editor)",                        // Holds an installed program's folder
                 LR"(PF\Runner)",                              // A Run value starts a file in it
                 LR"(PF\Seen Once)",                           // Only Explorer's MuiCache names a file in it
                 LR"(PF\Packages)",                            // Windows' own
                 LR"(PD\Litware Data)",                        // Nobody's
                 LR"(Users\me\Local\Programs\Old Tool)",       // Nobody's, per user
                 LR"(Users\me\Local\Saved Games)",             // AppData itself isn't searched
                 LR"(Users\other\Local\Programs\Their Tool)",  // Another user's
             }) {
            fs::create_directories(root_ / folder);
        }
        machine_.set_string(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)", L"Runner",
                            L"\"" + (root_ / LR"(PF\Runner\run.exe)").wstring() + L"\" /background");
        me_.set_string(LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache)",
                       (root_ / LR"(PF\Seen Once\app.exe)").wstring() + L".FriendlyAppName", L"App");
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    ScanResult find() {
        ScanEnvironment environment;
        environment.machine = machine_.get();
        wchar_t windows[MAX_PATH] = {};
        ::GetWindowsDirectoryW(windows, MAX_PATH);
        environment.machine_folders.windows = windows;
        environment.machine_folders.program_files = (root_ / L"PF").wstring();
        environment.machine_folders.program_data = (root_ / L"PD").wstring();
        environment.machine_folders.all = {environment.machine_folders.program_files,
                                           environment.machine_folders.program_data};
        for (const auto* sid : {kMe, kOther}) {
            const bool me = sid == std::wstring_view(kMe);
            UserScope user;
            user.profile = {sid, me ? L"PC\\me" : L"PC\\other",
                            (root_ / L"Users" / (me ? L"me" : L"other")).wstring(), false};
            user.folders.local_app_data = (root_ / L"Users" / (me ? L"me" : L"other") / L"Local").wstring();
            user.hive = me ? me_.get() : other_.get();
            environment.users.push_back(std::move(user));
        }
        environment.current_user_sid = kMe;
        environment.tasks = std::vector<ScheduledTask>{};

        std::vector<inventory::Program> installed(3);
        installed[0].id = L"reg:HKLM64:ContosoFixture";
        installed[0].display_name = L"Contoso Fixture 1.0";
        installed[0].publisher = L"Contoso Ltd.";  // Owns "Contoso Updates" though BCU keeps "Ltd"
        installed[1].id = L"reg:HKLM64:Paint";
        installed[1].display_name = L"Paint Studio";
        installed[1].publisher = L"Fabrikam, Inc.";
        installed[2].id = L"reg:HKLM64:Editor";
        installed[2].display_name = L"Text Editor";
        installed[2].install_location = (root_ / LR"(PF\Suite\Editor)").wstring();
        return find_orphans(environment, installed);
    }

    std::vector<std::pair<std::wstring, Confidence>> found(const ScanResult& result) const {
        std::vector<std::pair<std::wstring, Confidence>> items;
        for (const auto& item : result.leftovers) {
            EXPECT_EQ(item.found_by, Scanner::Orphans);
            const auto& path = std::get<FileLocation>(item.location).path;
            EXPECT_EQ(item.owner_ids, std::vector<std::wstring>{L"folder:" + path});
            items.emplace_back(fs::path(path).lexically_relative(root_).wstring(), level(item.records));
        }
        std::sort(items.begin(), items.end());
        return items;
    }

    fs::path root_;
    testing::ScratchKey machine_;
    testing::ScratchKey me_;
    testing::ScratchKey other_;
};

TEST_F(OrphansTest, FindsFoldersNoInstalledProgramOwns) {
    const std::vector<std::pair<std::wstring, Confidence>> expected = {
        {LR"(PD\Litware Data)", Confidence::Questionable},
        {LR"(PF\Litware Orphan)", Confidence::Questionable},
        {LR"(PF\Seen Once)", Confidence::Questionable},
        {LR"(Users\me\Local\Programs\Old Tool)", Confidence::Questionable},
        {LR"(Users\other\Local\Programs\Their Tool)", Confidence::Bad},
    };
    EXPECT_EQ(found(find()), expected);
}

}  // namespace
}  // namespace avel::leftovers
