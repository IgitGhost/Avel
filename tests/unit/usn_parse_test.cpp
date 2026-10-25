// Walking FSCTL_READ_USN_JOURNAL output: synthetic buffers of version 2 and 3 records, and the
// malformed ones the walk must survive.
#include <gtest/gtest.h>

#include <windows.h>
#include <winioctl.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "core/platform/usn_journal.h"

namespace avel::platform {
namespace {

struct Parsed {
    std::int64_t usn = 0;
    std::uint64_t frn = 0;
    std::uint64_t parent = 0;
    std::uint32_t reason = 0;
    std::wstring name;
    bool operator==(const Parsed&) const = default;
};

std::vector<std::byte> buffer_with_next(std::int64_t next) {
    std::vector<std::byte> buffer(sizeof(next));
    std::memcpy(buffer.data(), &next, sizeof(next));
    return buffer;
}

// Appends a record laid out the way NTFS writes it: the fixed part, the name, padding to 8 bytes.
template <typename Record>
void append(std::vector<std::byte>& buffer, Record header, std::wstring_view name) {
    constexpr std::size_t name_offset = offsetof(Record, FileName);
    const std::size_t length = (name_offset + name.size() * sizeof(wchar_t) + 7) & ~std::size_t{7};
    header.RecordLength = static_cast<DWORD>(length);
    header.FileNameOffset = static_cast<WORD>(name_offset);
    header.FileNameLength = static_cast<WORD>(name.size() * sizeof(wchar_t));
    std::vector<std::byte> record(length);
    std::memcpy(record.data(), &header, name_offset);
    std::memcpy(record.data() + name_offset, name.data(), name.size() * sizeof(wchar_t));
    buffer.insert(buffer.end(), record.begin(), record.end());
}

void append_v2(std::vector<std::byte>& buffer, std::int64_t usn, std::uint64_t frn, std::uint64_t parent,
               std::uint32_t reason, std::wstring_view name) {
    USN_RECORD_V2 header{};
    header.MajorVersion = 2;
    header.FileReferenceNumber = frn;
    header.ParentFileReferenceNumber = parent;
    header.Usn = usn;
    header.Reason = reason;
    header.FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    append(buffer, header, name);
}

void append_v3(std::vector<std::byte>& buffer, std::int64_t usn, std::uint64_t frn_low,
               std::uint64_t frn_high, std::uint64_t parent, std::uint32_t reason, std::wstring_view name) {
    USN_RECORD_V3 header{};
    header.MajorVersion = 3;
    const std::uint64_t id[2] = {frn_low, frn_high};
    const std::uint64_t parent_id[2] = {parent, 0};
    std::memcpy(header.FileReferenceNumber.Identifier, id, sizeof(id));
    std::memcpy(header.ParentFileReferenceNumber.Identifier, parent_id, sizeof(parent_id));
    header.Usn = usn;
    header.Reason = reason;
    header.FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    append(buffer, header, name);
}

std::vector<Parsed> parse(const std::vector<std::byte>& buffer, std::int64_t* next = nullptr) {
    std::vector<Parsed> records;
    const auto result = parse_usn_buffer(buffer, [&](const UsnRecord& record) {
        records.push_back(
            {record.usn, record.frn, record.parent_frn, record.reason, std::wstring(record.name)});
    });
    if (next) {
        *next = result;
    }
    return records;
}

TEST(UsnParseTest, ReadsVersion2Records) {
    auto buffer = buffer_with_next(4096);
    append_v2(buffer, 1000, 0x0001000000000100, 0x0005000000000005, USN_REASON_FILE_CREATE, L"setup.log");
    append_v2(buffer, 1096, 0x0001000000000100, 0x0005000000000005, USN_REASON_FILE_CREATE | USN_REASON_CLOSE,
              L"setup.log");
    std::int64_t next = 0;
    const std::vector<Parsed> expected = {
        {1000, 0x0001000000000100, 0x0005000000000005, USN_REASON_FILE_CREATE, L"setup.log"},
        {1096, 0x0001000000000100, 0x0005000000000005, USN_REASON_FILE_CREATE | USN_REASON_CLOSE,
         L"setup.log"},
    };
    EXPECT_EQ(parse(buffer, &next), expected);
    EXPECT_EQ(next, 4096);
}

TEST(UsnParseTest, ReadsVersion3RecordsByTheirLow64Bits) {
    auto buffer = buffer_with_next(8192);
    append_v3(buffer, 2000, 0x0002000000000200, 0, 0x0005000000000005, USN_REASON_RENAME_NEW_NAME,
              L"Contoso");
    UsnRecord seen;
    parse_usn_buffer(buffer, [&](const UsnRecord& record) { seen = record; });
    EXPECT_EQ(seen.frn, 0x0002000000000200u);
    EXPECT_EQ(seen.parent_frn, 0x0005000000000005u);
    EXPECT_EQ(seen.attributes, static_cast<std::uint32_t>(FILE_ATTRIBUTE_DIRECTORY));
    const std::vector<Parsed> expected = {
        {2000, 0x0002000000000200, 0x0005000000000005, USN_REASON_RENAME_NEW_NAME, L"Contoso"}};
    EXPECT_EQ(parse(buffer), expected);
}

TEST(UsnParseTest, SkipsIdsThatNeedMoreThan64Bits) {
    auto buffer = buffer_with_next(8192);
    append_v3(buffer, 2000, 0x200, 0x1, 0x0005000000000005, USN_REASON_FILE_CREATE, L"refs-only");
    append_v2(buffer, 2100, 0x300, 0x0005000000000005, USN_REASON_FILE_CREATE, L"kept");
    const auto records = parse(buffer);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].name, L"kept");
}

