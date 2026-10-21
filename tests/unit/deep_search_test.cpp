// The deep search: folder paths rebuilt from master file table records, and the scanner that
// matches them, against a made-up volume Q: (no disk is read). Reading a real volume needs
// administrator rights; that test skips without them.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/leftovers/scan.h"
#include "core/platform/elevation.h"
#include "core/platform/file_ops.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/platform/volume_folders.h"
#include "scratch_registry.h"

namespace avel::leftovers {
namespace {

namespace fs = std::filesystem;
using platform::VolumeFolder;
using platform::VolumeFolders;

constexpr std::uint64_t kRoot = 0x0005000000000005;  // NTFS's root folder: record 5, sequence 5

// The folders of Q:, by file reference number.
VolumeFolders volume_q() {
    std::vector<VolumeFolder> folders = {
        {kRoot, kRoot, L".", FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM},
        {10, kRoot, L"Tools", FILE_ATTRIBUTE_DIRECTORY},
        {11, 10, L"Contoso Fixture", FILE_ATTRIBUTE_DIRECTORY},  // Questionable: the name alone
        {20, kRoot, L"Contoso", FILE_ATTRIBUTE_DIRECTORY},
        {21, 20, L"Contoso Fixture", FILE_ATTRIBUTE_DIRECTORY},  // Good: in the publisher's folder
        {30, kRoot, L"Data", FILE_ATTRIBUTE_DIRECTORY},
        {31, 30, L"Contoso Fixture Projects", FILE_ATTRIBUTE_DIRECTORY},  // Only resembles the name
        {32, 30, L"Contoso Fixture", FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT},  // A junction
        {40, kRoot, L"$Recycle.Bin",
         FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM},
        {41, 40, L"S-1-5-21-1-2-3-1001", FILE_ATTRIBUTE_DIRECTORY},
        {42, 41, L"Contoso Fixture", FILE_ATTRIBUTE_DIRECTORY},
        {50, kRoot, L"Windows.old", FILE_ATTRIBUTE_DIRECTORY},
        {51, 50, L"Program Files", FILE_ATTRIBUTE_DIRECTORY},
        {52, 51, L"Contoso Fixture", FILE_ATTRIBUTE_DIRECTORY},
        {60, kRoot, L"Games", FILE_ATTRIBUTE_DIRECTORY},
        {61, 60, L"Other Suite", FILE_ATTRIBUTE_DIRECTORY},  // Another program's folder
        {62, 61, L"Contoso Fixture", FILE_ATTRIBUTE_DIRECTORY},
        {70, kRoot, L"ProgramData", FILE_ATTRIBUTE_DIRECTORY},  // A root of the drive scan
        {71, 70, L"Contoso Fixture", FILE_ATTRIBUTE_DIRECTORY},
        {80, kRoot, L"Apps", FILE_ATTRIBUTE_DIRECTORY},
        {81, 80, L"Contoso Fixture", FILE_ATTRIBUTE_DIRECTORY},  // The program's own folder
        {90, kRoot, L"Users", FILE_ATTRIBUTE_DIRECTORY},
        {91, 90, L"other", FILE_ATTRIBUTE_DIRECTORY},
        {92, 91, L"Contoso Fixture", FILE_ATTRIBUTE_DIRECTORY},                   // In another user's profile
        {100, 0x0001000000000099, L"Contoso Fixture", FILE_ATTRIBUTE_DIRECTORY},  // Its parent is gone
    };
    return VolumeFolders(L"Q:\\", kRoot, std::move(folders));
}

std::size_t index_of(const VolumeFolders& volume, std::uint64_t id) {
    const auto& folders = volume.folders();
    return static_cast<std::size_t>(
        std::find_if(folders.begin(), folders.end(), [&](const auto& folder) { return folder.id == id; }) -
        folders.begin());
}

TEST(VolumeFoldersTest, RebuildsPathsFromRecords) {
    const auto volume = volume_q();
    EXPECT_EQ(volume.path_of(index_of(volume, 10)), LR"(Q:\Tools)");
    EXPECT_EQ(volume.path_of(index_of(volume, 11)), LR"(Q:\Tools\Contoso Fixture)");
    EXPECT_EQ(volume.path_of(index_of(volume, 42)),
              LR"(Q:\$Recycle.Bin\S-1-5-21-1-2-3-1001\Contoso Fixture)");
    EXPECT_EQ(volume.parent_name(index_of(volume, 11)), L"Tools");
    EXPECT_EQ(volume.parent_name(index_of(volume, 10)), L"");
    // The root itself and a folder cut off from it have no path.
    EXPECT_EQ(volume.path_of(index_of(volume, kRoot)), L"");
    EXPECT_EQ(volume.path_of(index_of(volume, 100)), L"");
}

TEST(VolumeFoldersTest, StopsAtALoop) {
    const VolumeFolders volume(
        L"Q:\\", kRoot, {{10, 11, L"A", FILE_ATTRIBUTE_DIRECTORY}, {11, 10, L"B", FILE_ATTRIBUTE_DIRECTORY}});
    EXPECT_EQ(volume.path_of(0), L"");
}

TEST(VolumeFoldersTest, ReadsTheVolumeOfATemporaryFolder) {
    if (!platform::is_process_elevated()) {
        GTEST_SKIP() << "reading a volume's master file table needs administrator rights";
    }
    const std::wstring name = L"avel-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-mft";
    const fs::path folder = fs::temp_directory_path() / name / L"Inner";
    fs::create_directories(folder);
    const auto root = platform::volume_root(folder.wstring());
    ASSERT_TRUE(root);
    VolumeFolders volume;
    ASSERT_EQ(VolumeFolders::read(*root, volume), static_cast<std::uint32_t>(ERROR_SUCCESS));
    std::wstring found;
    for (std::size_t i = 0; i < volume.folders().size(); ++i) {
        if (volume.folders()[i].name == L"Inner" && volume.parent_name(i) == name) {
            found = volume.path_of(i);
        }
    }
    // The temporary folder may be spelled with short names; the rebuilt path is the long one.
    EXPECT_TRUE(found.ends_with(L"\\" + name + L"\\Inner")) << platform::to_utf8(found);
    EXPECT_NE(::GetFileAttributesW(found.c_str()), INVALID_FILE_ATTRIBUTES);
    std::error_code ignored;
    fs::remove_all(fs::temp_directory_path() / name, ignored);
}

class DeepScanTest : public ::testing::Test {
protected:
    void SetUp() override { com_ = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    void TearDown() override {
        if (SUCCEEDED(com_)) {
            ::CoUninitialize();
        }
    }

    ScanResult scan(bool deep) {
        ScanEnvironment environment;
        environment.machine = machine_.get();
        wchar_t windows[MAX_PATH] = {};
        ::GetWindowsDirectoryW(windows, MAX_PATH);
        environment.machine_folders.windows = windows;
        environment.machine_folders.program_data = LR"(Q:\ProgramData)";
        for (const auto* sid : {L"S-1-5-21-1-2-3-1001", L"S-1-5-21-1-2-3-1002"}) {
            UserScope user;
            const bool current = sid == std::wstring_view(L"S-1-5-21-1-2-3-1001");
            user.profile = {sid, current ? L"PC\\fixture" : L"PC\\other",
                            current ? LR"(Q:\Users\fixture)" : LR"(Q:\Users\other)", false};
            environment.users.push_back(std::move(user));
        }
        environment.current_user_sid = L"S-1-5-21-1-2-3-1001";
        environment.tasks = std::vector<ScheduledTask>{};
        environment.volumes = std::vector<VolumeFolders>{volume_q()};

        ScanTarget target;
        target.program_id = L"reg:HKLM64:ContosoFixture";
        target.display_name = L"Contoso Fixture";
        target.names = make_product_names(target.display_name, L"Contoso");
        target.install_location = LR"(Q:\Apps\Contoso Fixture)";
        inventory::Program other;
        other.id = L"reg:HKLM64:OtherSuite";
        other.display_name = L"Other Suite";
        other.install_location = LR"(Q:\Games\Other Suite)";
        return scan_leftovers(environment, std::vector<ScanTarget>{target},
                              std::vector<inventory::Program>{other}, {.deep = deep});
    }

    static std::vector<std::pair<std::wstring, Confidence>> found(const ScanResult& result) {
        std::vector<std::pair<std::wstring, Confidence>> items;
        for (const auto& item : result.leftovers) {
            if (item.found_by == Scanner::DeepSearch) {
                items.emplace_back(std::get<FileLocation>(item.location).path, level(item.records));
            }
        }
        std::sort(items.begin(), items.end());
        return items;
    }

    HRESULT com_ = E_FAIL;
    testing::ScratchKey machine_;
};

TEST_F(DeepScanTest, FindsFoldersNamedAfterTheProgramOutsideCommonLocations) {
    const auto result = scan(true);
    const std::vector<std::pair<std::wstring, Confidence>> expected = {
        {LR"(Q:\Contoso\Contoso Fixture)", Confidence::Good},
        {LR"(Q:\Tools\Contoso Fixture)", Confidence::Questionable},
        {LR"(Q:\Users\other\Contoso Fixture)", Confidence::Bad},
    };
    EXPECT_EQ(found(result), expected);
    const auto timing = std::find_if(result.timings.begin(), result.timings.end(),
                                     [](const auto& timing) { return timing.stage == "deep"; });
    EXPECT_NE(timing, result.timings.end());
}

TEST_F(DeepScanTest, OnlyWhenAsked) { EXPECT_TRUE(found(scan(false)).empty()); }

}  // namespace
}  // namespace avel::leftovers
