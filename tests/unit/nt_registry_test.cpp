// The native API registry helpers in the volatile scratch key: values byte for byte, subkey names
// with their last write times, and key counts, checked against what the Win32 functions report.
#include <gtest/gtest.h>

#include <windows.h>

#include <wil/resource.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "core/platform/nt_registry.h"
#include "core/platform/text.h"
#include "scratch_registry.h"

namespace avel::platform {
namespace {

std::vector<std::byte> wide(std::wstring_view text) {
    std::vector<std::byte> data(text.size() * sizeof(wchar_t));
    std::memcpy(data.data(), text.data(), data.size());
    return data;
}

FILETIME last_write_of(HKEY key) {
    FILETIME time{};
    EXPECT_EQ(::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, &time),
              ERROR_SUCCESS);
    return time;
}

bool same_time(const FILETIME& a, const FILETIME& b) { return ::CompareFileTime(&a, &b) == 0; }

void expect_value(const std::vector<RawValue>& values, const RawValue& expected) {
    const auto found = std::find_if(values.begin(), values.end(),
                                    [&](const RawValue& value) { return value.name == expected.name; });
    ASSERT_NE(found, values.end()) << to_utf8(expected.name);
    EXPECT_EQ(found->type, expected.type) << to_utf8(expected.name);
    EXPECT_EQ(found->data, expected.data) << to_utf8(expected.name);
}

TEST(NtRegistryTest, ValuesRoundTripByteForByte) {
    testing::ScratchKey scratch;
    const auto key = scratch.create(L"Values");
    const RawValue unterminated{L"unterminated", REG_SZ, wide(L"no terminator")};
    const RawValue empty_default{L"", REG_SZ, {}};
    std::vector<std::byte> big(5000);  // Larger than the first buffer
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<std::byte>(i * 7 + 3);
    }
    const RawValue binary{std::wstring(L"surrogate-\xD800-big"), REG_BINARY, big};
    for (const auto* value : {&unterminated, &empty_default, &binary}) {
        ASSERT_EQ(set_raw_value(key.get(), *value), ERROR_SUCCESS);
    }

    // Stored as given: no terminator added to the string, nothing at all in the default value.
    DWORD size = 1;
    ASSERT_EQ(::RegQueryValueExW(key.get(), L"unterminated", nullptr, nullptr, nullptr, &size),
              ERROR_SUCCESS);
    EXPECT_EQ(size, unterminated.data.size());
    ASSERT_EQ(::RegQueryValueExW(key.get(), L"", nullptr, nullptr, nullptr, &size), ERROR_SUCCESS);
    EXPECT_EQ(size, 0u);

    for (const auto* expected : {&unterminated, &empty_default, &binary}) {
        RawValue value;
        ASSERT_EQ(query_raw_value(key.get(), expected->name, value), ERROR_SUCCESS);
        EXPECT_EQ(value.name, expected->name);
        EXPECT_EQ(value.type, expected->type);
        EXPECT_EQ(value.data, expected->data);
    }

    std::vector<RawValue> values;
    ASSERT_EQ(read_raw_values(key.get(), values), ERROR_SUCCESS);
    ASSERT_EQ(values.size(), 3u);
    expect_value(values, unterminated);
    expect_value(values, empty_default);
    expect_value(values, binary);
}