TEST(UsnParseTest, SkipsUnknownVersions) {
    auto buffer = buffer_with_next(8192);
    USN_RECORD_V2 v4{};  // A range-tracking record has another layout; only its header matters here
    v4.MajorVersion = 4;
    append(buffer, v4, L"ranges");
    append_v2(buffer, 2100, 0x300, 0x0005000000000005, USN_REASON_DATA_EXTEND, L"after");
    const auto records = parse(buffer);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].name, L"after");
}

TEST(UsnParseTest, StopsAtAZeroLength) {
    auto buffer = buffer_with_next(8192);
    append_v2(buffer, 1000, 0x100, 0x0005000000000005, USN_REASON_FILE_CREATE, L"first");
    const std::size_t zero_at = buffer.size();
    append_v2(buffer, 1100, 0x200, 0x0005000000000005, USN_REASON_FILE_CREATE, L"never");
    std::memset(buffer.data() + zero_at, 0, sizeof(DWORD));
    const auto records = parse(buffer);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].name, L"first");
}

TEST(UsnParseTest, StopsAtARecordRunningPastTheBuffer) {
    auto buffer = buffer_with_next(8192);
    append_v2(buffer, 1000, 0x100, 0x0005000000000005, USN_REASON_FILE_CREATE, L"whole");
    append_v2(buffer, 1100, 0x200, 0x0005000000000005, USN_REASON_FILE_CREATE, L"cut off");
    buffer.resize(buffer.size() - 8);
    const auto records = parse(buffer);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].name, L"whole");
}

TEST(UsnParseTest, SkipsARecordWhoseNameLiesOutsideIt) {
    auto buffer = buffer_with_next(8192);
    const std::size_t bad_at = buffer.size();
    append_v2(buffer, 1000, 0x100, 0x0005000000000005, USN_REASON_FILE_CREATE, L"bad");
    append_v2(buffer, 1100, 0x200, 0x0005000000000005, USN_REASON_FILE_CREATE, L"good");
    const WORD length = 0x400;
    std::memcpy(buffer.data() + bad_at + offsetof(USN_RECORD_V2, FileNameLength), &length, sizeof(length));
    const auto records = parse(buffer);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].name, L"good");
}

TEST(UsnParseTest, ABufferWithoutRecords) {
    EXPECT_TRUE(parse(buffer_with_next(512)).empty());
    std::int64_t next = -1;
    EXPECT_TRUE(parse(std::vector<std::byte>(4), &next).empty());
    EXPECT_EQ(next, 0);
}

}  // namespace
}  // namespace avel::platform
