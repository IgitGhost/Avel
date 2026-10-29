// Locations through the ChangeSet's JSON (plan.json, and a trace's changes.json): every kind comes
// back exactly, value edits with their values byte for byte and names UTF-8 can't carry.
#include <gtest/gtest.h>

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/changes/serialization.h"
#include "core/platform/text.h"

namespace avel::changes {
namespace {

using leftovers::Location;
using leftovers::TaskLocation;
using leftovers::ValueEditKind;
using leftovers::ValueEditLocation;
using platform::Hive;
using platform::RawValue;

RawValue raw(std::wstring name, DWORD type, std::vector<std::byte> data) {
    return {std::move(name), type, std::move(data)};
}

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> data;
    for (const int value : values) {
        data.push_back(static_cast<std::byte>(value));
    }
    return data;
}

Location round_trip(const Location& location) {
    const std::string text = to_json(location).dump();
    Location loaded;
    EXPECT_TRUE(from_json(Json::parse(text), loaded)) << text;
    return loaded;
}

void expect_same(const std::optional<RawValue>& actual, const std::optional<RawValue>& expected) {
    ASSERT_EQ(actual.has_value(), expected.has_value());
    if (expected) {
        EXPECT_EQ(actual->name, expected->name);
        EXPECT_EQ(actual->type, expected->type);
        EXPECT_EQ(actual->data, expected->data);
    }
}

TEST(SerializationTest, EveryLocationKindRoundTrips) {
    const std::vector<Location> locations = {
        leftovers::FileLocation{LR"(C:\Program Files\Contoso)", true},
        leftovers::FileLocation{LR"(C:\Program Files\Contoso\app.exe)", false},
        leftovers::RegistryKeyLocation{{Hive::Machine, L"", LR"(SOFTWARE\Contoso)"}},
        leftovers::RegistryValueLocation{{Hive::User, L"S-1-5-21-1-2-3-1001", LR"(Software\Run)"},
                                         L"odd \xD800 name"},
        leftovers::ServiceLocation{L"ContosoService", true},
        TaskLocation{LR"(\Contoso\Updater)"},
        TaskLocation{LR"(\Contoso)", true},
        leftovers::PackageLocation{L"Contoso.App_1.0.0.0_x64__abc", L"S-1-5-21-1-2-3-1001"},
        leftovers::FirewallRuleLocation{L"{rule}"},
    };
    for (const auto& location : locations) {
        const Location loaded = round_trip(location);
        EXPECT_EQ(loaded.index(), location.index());
        EXPECT_EQ(leftovers::identity(loaded), leftovers::identity(location));
    }
    EXPECT_FALSE(std::get<TaskLocation>(round_trip(TaskLocation{LR"(\Contoso\Updater)"})).folder);
    EXPECT_TRUE(std::get<TaskLocation>(round_trip(TaskLocation{LR"(\Contoso)", true})).folder);
    EXPECT_TRUE(
        std::get<leftovers::ServiceLocation>(round_trip(leftovers::ServiceLocation{L"x", true})).driver);
}

TEST(SerializationTest, ATaskWithoutTheFolderFlagIsATask) {
    // Plans written before task folders existed.
    Location loaded;
    ASSERT_TRUE(from_json(Json::parse(R"({"kind":"task","path":"\\Contoso\\Updater"})"), loaded));
    const auto& task = std::get<TaskLocation>(loaded);
    EXPECT_EQ(task.path, LR"(\Contoso\Updater)");
    EXPECT_FALSE(task.folder);
    EXPECT_FALSE(to_json(Location(TaskLocation{L"\\x"})).contains("folder"));
}

TEST(SerializationTest, ValueEditsRoundTripExactly) {
    const std::wstring odd = L"C:\\Contoso\\b\xDC00n";  // An unpaired surrogate
    // An unterminated REG_EXPAND_SZ with an odd trailing byte, edited to one with a terminator.
    ValueEditLocation removal{{Hive::User, L"S-1-5-21-1-2-3-1001", L"Environment"},
                              L"Path",
                              ValueEditKind::RemovePathEntries,
                              raw(L"Path", REG_EXPAND_SZ, bytes({'C', 0, ':', 0, ';', 0, 'x', 0, 0x7f})),
                              raw(L"Path", REG_SZ, bytes({'C', 0, ':', 0, 0, 0})),
                              {odd, L"", L"D:\\tools"},
                              0};
    ValueEditLocation decrement{
        {Hive::Machine, L"", LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\SharedDLLs)"},
        L"C:\\Shared\\x\xD800.dll",
        ValueEditKind::DecrementSharedDll,
        raw(L"C:\\Shared\\x\xD800.dll", REG_DWORD, bytes({3, 0, 0, 0})),
        raw(L"C:\\Shared\\x\xD800.dll", REG_DWORD, bytes({1, 0, 0, 0})),
        {},
        2};
    // Absent before (a deleted variable put back), and deleted (a created one).
    ValueEditLocation restore{
        {Hive::Machine, L"", LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)"},
        L"CONTOSO_OLD",
        ValueEditKind::RestoreValue,
        std::nullopt,
        raw(L"CONTOSO_OLD", REG_BINARY, {}),
        {},
        0};
    ValueEditLocation deletion{
        restore.key, L"", ValueEditKind::DeleteValue, raw(L"", REG_SZ, bytes({0, 0})), std::nullopt, {}, 0};
    for (const auto& edit : {removal, decrement, restore, deletion}) {
        const Location loaded = round_trip(edit);
        ASSERT_TRUE(std::holds_alternative<ValueEditLocation>(loaded));
        const auto& back = std::get<ValueEditLocation>(loaded);
        EXPECT_TRUE(platform::same_address(back.key, edit.key));
        EXPECT_EQ(back.key.hive, edit.key.hive);
        EXPECT_EQ(back.value_name, edit.value_name);
        EXPECT_EQ(back.kind, edit.kind);
        expect_same(back.expected, edit.expected);
        expect_same(back.desired, edit.desired);
        EXPECT_EQ(back.entries, edit.entries);
        EXPECT_EQ(back.delta, edit.delta);
        EXPECT_EQ(leftovers::identity(loaded), leftovers::identity(edit));
    }
}

TEST(SerializationTest, MalformedValueEditsAreRejected) {
    const Json good = to_json(Location(
        ValueEditLocation{{Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)"},
                          L"C:\\x.dll",
                          ValueEditKind::DeleteValue,
                          raw(L"C:\\x.dll", REG_DWORD, bytes({1, 0, 0, 0})),
                          std::nullopt,
                          {},
                          1}));
    Location loaded;
    ASSERT_TRUE(from_json(good, loaded));
    for (const char* field : {"edit", "expected", "desired", "entries", "delta", "value", "hive"}) {
        Json broken = good;
        broken.erase(field);
        EXPECT_FALSE(from_json(broken, loaded)) << field;
    }
    for (const auto& [field, value] : {std::pair<const char*, Json>{"edit", "shrink-value"},
                                       {"expected", Json{{"type", 4}, {"data", "zz"}}},
                                       {"desired", "text"},
                                       {"entries", Json{1, 2}},
                                       {"delta", "1"}}) {
        Json broken = good;
        broken[field] = value;
        EXPECT_FALSE(from_json(broken, loaded)) << field;
    }
}

TEST(SerializationTest, AValueEditsIdentityHoldsWhatItDoes) {
    const leftovers::Location first = ValueEditLocation{
        {Hive::Machine, L"", LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)"},
        L"Path",
        ValueEditKind::RemovePathEntries,
        std::nullopt,
        std::nullopt,
        {LR"(C:\Contoso\bin)"},
        0};
    leftovers::Location second = first;
    std::get<ValueEditLocation>(second).entries = {LR"(C:\Other\bin)"};
    EXPECT_NE(leftovers::identity(first), leftovers::identity(second));
    EXPECT_TRUE(leftovers::identity(first).starts_with(L"edit:hklm\\system\\"));
    EXPECT_NE(leftovers::identity(TaskLocation{L"\\Contoso"}),
              leftovers::identity(TaskLocation{L"\\Contoso", true}));
}

}  // namespace
}  // namespace avel::changes
