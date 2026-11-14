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
using leftovers::PackageLocation;
using leftovers::PackageProvisioningLocation;
using leftovers::PackageUserLocation;
using leftovers::ServiceStart;
using leftovers::ServiceStartLocation;
using leftovers::StartupApprovedLocation;
using leftovers::TaskLocation;
using leftovers::TaskStateLocation;
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
        StartupApprovedLocation{
            {Hive::User, L"S-1-5-21-1-2-3-1001",
             LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)"},
            L"Contoso",
            std::nullopt,
            raw(L"Contoso", REG_BINARY, bytes({3, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8}))},
        TaskStateLocation{LR"(\Contoso\Updater)", true, false},
        ServiceStartLocation{L"ContosoService", {SERVICE_AUTO_START, true}, {SERVICE_DEMAND_START, true}},
        PackageLocation{L"Contoso.App_1.0.0.0_x64__abc", L"", true},
        PackageUserLocation{L"Contoso.App_abc", L"Contoso.App_1.0.0.0_x64__abc", L"S-1-5-21-1-2-3-1001", true,
                            false},
        PackageProvisioningLocation{L"Contoso.App_abc", L"Contoso.App_1.0.0.0_x64__abc", true, false},
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

TEST(SerializationTest, StateChangesRoundTripExactly) {
    // A user's approval with a name UTF-8 can't carry, disabled from enabled; the machine's, deleted.
    const StartupApprovedLocation disable{
        {Hive::User, L"S-1-5-21-1-2-3-1001",
         LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)"},
        L"Contoso \xD800 Agent",
        raw(L"Contoso \xD800 Agent", REG_BINARY, bytes({2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})),
        raw(L"Contoso \xD800 Agent", REG_BINARY,
            bytes({3, 0, 0, 0, 0x7f, 0x6e, 0x5d, 0x4c, 0x3b, 0x2a, 0xdb, 1}))};
    const StartupApprovedLocation clear{
        {Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run32)"},
        L"",
        raw(L"", REG_BINARY, bytes({1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})),
        std::nullopt};
    for (const auto& approval : {disable, clear}) {
        const Location loaded = round_trip(approval);
        ASSERT_TRUE(std::holds_alternative<StartupApprovedLocation>(loaded));
        const auto& back = std::get<StartupApprovedLocation>(loaded);
        EXPECT_TRUE(platform::same_address(back.key, approval.key));
        EXPECT_EQ(back.value_name, approval.value_name);
        expect_same(back.expected, approval.expected);
        expect_same(back.desired, approval.desired);
    }

    for (const bool enable : {false, true}) {
        const Location loaded = round_trip(TaskStateLocation{LR"(\Contoso\Updater)", !enable, enable});
        const auto& task = std::get<TaskStateLocation>(loaded);
        EXPECT_EQ(task.path, LR"(\Contoso\Updater)");
        EXPECT_EQ(task.enabled_before, !enable);
        EXPECT_EQ(task.enabled_after, enable);
    }

    const ServiceStartLocation service{
        L"Contoso\xDC00Svc", {SERVICE_AUTO_START, true}, {SERVICE_DISABLED, false}};
    const Location loaded = round_trip(service);
    const auto& back = std::get<ServiceStartLocation>(loaded);
    EXPECT_EQ(back.name, service.name);
    EXPECT_EQ(back.before, service.before);
    EXPECT_EQ(back.after, service.after);
}

