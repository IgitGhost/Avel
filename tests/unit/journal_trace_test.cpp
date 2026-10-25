// The decisions behind reading a traced volume's journal: whether the journal can still be read
// from the trace's position, where the volume is now, which records are NTFS's own, and the
// checkpoint that carries a volume's state across a restart. No volume is opened.
#include <gtest/gtest.h>

#include <windows.h>
#include <winioctl.h>

#include <string>
#include <vector>

#include "core/tracker/journal_trace.h"

namespace avel::tracker {
namespace {

constexpr std::uint64_t kRoot = 0x0005000000000005;
constexpr wchar_t kVolumeD[] = LR"(\\?\Volume{6f1c2a3b-0000-0000-0000-100000000000}\)";
constexpr wchar_t kVolumeE[] = LR"(\\?\Volume{6f1c2a3b-0000-0000-0000-200000000000}\)";

VolumeTrace traced_d() {
    VolumeTrace volume;
    volume.root = LR"(D:\)";
    volume.guid_path = kVolumeD;
    volume.serial = 0x1234ABCD;
    volume.root_frn = kRoot;
    volume.journal_id = 0x01DB000000000001;
    volume.start_usn = 0x100000;
    volume.next_usn = 0x180000;
    return volume;
}

platform::JournalState journal_now() {
    return {0x01DB000000000001, 0x080000, 0x200000, 0x000000, 64ull << 20};
}

TEST(JournalTraceTest, ReadsOnWhileTheJournalIsUnchanged) {
    EXPECT_EQ(check_journal(traced_d(), 0x1234ABCD, ERROR_SUCCESS, journal_now()), JournalStatus::Ok);
    // Nothing new since the last read.
    auto journal = journal_now();
    journal.next_usn = traced_d().next_usn;
    EXPECT_EQ(check_journal(traced_d(), 0x1234ABCD, ERROR_SUCCESS, journal), JournalStatus::Ok);
}

TEST(JournalTraceTest, AJournalDeletedOrCreatedAgainIsReplaced) {
    auto journal = journal_now();
    journal.id = 0x01DB000000000002;
    EXPECT_EQ(check_journal(traced_d(), 0x1234ABCD, ERROR_SUCCESS, journal), JournalStatus::Replaced);
    EXPECT_EQ(check_journal(traced_d(), 0x1234ABCD, ERROR_JOURNAL_NOT_ACTIVE, {}), JournalStatus::Replaced);
    EXPECT_EQ(check_journal(traced_d(), 0x1234ABCD, ERROR_JOURNAL_DELETE_IN_PROGRESS, {}),
              JournalStatus::Replaced);
    // Same id, but it ends before where the trace got to.
    journal = journal_now();
    journal.next_usn = 0x100;
    EXPECT_EQ(check_journal(traced_d(), 0x1234ABCD, ERROR_SUCCESS, journal), JournalStatus::Replaced);
}

TEST(JournalTraceTest, APositionOverwrittenSinceIsWrapped) {
    auto journal = journal_now();
    journal.first_usn = 0x190000;
    EXPECT_EQ(check_journal(traced_d(), 0x1234ABCD, ERROR_SUCCESS, journal), JournalStatus::Wrapped);
    journal = journal_now();
    journal.lowest_valid_usn = 0x190000;
    EXPECT_EQ(check_journal(traced_d(), 0x1234ABCD, ERROR_SUCCESS, journal), JournalStatus::Wrapped);
}

TEST(JournalTraceTest, AFormattedVolumeHasAnotherSerial) {
    EXPECT_EQ(check_journal(traced_d(), 0x5555AAAA, ERROR_SUCCESS, journal_now()),
              JournalStatus::SerialChanged);
}

TEST(JournalTraceTest, OtherQueryFailuresAreUnreadable) {
    EXPECT_EQ(check_journal(traced_d(), 0x1234ABCD, ERROR_ACCESS_DENIED, {}), JournalStatus::Unreadable);
}

TEST(JournalTraceTest, ReadFailures) {
    EXPECT_EQ(read_failure(ERROR_JOURNAL_ENTRY_DELETED), JournalStatus::Wrapped);
    EXPECT_EQ(read_failure(ERROR_JOURNAL_NOT_ACTIVE), JournalStatus::Replaced);
    EXPECT_EQ(read_failure(ERROR_JOURNAL_DELETE_IN_PROGRESS), JournalStatus::Replaced);
    EXPECT_EQ(read_failure(ERROR_INVALID_PARAMETER), JournalStatus::Replaced);
    EXPECT_EQ(read_failure(ERROR_NOT_READY), JournalStatus::Unreadable);
}

TEST(JournalTraceTest, FindsTheVolumeByItsGuidAfterItsLetterChanged) {
    const std::vector<MountedVolume> mounted = {
        {LR"(C:\)", LR"(\\?\Volume{00000000-0000-0000-0000-000000000001}\)"},
        {LR"(D:\)", kVolumeE},  // Another disk took D:
        {LR"(F:\)", kVolumeD},
    };
    EXPECT_EQ(find_volume_root(traced_d(), mounted), std::optional<std::wstring>(LR"(F:\)"));
}

TEST(JournalTraceTest, PrefersTheLetterItHad) {
    const std::vector<MountedVolume> mounted = {{LR"(G:\)", kVolumeD}, {LR"(D:\)", kVolumeD}};
    EXPECT_EQ(find_volume_root(traced_d(), mounted), std::optional<std::wstring>(LR"(D:\)"));
}

TEST(JournalTraceTest, AVolumeThatIsGoneIsNotFound) {
    const std::vector<MountedVolume> mounted = {{LR"(D:\)", kVolumeE}};
    EXPECT_EQ(find_volume_root(traced_d(), mounted), std::nullopt);
    EXPECT_EQ(find_volume_root(traced_d(), {}), std::nullopt);
}

TEST(JournalTraceTest, VolumesThatAreNoLongerOkAreNotRead) {
    auto volume = traced_d();
    volume.status = JournalStatus::Wrapped;
    FileChangeAggregator changes;
    drain_volume(volume, changes);
    EXPECT_EQ(volume.status, JournalStatus::Wrapped);
    EXPECT_EQ(volume.next_usn, traced_d().next_usn);
    EXPECT_TRUE(changes.empty());
}

platform::UsnRecord record(std::uint64_t frn, std::uint64_t parent, std::wstring_view name,
                           std::uint32_t source_info = 0) {
    platform::UsnRecord record;
    record.frn = frn;
    record.parent_frn = parent;
    record.name = name;
    record.reason = USN_REASON_DATA_EXTEND;
    record.source_info = source_info;
    return record;
}

TEST(JournalTraceTest, NtfsMetadataAndAuxiliaryRecordsAreNoise) {
    EXPECT_TRUE(is_journal_noise(record(0x0001000000000000, kRoot, L"$MFT"), kRoot));
    EXPECT_TRUE(is_journal_noise(record(0x000B00000000000B, kRoot, L"$Extend"), kRoot));
    // $Extend\$UsnJrnl and its siblings live in MFT records past 24, inside $Extend (record 11).
    EXPECT_TRUE(is_journal_noise(record(0x0001000000000040, 0x000B00000000000B, L"$UsnJrnl"), kRoot));
    EXPECT_TRUE(is_journal_noise(record(0x0001000000000041, kRoot, L"$Secure:$SDS"), kRoot));
    EXPECT_TRUE(
        is_journal_noise(record(0x0001000000001000, kRoot, L"setup.log", USN_SOURCE_AUXILIARY_DATA), kRoot));

    EXPECT_FALSE(is_journal_noise(record(0x0001000000001000, kRoot, L"Program Files"), kRoot));
    EXPECT_FALSE(is_journal_noise(record(0x0001000000001001, 0x0001000000001000, L"$Contoso"), kRoot));
    EXPECT_FALSE(
        is_journal_noise(record(0x0001000000001002, kRoot, L"setup.exe", USN_SOURCE_DATA_MANAGEMENT), kRoot));
}

TEST(JournalTraceTest, CheckpointsCarryTheVolumeAndItsChanges) {
    auto volume = traced_d();
    volume.created_journal = true;
    volume.status = JournalStatus::Wrapped;
    volume.error = ERROR_JOURNAL_ENTRY_DELETED;
    FileChangeAggregator changes;
    changes.add(record(0x0001000000001000, kRoot, L"Contoso"));

    const auto bytes = save_checkpoint(volume, changes);
    VolumeTrace loaded;
    FileChangeAggregator loaded_changes;
    ASSERT_TRUE(load_checkpoint(bytes, loaded, loaded_changes));
    EXPECT_EQ(loaded.root, volume.root);
    EXPECT_EQ(loaded.guid_path, volume.guid_path);
    EXPECT_EQ(loaded.serial, volume.serial);
    EXPECT_EQ(loaded.root_frn, volume.root_frn);
    EXPECT_EQ(loaded.journal_id, volume.journal_id);
    EXPECT_EQ(loaded.start_usn, volume.start_usn);
    EXPECT_EQ(loaded.next_usn, volume.next_usn);
    EXPECT_TRUE(loaded.created_journal);
    EXPECT_EQ(loaded.status, JournalStatus::Wrapped);
    EXPECT_EQ(loaded.error, static_cast<std::uint32_t>(ERROR_JOURNAL_ENTRY_DELETED));
    EXPECT_EQ(loaded_changes.entries(), changes.entries());

    auto damaged = bytes;
    damaged[damaged.size() / 2] ^= std::byte{1};
    VolumeTrace untouched = traced_d();
    EXPECT_FALSE(load_checkpoint(damaged, untouched, loaded_changes));
    EXPECT_EQ(untouched.next_usn, traced_d().next_usn);
    EXPECT_FALSE(load_checkpoint(std::span(bytes).first(bytes.size() - 3), untouched, loaded_changes));
}

TEST(JournalTraceTest, StatusNames) {
    EXPECT_EQ(to_string(JournalStatus::Ok), "ok");
    EXPECT_EQ(to_string(JournalStatus::Untracked), "untracked");
    EXPECT_EQ(to_string(JournalStatus::VolumeMissing), "volume-missing");
    EXPECT_EQ(to_string(JournalStatus::SerialChanged), "serial-changed");
    EXPECT_EQ(to_string(JournalStatus::Replaced), "replaced");
    EXPECT_EQ(to_string(JournalStatus::Wrapped), "wrapped");
    EXPECT_EQ(to_string(JournalStatus::Unreadable), "unreadable");
}

}  // namespace
}  // namespace avel::tracker
