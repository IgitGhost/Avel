// The known spots' values, captured byte for byte at a trace's start and stop in the volatile
// scratch key standing in for the real keys, compared, and kept as JSON exactly.
#include <gtest/gtest.h>

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/platform/nt_registry.h"
#include "core/platform/text.h"
#include "core/tracker/known_values.h"
#include "scratch_registry.h"

namespace avel::tracker {
namespace {

using platform::Hive;
using platform::RawValue;
using platform::RegistryAddress;

const RegistryAddress kMachineEnvironment{Hive::Machine, L"",
                                          LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)"};
const RegistryAddress kSharedDlls{Hive::Machine, L"",
                                  LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)"};
const RegistryAddress kUserEnvironment{Hive::User, L"S-1-5-21-1-2-3-1001", L"Environment"};

// The characters without a terminator, as some programs store them.
std::vector<std::byte> bytes_of(std::wstring_view text) {
    std::vector<std::byte> data(text.size() * sizeof(wchar_t));
    std::memcpy(data.data(), text.data(), data.size());
    return data;
}

std::vector<std::byte> dword_bytes(DWORD value) {
    std::vector<std::byte> data(sizeof(value));
    std::memcpy(data.data(), &value, sizeof(value));
    return data;
}

KnownRoot root_at(const testing::ScratchKey& scratch, KnownSpot spot, std::wstring_view subpath,
                  const RegistryAddress& address) {
    return {spot, address, scratch.get(), std::wstring(subpath), nullptr};
}

void expect_same(const RawValue& actual, const RawValue& expected) {
    EXPECT_EQ(actual.name, expected.name);
    EXPECT_EQ(actual.type, expected.type) << platform::to_utf8(expected.name);
    EXPECT_EQ(actual.data, expected.data) << platform::to_utf8(expected.name);
}

void expect_same(const std::optional<RawValue>& actual, const std::optional<RawValue>& expected) {
    ASSERT_EQ(actual.has_value(), expected.has_value());
    if (expected) {
        expect_same(*actual, *expected);
    }
}

void expect_same(const KnownValueChange& actual, const KnownValueChange& expected) {
    EXPECT_EQ(actual.spot, expected.spot);
    EXPECT_TRUE(platform::same_address(actual.key, expected.key))
        << platform::to_utf8(display_path(actual.key));
    EXPECT_EQ(actual.name, expected.name);
    expect_same(actual.before, expected.before);
    expect_same(actual.after, expected.after);
}

TEST(KnownValuesTest, FindsAddedChangedAndDeletedValues) {
    testing::ScratchKey scratch;
    const RawValue path{L"Path", REG_EXPAND_SZ, bytes_of(L"C:\\A;%ProgramFiles%\\B")};
    const RawValue gone{L"Gone", REG_SZ, bytes_of(std::wstring(L"x\0", 2))};
    const RawValue same{L"Same", REG_SZ, bytes_of(L"y")};
    const auto environment = scratch.create(L"Environment");
    for (const auto* value : {&path, &gone, &same}) {
        ASSERT_EQ(platform::set_raw_value(environment.get(), *value), ERROR_SUCCESS);
    }
    const std::vector roots{
        root_at(scratch, KnownSpot::MachineEnvironment, L"Environment", kMachineEnvironment),
        root_at(scratch, KnownSpot::SharedDlls, L"SharedDLLs", kSharedDlls),
        root_at(scratch, KnownSpot::UserEnvironment, L"Missing", kUserEnvironment)};
    const KnownValues before = capture_known_values(roots);
    ASSERT_EQ(before.keys.size(), 3u);
    EXPECT_TRUE(before.keys[0].present);
    ASSERT_EQ(before.keys[0].values.size(), 3u);
    expect_same(before.keys[0].values[0], gone);  // Sorted by name
    expect_same(before.keys[0].values[1], path);
    expect_same(before.keys[0].values[2], same);
    EXPECT_FALSE(before.keys[1].present);  // The key is missing at the start
    EXPECT_TRUE(before.keys[1].values.empty());

    const RawValue longer{L"Path", REG_EXPAND_SZ, bytes_of(L"C:\\A;%ProgramFiles%\\B;C:\\Contoso\\bin")};
    const RawValue added{
        std::wstring(L"Added\xD800"), REG_BINARY, {std::byte{1}, std::byte{0}, std::byte{0xff}}};
    const RawValue counted{L"C:\\Shared\\contoso.dll", REG_DWORD, dword_bytes(1)};
    ASSERT_EQ(platform::set_raw_value(environment.get(), longer), ERROR_SUCCESS);
    ASSERT_EQ(platform::set_raw_value(environment.get(), added), ERROR_SUCCESS);
    ASSERT_EQ(platform::set_raw_value(environment.get(), same), ERROR_SUCCESS);  // Rewritten as it was
    ASSERT_EQ(::RegDeleteValueW(environment.get(), L"Gone"), ERROR_SUCCESS);
    ASSERT_EQ(platform::set_raw_value(scratch.create(L"SharedDLLs").get(), counted), ERROR_SUCCESS);
    const KnownValues after = capture_known_values(roots);

    const auto changes = diff_known_values(before, after);
    ASSERT_EQ(changes.size(), 4u);
    expect_same(changes[0],
                {KnownSpot::MachineEnvironment, kMachineEnvironment, added.name, std::nullopt, added});
    expect_same(changes[1],
                {KnownSpot::MachineEnvironment, kMachineEnvironment, L"Gone", gone, std::nullopt});
    expect_same(changes[2], {KnownSpot::MachineEnvironment, kMachineEnvironment, L"Path", path, longer});
    expect_same(changes[3], {KnownSpot::SharedDlls, kSharedDlls, counted.name, std::nullopt, counted});
}

TEST(KnownValuesTest, KeysThatCantBeReadAreLeftOut) {
    testing::ScratchKey scratch;
    scratch.set_string(L"Environment", L"Path", L"C:\\A");
    std::vector roots{
        root_at(scratch, KnownSpot::MachineEnvironment, L"Environment", kMachineEnvironment),
        // The same key by another name is read once.
        root_at(scratch, KnownSpot::SharedDlls, L"Environment", kSharedDlls),
        KnownRoot{KnownSpot::UserEnvironment, kUserEnvironment, nullptr, L"Environment", nullptr}};
    const KnownValues captured = capture_known_values(roots);
    ASSERT_EQ(captured.keys.size(), 1u);
    EXPECT_EQ(captured.keys[0].spot, KnownSpot::MachineEnvironment);

    // A key captured at one end only isn't compared.
    EXPECT_TRUE(diff_known_values(captured, KnownValues{}).empty());
    EXPECT_TRUE(diff_known_values(KnownValues{}, captured).empty());
}

TEST(KnownValuesTest, JsonKeepsValuesExactly) {
    KnownValues values;
    values.keys.push_back({KnownSpot::MachineEnvironment,
                           kMachineEnvironment,
                           {{L"", REG_SZ, {}},
                            {std::wstring(L"odd\xD800"), REG_BINARY, {std::byte{0}, std::byte{0xff}}},
                            {L"Path", REG_EXPAND_SZ, bytes_of(L"unterminated \x0531\x057E\x0565\x056C")}},
                           true});
    values.keys.push_back({KnownSpot::UserEnvironment, kUserEnvironment, {}, false});
    values.keys.push_back({KnownSpot::SharedDlls,
                           kSharedDlls,
                           {{L"C:\\Shared\\contoso.dll", REG_DWORD, dword_bytes(3)}},
                           true});

    const std::string text = to_json(values).dump();
    KnownValues loaded;
    ASSERT_TRUE(from_json(changes::Json::parse(text), loaded));
    EXPECT_EQ(to_json(loaded).dump(), text);
    ASSERT_EQ(loaded.keys.size(), values.keys.size());
    for (std::size_t i = 0; i < values.keys.size(); ++i) {
        EXPECT_EQ(loaded.keys[i].spot, values.keys[i].spot);
        EXPECT_TRUE(platform::same_address(loaded.keys[i].key, values.keys[i].key));
        EXPECT_EQ(loaded.keys[i].present, values.keys[i].present);
        ASSERT_EQ(loaded.keys[i].values.size(), values.keys[i].values.size());
        for (std::size_t j = 0; j < values.keys[i].values.size(); ++j) {
            expect_same(loaded.keys[i].values[j], values.keys[i].values[j]);
        }
    }

    const RawValue before{std::wstring(L"Path\xDC00"), REG_EXPAND_SZ, bytes_of(L"C:\\A")};
    const RawValue after{std::wstring(L"Path\xDC00"), REG_EXPAND_SZ, bytes_of(L"C:\\A;C:\\B")};
    const KnownValueChange changes[] = {
        {KnownSpot::MachineEnvironment, kMachineEnvironment, before.name, before, after},
        {KnownSpot::UserEnvironment, kUserEnvironment, before.name, std::nullopt, after},
        {KnownSpot::SharedDlls, kSharedDlls, before.name, before, std::nullopt},
    };
    for (const auto& change : changes) {
        const std::string change_text = to_json(change).dump();
        KnownValueChange loaded_change;
        ASSERT_TRUE(from_json(changes::Json::parse(change_text), loaded_change)) << change_text;
        EXPECT_EQ(to_json(loaded_change).dump(), change_text);
        expect_same(loaded_change, change);
    }

    KnownValues ignored;
    KnownValueChange ignored_change;
    EXPECT_FALSE(from_json(changes::Json::parse("{}"), ignored));
    EXPECT_FALSE(from_json(changes::Json::parse(R"({"keys":[{"spot":"nowhere"}]})"), ignored));
    EXPECT_FALSE(from_json(changes::Json::parse("[]"), ignored_change));
    auto damaged = to_json(changes[0]);
    damaged["after"]["data"] = "zz";
    EXPECT_FALSE(from_json(damaged, ignored_change));
}

}  // namespace
}  // namespace avel::tracker
