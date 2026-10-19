// Quarantine round trips in a temporary folder: dump a tree, move it to quarantine, move it back,
// dump again; the dumps must match, file ids included (a rename keeps them).
#include <gtest/gtest.h>

#include <windows.h>

#include <wil/resource.h>

#include <string>

#include "core/changes/file_quarantine.h"
#include "core/changes/locks.h"
#include "file_tree.h"

namespace avel::changes {
namespace {

using testing::dump_tree;
using testing::write_file;

class FileQuarantineTest : public ::testing::Test {
protected:
    void SetUp() override {
        app_ = temp_ / L"Program Files\\Contoso";
        quarantine_ = temp_ / L"Quarantine\\0001";
        write_file(app_ + L"\\app.exe", "MZ fixture");
        write_file(app_ + L"\\readme.txt", "read me");
        ::SetFileAttributesW((app_ + L"\\readme.txt").c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN);
        write_file(app_ + L"\\data\\settings.ini", "[x]\r\ny=1\r\n");
        write_file(app_ + L"\\data\\settings.ini:Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
        ASSERT_TRUE(::CreateHardLinkW((app_ + L"\\data\\settings-link.ini").c_str(),
                                      (app_ + L"\\data\\settings.ini").c_str(), nullptr));
        // A folder with its own protected DACL, made before its contents so they inherit from it.
        testing::make_folder(app_ + L"\\secret");
        testing::set_sddl(app_ + L"\\secret",
                          L"D:P(A;OICI;FA;;;" + testing::current_user_sid_text() + L")(A;OICI;FA;;;SY)");
        write_file(app_ + L"\\secret\\key.bin", std::string("\x01\x02\x03", 3));
        // A path longer than MAX_PATH.
        std::wstring deep = app_ + L"\\deep";
        for (int i = 0; i < 6; ++i) {
            deep += L"\\" + std::wstring(48, static_cast<wchar_t>(L'a' + i));
        }
        write_file(deep + L"\\far.txt", "far away");
        // A junction pointing out of the tree: moved as a link, its target never touched.
        write_file(temp_ / L"Outside\\keep.txt", "not part of the program");
        ASSERT_TRUE(testing::make_junction(app_ + L"\\link-out", temp_ / L"Outside"));
        // Folder times are set last; writing into a folder changes them.
        FILETIME old{0x12345678, 0x01d00000};
        wil::unique_hfile folder(::CreateFileW((app_ + L"\\data").c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        ::SetFileTime(folder.get(), &old, nullptr, &old);
    }

    std::optional<FileMove> prepare(const std::wstring& path, bool directory = true) {
        ItemStatus failure;
        auto move = prepare_move(path, directory, quarantine_, failure);
        // Through JSON, as the ChangeSet stores it.
        if (move) {
            FileMove loaded;
            EXPECT_TRUE(from_json(Json::parse(to_json(*move).dump()), loaded));
            return loaded;
        }
        failure_ = failure;
        return std::nullopt;
    }

    testing::TempFolder temp_;
    std::wstring app_;
    std::wstring quarantine_;
    ItemStatus failure_;
};

TEST_F(FileQuarantineTest, AWholeFolderMovesAndComesBackExactly) {
    const std::string before = dump_tree(app_);
    const std::string outside = dump_tree(temp_ / L"Outside");
    const auto move = prepare(app_);
    ASSERT_TRUE(move);
    EXPECT_EQ(move->bytes, 10u + 7 + 10 + 10 + 3 + 8);  // A hard link's data counts once per name

    const auto moved = move_to_quarantine(*move);
    EXPECT_EQ(moved.status.state, ItemState::Done);
    EXPECT_EQ(dump_tree(app_), "<missing>");
    EXPECT_EQ(dump_tree(temp_ / L"Outside"), outside);

    const auto restored = restore_from_quarantine(*move, false);
    EXPECT_EQ(restored.status.state, ItemState::Restored);
    EXPECT_EQ(dump_tree(app_), before);
    EXPECT_EQ(dump_tree(quarantine_), "<missing>");
}

TEST_F(FileQuarantineTest, AFolderWithAFileInUseMovesFileByFileAndComesBack) {
    const testing::DumpOptions without_folder_ids{false};
    const std::string before = dump_tree(app_, without_folder_ids);
    const auto move = prepare(app_);
    ASSERT_TRUE(move);
    {
        // Held open without delete sharing, like a running program's file.
        wil::unique_hfile busy(::CreateFileW((app_ + L"\\data\\settings.ini").c_str(), GENERIC_READ, FILE_SHARE_READ,
                                             nullptr, OPEN_EXISTING, 0, nullptr));
        ASSERT_TRUE(busy);
        const auto moved = move_to_quarantine(*move);
        EXPECT_EQ(moved.status.state, ItemState::Partial);
        EXPECT_EQ(moved.status.reason, Reason::InUse);
        // NTFS lets the other name of a hard-linked file move while the file is open.
        ASSERT_EQ(moved.left_behind.size(), 1u);
        EXPECT_EQ(moved.left_behind.front(), app_ + L"\\data\\settings.ini");
        EXPECT_EQ(::GetFileAttributesW((app_ + L"\\app.exe").c_str()), INVALID_FILE_ATTRIBUTES);
        EXPECT_EQ(::GetFileAttributesW((app_ + L"\\secret").c_str()), INVALID_FILE_ATTRIBUTES);
        EXPECT_NE(::GetFileAttributesW((app_ + L"\\data\\settings.ini").c_str()), INVALID_FILE_ATTRIBUTES);
    }
    const auto restored = restore_from_quarantine(*move, true);
    EXPECT_EQ(restored.status.state, ItemState::Restored);
    EXPECT_EQ(dump_tree(app_, without_folder_ids), before);
}

TEST_F(FileQuarantineTest, RestartManagerNamesWhoHoldsAFile) {
    const std::wstring file = app_ + L"\\app.exe";
    EXPECT_TRUE(processes_using({file}).empty());
    wil::unique_hfile busy(::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    ASSERT_TRUE(busy);
    const auto holders = processes_using({file});
    ASSERT_EQ(holders.size(), 1u);
    EXPECT_EQ(holders.front().pid, ::GetCurrentProcessId());
}

TEST_F(FileQuarantineTest, NothingMovesWhenEveryFileIsInUse) {
    write_file(temp_ / L"Busy\\only.dll", "x");
    const std::string before = dump_tree(temp_ / L"Busy");
    ItemStatus failure;
    const auto move = prepare_move(temp_ / L"Busy", true, temp_ / L"Quarantine\\0002", failure);
    ASSERT_TRUE(move);
    wil::unique_hfile busy(::CreateFileW((temp_ / L"Busy\\only.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                         OPEN_EXISTING, 0, nullptr));
    const auto moved = move_to_quarantine(*move);
    EXPECT_EQ(moved.status.state, ItemState::Failed);
    EXPECT_EQ(dump_tree(temp_ / L"Busy"), before);
    EXPECT_EQ(dump_tree(temp_ / L"Quarantine\\0002"), "<missing>");
}

TEST_F(FileQuarantineTest, NeverRestoresOverSomethingNew) {
    const auto move = prepare(app_);
    ASSERT_TRUE(move);
    ASSERT_EQ(move_to_quarantine(*move).status.state, ItemState::Done);
    write_file(app_ + L"\\reinstalled.exe", "new");
    const std::string reinstalled = dump_tree(app_);
    const std::string quarantined = dump_tree(quarantine_);
    EXPECT_EQ(restore_from_quarantine(*move, false).status.state, ItemState::RestoreSkipped);
    EXPECT_EQ(dump_tree(app_), reinstalled);
    EXPECT_EQ(dump_tree(quarantine_), quarantined);
}

TEST_F(FileQuarantineTest, SingleFilesMoveToo) {
    // The file itself; its folder's time changes when a file leaves it, as with any removal.
    const std::wstring readme = app_ + L"\\readme.txt";
    const std::string before = dump_tree(readme);
    const auto move = prepare(readme, false);
    ASSERT_TRUE(move);
    EXPECT_FALSE(move->directory);
    EXPECT_EQ(move_to_quarantine(*move).status.state, ItemState::Done);
    EXPECT_EQ(::GetFileAttributesW(readme.c_str()), INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(restore_from_quarantine(*move, false).status.state, ItemState::Restored);
    EXPECT_EQ(dump_tree(readme), before);
    EXPECT_EQ(dump_tree(quarantine_), "<missing>");
}

TEST_F(FileQuarantineTest, RefusesPathsThroughLinksAndMissingItems) {
    ASSERT_TRUE(testing::make_junction(temp_ / L"Shortcut", app_));
    EXPECT_FALSE(prepare(temp_ / L"Shortcut\\data"));
    EXPECT_EQ(failure_.reason, Reason::PathChanged);
    EXPECT_FALSE(prepare(temp_ / L"Nowhere"));
    EXPECT_EQ(failure_.reason, Reason::Missing);
    // A file where a folder was expected.
    EXPECT_FALSE(prepare(app_ + L"\\app.exe", true));
    EXPECT_EQ(failure_.reason, Reason::PathChanged);
}

}  // namespace
}  // namespace avel::changes
