#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <string>

#include "core/platform/known_folders.h"
#include "core/platform/text.h"
#include "scratch_registry.h"

namespace avel::platform {
namespace {

bool contains(const std::vector<std::wstring>& list, std::wstring_view path) {
    return std::any_of(list.begin(), list.end(), [&](const auto& item) { return iequals(item, path); });
}

constexpr wchar_t kShellFolders[] = LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders)";

TEST(KnownFoldersTest, UserFoldersComeFromTheUsersHiveIncludingRedirection) {
    testing::ScratchKey hive;
    hive.set_string(kShellFolders, L"AppData", LR"(%USERPROFILE%\AppData\Roaming)", REG_EXPAND_SZ);
    hive.set_string(kShellFolders, L"Desktop", LR"(D:\Redirected\Desktop\)", REG_EXPAND_SZ);
    hive.set_string(kShellFolders, L"{374DE290-123F-4565-9164-39C4925E467B}", LR"(%USERPROFILE%\Downloads)",
                    REG_EXPAND_SZ);
    const UserProfile profile{L"S-1-5-21-1-2-3-1001", L"PC\\fixture", LR"(C:\Users\fixture)", false};

    const UserFolders folders = user_folders(hive.get(), profile);
    EXPECT_EQ(folders.profile, LR"(C:\Users\fixture)");
    EXPECT_EQ(folders.roaming_app_data, LR"(C:\Users\fixture\AppData\Roaming)");
    EXPECT_EQ(folders.desktop, LR"(D:\Redirected\Desktop)");
    // Not named in the hive: the Windows defaults.
    EXPECT_EQ(folders.local_app_data, LR"(C:\Users\fixture\AppData\Local)");
    EXPECT_EQ(folders.local_low_app_data, LR"(C:\Users\fixture\AppData\LocalLow)");
    EXPECT_EQ(folders.programs, LR"(C:\Users\fixture\AppData\Roaming\Microsoft\Windows\Start Menu\Programs)");
    EXPECT_TRUE(contains(folders.all, LR"(C:\Users\fixture)"));
    EXPECT_TRUE(contains(folders.all, LR"(C:\Users\fixture\AppData)"));
    EXPECT_TRUE(contains(folders.all, LR"(C:\Users\fixture\Downloads)"));
}

TEST(KnownFoldersTest, UserFoldersWithoutAHiveUseDefaults) {
    const UserProfile profile{L"S-1-5-21-1-2-3-1002", L"PC\\other", LR"(C:\Users\other)", false};
    const UserFolders folders = user_folders(nullptr, profile);
    EXPECT_EQ(folders.roaming_app_data, LR"(C:\Users\other\AppData\Roaming)");
    EXPECT_EQ(folders.desktop, LR"(C:\Users\other\Desktop)");
}

TEST(KnownFoldersTest, MachineFoldersOnThisSystem) {
    const MachineFolders folders = machine_folders();
    wchar_t windows[MAX_PATH] = {};
    ASSERT_GT(::GetWindowsDirectoryW(windows, MAX_PATH), 0u);
    EXPECT_TRUE(iequals(folders.windows, windows));
    EXPECT_TRUE(iends_with(folders.program_files, L"\\Program Files"));
    EXPECT_TRUE(iends_with(folders.program_files_x86, L"\\Program Files (x86)"));
    EXPECT_FALSE(folders.program_data.empty());
    EXPECT_FALSE(folders.users.empty());
    EXPECT_TRUE(contains(folders.all, folders.windows));
    EXPECT_TRUE(contains(folders.all, folders.users));
    EXPECT_GT(folders.all.size(), 20u);  // Fonts, System32, Public folders, common Start Menu, ...
}

}  // namespace
}  // namespace avel::platform
