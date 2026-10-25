// The tracker's file half on a real volume: start on the volume of a temporary folder, change
// files and folders in it, read the journal and resolve the net changes. Opening a volume needs
// administrator rights, so these tests skip without them; they run in Windows Sandbox with
// --gtest_filter=UsnLiveTest.*. Other activity on the volume is expected, so only changes inside
// the temporary folder are compared.
#include <gtest/gtest.h>

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/platform/elevation.h"
#include "core/platform/file_ops.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/platform/usn_journal.h"
#include "core/tracker/journal_trace.h"
#include "file_tree.h"

namespace avel::tracker {
namespace {

using platform::to_utf8;

std::int64_t now_as_filetime() {
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(now.dwHighDateTime) << 32) |
                                     now.dwLowDateTime);
}

std::uint64_t id_of(const std::wstring& path) {
    const auto item = platform::open_item(path, FILE_READ_ATTRIBUTES);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!item || !::GetFileInformationByHandle(item.get(), &info)) {
        return 0;
    }
    return (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}

// "created new\sub\ +1": changes inside `folder`, relative to it, sorted.
std::vector<std::string> describe_inside(const std::vector<TraceChange>& changes,
                                         const std::wstring& folder) {
    std::vector<std::string> lines;
    for (const auto& change : changes) {
        const auto& location = std::get<leftovers::FileLocation>(change.location);
        if (!platform::path_is_inside(location.path, folder)) {
            continue;
        }
        std::string line = std::string(to_string(change.change)) + " " +
                           to_utf8(std::wstring_view(location.path).substr(folder.size() + 1));
        if (location.directory) {
            line += "\\";
        }
        if (!change.old_path.empty()) {
            line += " <- " + (platform::path_is_inside(change.old_path, folder)
                                  ? to_utf8(std::wstring_view(change.old_path).substr(folder.size() + 1))
                                  : to_utf8(change.old_path));
        }
        if (change.contents != 0) {
            line += " +" + std::to_string(change.contents);
        }
        if (change.replaced) {
            line += " replaced";
        }
        if (change.noise != NoiseReason::None) {
            line += " (" + std::string(to_string(change.noise)) + ")";
        }
        lines.push_back(std::move(line));
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

class UsnLiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!platform::is_process_elevated()) {
            GTEST_SKIP() << "reading a volume's USN journal needs administrator rights";
        }
        // The journal reports long names; the temporary folder may be spelled with short ones.
        folder_ = platform::long_path(temp_.path());
        const auto root = platform::volume_root(folder_);
        ASSERT_TRUE(root);
        root_ = *root;
    }

    testing::TempFolder temp_;
    std::wstring folder_;
    std::wstring root_;
};