TEST(NtRegistryTest, AMissingValueIsNotFound) {
    testing::ScratchKey scratch;
    const auto key = scratch.create(L"Values");
    RawValue value;
    EXPECT_EQ(query_raw_value(key.get(), L"no-such-value", value), ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(value.name, L"no-such-value");
    // A new key has no default value, not an empty one.
    EXPECT_EQ(query_raw_value(key.get(), L"", value), ERROR_FILE_NOT_FOUND);
}

TEST(NtRegistryTest, SubkeysComeWithTheirLastWriteTimes) {
    testing::ScratchKey scratch;
    const std::vector<std::wstring> names = {L"Plain", L"\x0531\x057E\x0565\x056C", std::wstring(255, L'n')};
    for (const auto& name : names) {
        scratch.create(L"Parent\\" + name);
    }
    scratch.set_dword(L"Parent\\Plain", L"changed", 1);  // A later time than its siblings, likely
    const auto parent = scratch.create(L"Parent");

    std::vector<SubkeyInfo> subkeys;
    ASSERT_EQ(enum_subkeys(parent.get(), subkeys), ERROR_SUCCESS);
    ASSERT_EQ(subkeys.size(), names.size());
    for (const auto& name : names) {
        const auto found = std::find_if(subkeys.begin(), subkeys.end(),
                                        [&](const SubkeyInfo& subkey) { return subkey.name == name; });
        ASSERT_NE(found, subkeys.end()) << to_utf8(name);
        HKEY raw = nullptr;
        ASSERT_EQ(::RegOpenKeyExW(parent.get(), name.c_str(), 0, KEY_READ, &raw), ERROR_SUCCESS);
        const wil::unique_hkey child(raw);
        EXPECT_TRUE(same_time(found->last_write, last_write_of(child.get()))) << to_utf8(name);
    }

    // It appends, and a key without subkeys adds nothing.
    ASSERT_EQ(enum_subkeys(parent.get(), subkeys), ERROR_SUCCESS);
    EXPECT_EQ(subkeys.size(), 2 * names.size());
    ASSERT_EQ(enum_subkeys(scratch.create(L"Parent\\Plain").get(), subkeys), ERROR_SUCCESS);
    EXPECT_EQ(subkeys.size(), 2 * names.size());
}

TEST(NtRegistryTest, KeyInfoMatchesRegQueryInfoKey) {
    testing::ScratchKey scratch;
    // A class name longer than the first buffer, which the full information carries along.
    std::wstring class_name(700, L'c');
    HKEY raw = nullptr;
    ASSERT_EQ(::RegCreateKeyExW(scratch.get(), L"Info", 0, class_name.data(), REG_OPTION_VOLATILE,
                                KEY_ALL_ACCESS, nullptr, &raw, nullptr),
              ERROR_SUCCESS);
    const wil::unique_hkey key(raw);
    scratch.create(LR"(Info\Short)");
    scratch.create(LR"(Info\A longer subkey name)");
    scratch.set_string(L"Info", L"a value with a long name", L"text");
    scratch.set_dword(L"Info", L"", 7);
    ASSERT_EQ(set_raw_value(key.get(), {L"binary", REG_BINARY, std::vector<std::byte>(300)}), ERROR_SUCCESS);

    KeyInfo info;
    ASSERT_EQ(query_key_info(key.get(), info), ERROR_SUCCESS);
    DWORD subkeys = 0;
    DWORD max_name_length = 0;
    DWORD values = 0;
    DWORD max_value_name_length = 0;
    DWORD max_value_data_size = 0;
    FILETIME last_write{};
    ASSERT_EQ(::RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, &subkeys, &max_name_length, nullptr,
                                 &values, &max_value_name_length, &max_value_data_size, nullptr, &last_write),
              ERROR_SUCCESS);
    EXPECT_EQ(info.subkeys, 2u);
    EXPECT_EQ(info.values, 3u);
    EXPECT_EQ(info.subkeys, subkeys);
    EXPECT_EQ(info.values, values);
    EXPECT_EQ(info.max_name_length, max_name_length);
    EXPECT_EQ(info.max_value_name_length, max_value_name_length);
    EXPECT_EQ(info.max_value_data_size, max_value_data_size);
    EXPECT_EQ(info.max_name_length, std::wstring_view(L"A longer subkey name").size());
    EXPECT_EQ(info.max_value_data_size, 300u);
    EXPECT_TRUE(same_time(info.last_write, last_write));
}

TEST(NtRegistryTest, KernelNameEndsWithThePath) {
    testing::ScratchKey scratch;
    const std::wstring name = to_lower(kernel_name(scratch.get()));
    EXPECT_EQ(name.rfind(L"\\registry\\user\\", 0), 0u) << to_utf8(name);
    EXPECT_TRUE(name.ends_with(L"\\" + to_lower(scratch.path()))) << to_utf8(name);
    EXPECT_EQ(kernel_name(nullptr), L"");
}

}  // namespace
}  // namespace avel::platform