TEST(SerializationTest, AStateChangesIdentityNamesOnlyTheItem) {
    const auto key = leftovers::StartupApprovedLocation{
        {Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)"},
        L"Contoso",
        std::nullopt,
        std::nullopt};
    auto other = key;
    other.desired = raw(L"Contoso", REG_BINARY, bytes({2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(leftovers::identity(key), leftovers::identity(other));
    EXPECT_EQ(
        leftovers::identity(key),
        LR"(approved:hklm\software\microsoft\windows\currentversion\explorer\startupapproved\run\\contoso)");
    EXPECT_EQ(leftovers::identity(TaskStateLocation{LR"(\Contoso\Updater)", true, false}),
              leftovers::identity(TaskStateLocation{LR"(\CONTOSO\Updater)", false, true}));
    EXPECT_EQ(leftovers::identity(ServiceStartLocation{L"Contoso", {2, false}, {3, false}}),
              L"service-start:contoso");
    // Never the same item as what removes it.
    EXPECT_NE(leftovers::identity(TaskStateLocation{LR"(\Contoso)"}),
              leftovers::identity(TaskLocation{LR"(\Contoso)"}));
    EXPECT_NE(leftovers::identity(ServiceStartLocation{L"Contoso"}),
              leftovers::identity(leftovers::ServiceLocation{L"Contoso"}));
    EXPECT_EQ(leftovers::owner_sid(StartupApprovedLocation{{Hive::User, L"S-1-5-21-9", L"Software"}, L"x"}),
              L"S-1-5-21-9");
    EXPECT_EQ(leftovers::category_of(TaskStateLocation{}), leftovers::Category::Tasks);
    EXPECT_EQ(leftovers::category_of(ServiceStartLocation{}), leftovers::Category::Services);
    EXPECT_EQ(leftovers::category_of(key), leftovers::Category::Registry);
}

TEST(SerializationTest, MalformedStateChangesAreRejected) {
    const Json approval = to_json(Location(StartupApprovedLocation{
        {Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)"},
        L"Contoso",
        std::nullopt,
        raw(L"Contoso", REG_BINARY, bytes({2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}))}));
    const Json task = to_json(Location(TaskStateLocation{LR"(\Contoso\Updater)", true, false}));
    const Json service = to_json(Location(ServiceStartLocation{L"Contoso", {2, false}, {3, false}}));
    Location loaded;
    for (const auto* good : {&approval, &task, &service}) {
        ASSERT_TRUE(from_json(*good, loaded)) << good->dump();
    }
    const auto expect_rejected = [&](const Json& good, const char* field, std::optional<Json> value) {
        Json broken = good;
        if (value) {
            broken[field] = *value;
        } else {
            broken.erase(field);
        }
        EXPECT_FALSE(from_json(broken, loaded)) << broken.dump();
    };
    for (const char* field : {"hive", "path", "value", "expected", "desired"}) {
        expect_rejected(approval, field, std::nullopt);
    }
    expect_rejected(approval, "expected", Json{{"type", 3}, {"data", "zz"}});
    expect_rejected(approval, "desired", Json("02000000"));
    for (const char* field : {"path", "enabled_before", "enabled_after"}) {
        expect_rejected(task, field, std::nullopt);
    }
    expect_rejected(task, "enabled_after", Json(0));
    expect_rejected(task, "enabled_before", Json("true"));
    for (const char* field : {"name", "before", "after"}) {
        expect_rejected(service, field, std::nullopt);
    }
    expect_rejected(service, "before", Json{{"start_type", 2}});
    expect_rejected(service, "after", Json{{"start_type", -1}, {"delayed", false}});
    expect_rejected(service, "after", Json{{"start_type", 0x100000000ull}, {"delayed", false}});
    expect_rejected(service, "after", Json{{"start_type", 3}, {"delayed", 1}});
    expect_rejected(service, "before", Json::array());
}

TEST(SerializationTest, PackageStatesRoundTripExactly) {
    for (const bool after : {false, true}) {
        const PackageUserLocation registration{L"Contoso.App_abc", L"Contoso.App_1.0.0.0_x64__abc",
                                               L"S-1-5-21-1-2-3-1001", !after, after};
        const auto user = std::get<PackageUserLocation>(round_trip(registration));
        EXPECT_EQ(user.family_name, registration.family_name);
        EXPECT_EQ(user.full_name, registration.full_name);
        EXPECT_EQ(user.user_sid, registration.user_sid);
        EXPECT_EQ(user.before, !after);
        EXPECT_EQ(user.after, after);

        // A name UTF-8 can't carry still comes back.
        const PackageProvisioningLocation provisioning{
            L"Contoso.\xD800"
            L"App_abc",
            after ? L"" : L"Contoso.App_1_x64__abc", !after, after};
        const auto back = std::get<PackageProvisioningLocation>(round_trip(provisioning));
        EXPECT_EQ(back.family_name, provisioning.family_name);
        EXPECT_EQ(back.provisioned_full_name, provisioning.provisioned_full_name);
        EXPECT_EQ(back.before, !after);
        EXPECT_EQ(back.after, after);
    }
    const auto everyone =
        std::get<PackageLocation>(round_trip(PackageLocation{L"Contoso.App_1_x64__abc", L"", true}));
    EXPECT_TRUE(everyone.all_users);
    EXPECT_FALSE(to_json(Location(PackageLocation{L"x", L"S-1"})).contains("all_users"));

    // Plans written before deep removal.
    Location loaded;
    ASSERT_TRUE(from_json(Json::parse(R"({"kind":"package","full_name":"x","user_sid":"S-1"})"), loaded));
    EXPECT_FALSE(std::get<PackageLocation>(loaded).all_users);
}

TEST(SerializationTest, APackageStatesIdentityNamesOnlyTheItem) {
    const PackageUserLocation registration{L"Contoso.App_ABC", L"Contoso.App_1.0.0.0_x64__ABC", L"S-1-5-21-9",
                                           true, false};
    EXPECT_EQ(leftovers::identity(registration), L"package-user:s-1-5-21-9\\contoso.app_abc");
    EXPECT_EQ(leftovers::identity(registration),
              leftovers::identity(PackageUserLocation{L"contoso.app_abc", L"Contoso.App_2_x64__ABC",
                                                      L"S-1-5-21-9", false, true}));
    EXPECT_NE(leftovers::identity(registration),
              leftovers::identity(PackageUserLocation{L"Contoso.App_ABC", L"", L"S-1-5-21-10", true, false}));
    EXPECT_EQ(leftovers::identity(PackageProvisioningLocation{L"Contoso.App_ABC", L"", true, false}),
              L"package-provisioning:contoso.app_abc");
    EXPECT_EQ(leftovers::identity(PackageLocation{L"Contoso.App_1_x64__ABC", L"S-1-5-21-9", true}),
              L"package:all-users:contoso.app_1_x64__abc");
    EXPECT_EQ(leftovers::identity(PackageLocation{L"Contoso.App_1_x64__ABC", L"S-1-5-21-9"}),
              L"package:s-1-5-21-9:contoso.app_1_x64__abc");
    EXPECT_EQ(leftovers::owner_sid(registration), L"S-1-5-21-9");
    EXPECT_EQ(leftovers::owner_sid(PackageProvisioningLocation{L"Contoso.App_ABC"}), L"");
    EXPECT_EQ(leftovers::owner_sid(PackageLocation{L"x", L"S-1-5-21-9", true}), L"");
    EXPECT_EQ(leftovers::category_of(registration), leftovers::Category::Packages);
    EXPECT_EQ(leftovers::category_of(PackageProvisioningLocation{}), leftovers::Category::Packages);

    // A removal for every user holds the family's registrations and provisioning; nothing else does.
    const PackageLocation everyone{L"Contoso.App_1.0.0.0_x64__ABC", L"", true};
    EXPECT_EQ(leftovers::package_family_name(everyone.full_name), L"Contoso.App_ABC");
    EXPECT_EQ(leftovers::package_family_name(L"Contoso.App_1.0.0.0_x64_abc"), L"");
    EXPECT_TRUE(leftovers::contains(everyone, registration));
    EXPECT_TRUE(leftovers::contains(everyone, PackageProvisioningLocation{L"contoso.app_abc"}));
    EXPECT_FALSE(leftovers::contains(everyone, PackageProvisioningLocation{L"Contoso.Other_ABC"}));
    EXPECT_FALSE(leftovers::contains(PackageLocation{everyone.full_name, L"S-1-5-21-9"}, registration));
}

TEST(SerializationTest, MalformedPackageStatesAreRejected) {
    const Json user = to_json(Location(
        PackageUserLocation{L"Contoso.App_abc", L"Contoso.App_1_x64__abc", L"S-1-5-21-9", true, false}));
    const Json provisioning =
        to_json(Location(PackageProvisioningLocation{L"Contoso.App_abc", L"", true, false}));
    const Json package = to_json(Location(PackageLocation{L"Contoso.App_1_x64__abc", L"", true}));
    Location loaded;
    for (const auto* good : {&user, &provisioning, &package}) {
        ASSERT_TRUE(from_json(*good, loaded)) << good->dump();
    }
    const auto expect_rejected = [&](const Json& good, const char* field, std::optional<Json> value) {
        Json broken = good;
        if (value) {
            broken[field] = *value;
        } else {
            broken.erase(field);
        }
        EXPECT_FALSE(from_json(broken, loaded)) << broken.dump();
    };
    for (const char* field : {"family_name", "full_name", "user_sid", "before", "after"}) {
        expect_rejected(user, field, std::nullopt);
    }
    expect_rejected(user, "family_name", Json(""));
    expect_rejected(user, "user_sid", Json(""));
    expect_rejected(user, "before", Json(1));
    expect_rejected(user, "after", Json("false"));
    for (const char* field : {"family_name", "provisioned_full_name", "before", "after"}) {
        expect_rejected(provisioning, field, std::nullopt);
    }
    expect_rejected(provisioning, "family_name", Json(""));
    expect_rejected(provisioning, "after", Json(nullptr));
    expect_rejected(package, "all_users", Json(1));
    expect_rejected(package, "all_users", Json("true"));
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