TEST_F(UsnLiveTest, ReadsAndResolvesTheNetChangesInAFolder) {
    // What exists before the trace starts.
    testing::write_file(folder_ + L"\\kept.txt", "kept");
    testing::write_file(folder_ + L"\\old name.txt", "renamed");
    testing::write_file(folder_ + L"\\doomed.txt", "deleted");
    testing::write_file(folder_ + L"\\replaced.dll", "version 1");
    testing::write_file(folder_ + L"\\shared.dll", "linked");
    testing::write_file(folder_ + L"\\Old Folder\\inside.txt", "moves with its folder");
    testing::write_file(folder_ + L"\\untouched.txt", "untouched");
    // A virus scanner may keep a new file open for a moment; its close record would then fall
    // inside the trace.
    ::Sleep(500);

    const std::int64_t trace_start = now_as_filetime();
    auto volume = start_volume(root_);
    ASSERT_EQ(volume.status, JournalStatus::Ok) << to_string(volume.status) << " " << volume.error;
    EXPECT_FALSE(volume.guid_path.empty());
    EXPECT_NE(volume.root_frn, 0u);

    // The installer's work.
    testing::write_file(folder_ + L"\\New\\Sub\\file.txt", "created");
    testing::write_file(folder_ + L"\\New\\top.txt", "created");
    testing::write_file(folder_ + L"\\kept.txt", "kept, written again");
    ASSERT_TRUE(::MoveFileW((folder_ + L"\\old name.txt").c_str(), (folder_ + L"\\new name.txt").c_str()));
    ASSERT_TRUE(::DeleteFileW((folder_ + L"\\doomed.txt").c_str()));
    testing::write_file(folder_ + L"\\replaced.dll.tmp", "version 2");
    ASSERT_TRUE(::MoveFileExW((folder_ + L"\\replaced.dll.tmp").c_str(),
                              (folder_ + L"\\replaced.dll").c_str(), MOVEFILE_REPLACE_EXISTING));
    ASSERT_TRUE(::CreateHardLinkW((folder_ + L"\\shared-link.dll").c_str(),
                                  (folder_ + L"\\shared.dll").c_str(), nullptr));
    testing::write_file(folder_ + L"\\transient.tmp", "gone again");
    ASSERT_TRUE(::DeleteFileW((folder_ + L"\\transient.tmp").c_str()));
    testing::make_folder(folder_ + L"\\Empty Then Gone");
    ASSERT_TRUE(::RemoveDirectoryW((folder_ + L"\\Empty Then Gone").c_str()));
    ASSERT_TRUE(::MoveFileW((folder_ + L"\\Old Folder").c_str(), (folder_ + L"\\New Folder").c_str()));

    // Drained twice, the way the session drains while an installer runs.
    FileChangeAggregator changes;
    drain_volume(volume, changes);
    ASSERT_EQ(volume.status, JournalStatus::Ok) << to_string(volume.status) << " " << volume.error;
    testing::write_file(folder_ + L"\\New\\late.txt", "after the first drain");
    drain_volume(volume, changes);
    ASSERT_EQ(volume.status, JournalStatus::Ok) << to_string(volume.status) << " " << volume.error;
    EXPECT_GT(volume.next_usn, volume.start_usn);

    const std::vector<std::string> expected = {
        "created New\\ +4",
        "created shared-link.dll",
        "deleted doomed.txt",
        "modified kept.txt",
        "modified replaced.dll replaced",
        "renamed New Folder\\ <- Old Folder",
        "renamed new name.txt <- old name.txt",
    };
    EXPECT_EQ(describe_inside(resolve_volume(volume, changes, trace_start), folder_), expected);

    // The same after a checkpoint.
    VolumeTrace restored;
    FileChangeAggregator restored_changes;
    ASSERT_TRUE(load_checkpoint(save_checkpoint(volume, changes), restored, restored_changes));
    EXPECT_EQ(describe_inside(resolve_volume(restored, restored_changes, trace_start), folder_), expected);
}

TEST_F(UsnLiveTest, ReadsOnlyUpToTheGivenPosition) {
    std::uint32_t error = 0;
    const auto volume = platform::UsnVolume::open(root_, error);
    ASSERT_TRUE(volume) << error;
    platform::JournalState journal;
    ASSERT_EQ(volume->query(journal), static_cast<std::uint32_t>(ERROR_SUCCESS));
    const std::int64_t start = journal.next_usn;
    testing::write_file(folder_ + L"\\before.txt", "before");
    ASSERT_EQ(volume->query(journal), static_cast<std::uint32_t>(ERROR_SUCCESS));
    const std::int64_t until = journal.next_usn;
    testing::write_file(folder_ + L"\\after.txt", "after");

    const std::uint64_t folder_id = id_of(folder_);
    std::vector<std::wstring> names;
    std::int64_t reached = 0;
    ASSERT_EQ(volume->read(
                  journal.id, start, until,
                  [&](const platform::UsnRecord& record) {
                      EXPECT_GE(record.usn, start);
                      EXPECT_LT(record.usn, until);
                      if (record.parent_frn == folder_id) {
                          names.emplace_back(record.name);
                      }
                  },
                  reached),
              static_cast<std::uint32_t>(ERROR_SUCCESS));
    EXPECT_GE(reached, until);
    EXPECT_NE(std::find(names.begin(), names.end(), L"before.txt"), names.end());
    EXPECT_EQ(std::find(names.begin(), names.end(), L"after.txt"), names.end());
}

TEST_F(UsnLiveTest, NamesFoldersByTheirIds) {
    testing::make_folder(folder_ + L"\\Inner Folder");
    std::uint32_t error = 0;
    const auto volume = platform::UsnVolume::open(root_, error);
    ASSERT_TRUE(volume) << error;
    const std::uint64_t parent = id_of(folder_);
    const std::uint64_t inner = id_of(folder_ + L"\\Inner Folder");
    ASSERT_NE(inner, 0u);

    const auto name = volume->name_of(inner);
    ASSERT_TRUE(name);
    EXPECT_EQ(name->first, parent);
    EXPECT_EQ(name->second, L"Inner Folder");
    const auto path = volume->path_of(inner);
    ASSERT_TRUE(path);
    EXPECT_TRUE(platform::paths_equal(*path, folder_ + L"\\Inner Folder")) << to_utf8(*path);
    EXPECT_EQ(volume->path_of(volume->root_frn()),
              std::optional<std::wstring>(platform::normalize_path(root_)));
    EXPECT_FALSE(volume->name_of(0x0000FFFFFFFFFFF0));  // No such record
}

}  // namespace
}  // namespace avel::tracker
