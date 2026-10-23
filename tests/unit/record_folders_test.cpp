// Record ids and times as the ChangeStore and the TraceStore write them. The folders themselves
// (locks, listing) are exercised through the ChangeStore's tests.
#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <string>

#include "core/platform/paths.h"
#include "core/platform/record_folders.h"
#include "core/platform/text.h"

namespace avel::platform {
namespace {

using namespace std::chrono_literals;

TEST(RecordFoldersTest, IdsStartWithTheTimeInUtc) {
    const std::chrono::system_clock::time_point time =
        std::chrono::sys_days{2026y / 10 / 23} + 14h + 5min + 9s + 300ms;
    const std::wstring id = new_record_id(time);
    ASSERT_EQ(id.size(), 24u);
    EXPECT_EQ(id.substr(0, 16), L"20261023-140509-");
    EXPECT_EQ(id.find_first_not_of(L"0123456789abcdef", 16), std::wstring::npos);
    EXPECT_NE(new_record_id(time), id);
    EXPECT_TRUE(is_safe_record_id(id));

    EXPECT_EQ(iso_time(time), "2026-10-23T14:05:09Z");
    EXPECT_EQ(from_unix(unix_seconds(time)), std::chrono::floor<std::chrono::seconds>(time));
}

TEST(RecordFoldersTest, UnsafeIdsAreRefused) {
    for (const std::wstring_view id : {L"", L".", L"..", L"a\\b", L"a/b", L"C:x", L"..\\x"}) {
        EXPECT_FALSE(is_safe_record_id(id)) << to_utf8(id);
    }
}

TEST(RecordFoldersTest, ProgramDataFolderIsUnderAvel) {
    wchar_t program_data[MAX_PATH] = {};
    ASSERT_GT(::GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH), 0u);
    EXPECT_TRUE(paths_equal(program_data_folder(L"Traces"), std::wstring(program_data) + LR"(\Avel\Traces)"));
}

}  // namespace
}  // namespace avel::platform
