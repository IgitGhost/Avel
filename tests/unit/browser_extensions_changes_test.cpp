// The browser extensions planner: list policies renumbered without a gap, JSON policies losing only the
// extension's key, registrations taken whole, the refusals; then a removal for real and its undo, in
// scratch keys standing in for HKLM (never the real policies).
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "core/changes/executor.h"
#include "core/platform/nt_registry.h"
#include "core/platform/text.h"
#include "core/tools/browser_extensions.h"
#include "core/tools/browser_extensions_changes.h"
#include "file_tree.h"
#include "registry_dump.h"
#include "scratch_registry.h"

namespace avel::tools {
namespace {

using leftovers::Location;
using leftovers::ProtectionReason;
using leftovers::RegistryKeyLocation;
using leftovers::RegistryValueLocation;
using leftovers::ValueEditKind;
using leftovers::ValueEditLocation;
using platform::Hive;
using platform::RawValue;
using platform::RegistryAddress;
using Code = ExtensionPlanIssueCode;

constexpr wchar_t kForceList[] = LR"(SOFTWARE\Policies\Google\Chrome\ExtensionInstallForcelist)";
constexpr wchar_t kChromePolicy[] = LR"(SOFTWARE\Policies\Google\Chrome)";
constexpr wchar_t kA[] = L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr wchar_t kB[] = L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr wchar_t kC[] = L"cccccccccccccccccccccccccccccccc";
constexpr wchar_t kD[] = L"dddddddddddddddddddddddddddddddd";

RawValue string_value(const std::wstring& name, std::wstring_view text, DWORD type = REG_SZ) {
    const std::size_t terminators = type == REG_MULTI_SZ ? 2 : 1;
    RawValue value{name, type, std::vector<std::byte>((text.size() + terminators) * sizeof(wchar_t))};
    std::memcpy(value.data.data(), text.data(), text.size() * sizeof(wchar_t));
    return value;
}

std::wstring text_of(const std::optional<RawValue>& value) {
    if (!value) {
        return L"<absent>";
    }
    std::wstring text(value->data.size() / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), value->data.data(), text.size() * sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0') {
        text.pop_back();
    }
    return text;
}

RegistryAddress machine(std::wstring path) { return {Hive::Machine, L"", std::move(path)}; }

// A list built by hand, as the listing would make it.
class Fixture {
public:
    // A numbered list: one source per value, each knowing every value of the key.
    void force_list(const std::vector<std::pair<std::wstring, std::wstring>>& values) {
        std::vector<RawValue> raw;
        for (const auto& [name, id] : values) {
            raw.push_back(string_value(name, id));
        }
        for (const auto& value : raw) {
            ExtensionSource source;
            source.kind = ExtensionSourceKind::ForceList;
            source.browsers = {BrowserKind::Chrome};
            source.key = machine(kForceList);
            source.value_name = value.name;
            source.value = value;
            source.extension_id = text_of(value);
            source.list = raw;
            add_source(std::move(source));
        }
    }

    void settings(std::wstring_view json, std::vector<std::wstring> ids,
                  BrowserKind browser = BrowserKind::Chrome, DWORD type = REG_SZ) {
        const RawValue value = string_value(L"ExtensionSettings", json, type);
        for (auto& id : ids) {
            ExtensionSource source;
            source.kind = ExtensionSourceKind::Settings;
            source.browsers = {browser};
            source.key = machine(browser == BrowserKind::Firefox ? LR"(SOFTWARE\Policies\Mozilla\Firefox)"
                                                                 : kChromePolicy);
            source.value_name = L"ExtensionSettings";
            source.value = value;
            source.extension_id = std::move(id);
            add_source(std::move(source));
        }
    }

    std::size_t add_source(ExtensionSource source) {
        list.sources.push_back(std::move(source));
        return list.sources.size() - 1;
    }

    // An entry for every source naming `extension_id`, with id `id`.
    BrowserExtension& entry(const std::wstring& id, const std::wstring& extension_id,
                            ExtensionRemoval removal = ExtensionRemoval::Removable) {
        BrowserExtension entry;
        entry.id = id;
        entry.extension_id = extension_id;
        entry.name = L"Name of " + id;
        for (std::size_t s = 0; s < list.sources.size(); ++s) {
            if (list.sources[s].extension_id == extension_id) {
                entry.sources.push_back(s);
            }
        }
        entry.removal = entry.sources.empty() ? ExtensionRemoval::NotRemovable : removal;
        list.extensions.push_back(std::move(entry));
        return list.extensions.back();
    }

    BrowserExtensionsList list;
};

ExtensionsPlan plan(const BrowserExtensionsList& list, std::vector<std::wstring> ids,
                    const std::function<ProtectionReason(const Location&)>& protection = {}) {
    return plan_remove_extensions(list, ids, protection);
}

// "name: before -> after" of each value edit, in plan order.
std::vector<std::wstring> edits(const ExtensionsPlan& planned) {
    std::vector<std::wstring> lines;
    for (const auto& item : planned.plan.items) {
        const auto* edit = std::get_if<ValueEditLocation>(&item.location);
        if (!edit) {
            lines.push_back(L"<" + leftovers::identity(item.location) + L">");
            continue;
        }
        EXPECT_EQ(edit->kind, ValueEditKind::RestoreValue);
        lines.push_back(edit->value_name + L": " + text_of(edit->expected) + L" -> " +
                        text_of(edit->desired));
    }
    return lines;
}

TEST(ExtensionsPlanTest, AListLosesTheEntryAndIsRenumberedWithoutAGap) {
    Fixture fixture;
    fixture.force_list({{L"1", L"A"}, {L"2", L"B"}, {L"3", L"C"}});
    fixture.entry(L"id-a", L"A");
    fixture.entry(L"id-b", L"B");
    fixture.entry(L"id-c", L"C");

    const auto middle = plan(fixture.list, {L"id-b"});
    EXPECT_TRUE(middle.issues.empty());
    EXPECT_EQ(middle.planned, std::vector<std::wstring>{L"id-b"});
    EXPECT_EQ(edits(middle), (std::vector<std::wstring>{L"2: B -> C", L"3: C -> <absent>"}));
    for (const auto& item : middle.plan.items) {
        EXPECT_EQ(item.programs, std::vector<std::wstring>{L"Name of id-b"});
        EXPECT_EQ(item.category, leftovers::Category::Registry);
    }

    // Two from one list: one pass over the list.
    EXPECT_EQ(edits(plan(fixture.list, {L"ID-A", L"id-c", L"id-a"})),
              (std::vector<std::wstring>{L"1: A -> B", L"2: B -> <absent>", L"3: C -> <absent>"}));
    // The last one: only deleted.
    EXPECT_EQ(edits(plan(fixture.list, {L"id-c"})), std::vector<std::wstring>{L"3: C -> <absent>"});
    // Every one.
    EXPECT_EQ(edits(plan(fixture.list, {L"id-a", L"id-b", L"id-c"})),
              (std::vector<std::wstring>{L"1: A -> <absent>", L"2: B -> <absent>", L"3: C -> <absent>"}));
}

TEST(ExtensionsPlanTest, ValuesPastAGapStayWhereTheyAre) {
    // Chromium reads "1", "2" and stops at the gap: "4" is inert, and must stay so.
    Fixture fixture;
    fixture.force_list({{L"1", L"A"}, {L"2", L"B"}, {L"4", L"D"}});
    fixture.entry(L"id-a", L"A");
    fixture.entry(L"id-d", L"D");
    EXPECT_EQ(edits(plan(fixture.list, {L"id-a"})),
              (std::vector<std::wstring>{L"1: A -> B", L"2: B -> <absent>"}));
    EXPECT_EQ(edits(plan(fixture.list, {L"id-d"})), std::vector<std::wstring>{L"4: D -> <absent>"});
}

TEST(ExtensionsPlanTest, AJsonPolicyLosesOnlyTheExtensionsKey) {
    Fixture fixture;
    fixture.settings(
        LR"({"a, b":{"installation_mode":"force_installed","update_url":"u"},)"
        LR"("c":{"installation_mode":"normal_installed"},"*":{"installation_mode":"blocked"}})",
        {L"a", L"b", L"c"});
    fixture.entry(L"id-a", L"a");
    fixture.entry(L"id-b", L"b");
    fixture.entry(L"id-c", L"c");
    EXPECT_EQ(edits(plan(fixture.list, {L"id-b"})),
              std::vector<std::wstring>{
                  LR"(ExtensionSettings: {"a, b":{"installation_mode":"force_installed","update_url":"u"},)"
                  LR"("c":{"installation_mode":"normal_installed"},"*":{"installation_mode":"blocked"}} -> )"
                  LR"({"a":{"installation_mode":"force_installed","update_url":"u"},)"
                  LR"("c":{"installation_mode":"normal_installed"},"*":{"installation_mode":"blocked"}})"});
    // All three: the default for every extension stays.
    const auto all = plan(fixture.list, {L"id-a", L"id-b", L"id-c"});
    ASSERT_EQ(all.plan.items.size(), 1u);
    const auto& edit = std::get<ValueEditLocation>(all.plan.items[0].location);
    EXPECT_EQ(text_of(edit.desired), LR"({"*":{"installation_mode":"blocked"}})");
    EXPECT_EQ(edit.desired->type, static_cast<DWORD>(REG_SZ));
}

TEST(ExtensionsPlanTest, AJsonPolicyLeftEmptyIsDeletedAndKeepsItsType) {
    Fixture fixture;
    fixture.settings(LR"({"x@example.com":{"installation_mode":"force_installed"}})", {L"x@example.com"},
                     BrowserKind::Firefox, REG_MULTI_SZ);
    fixture.entry(L"id-x", L"x@example.com");
    EXPECT_EQ(
        edits(plan(fixture.list, {L"id-x"})),
        std::vector<std::wstring>{
            LR"(ExtensionSettings: {"x@example.com":{"installation_mode":"force_installed"}} -> <absent>)"});

    Fixture two;
    two.settings(
        LR"({"x@example.com":{"installation_mode":"force_installed"},)"
        LR"("y@example.com":{"installation_mode":"force_installed"}})",
        {L"x@example.com", L"y@example.com"}, BrowserKind::Firefox, REG_MULTI_SZ);
    two.entry(L"id-x", L"x@example.com");
    const auto planned = plan(two.list, {L"id-x"});
    ASSERT_EQ(planned.plan.items.size(), 1u);
    const auto& desired = std::get<ValueEditLocation>(planned.plan.items[0].location).desired;
    ASSERT_TRUE(desired);
    EXPECT_EQ(desired->type, static_cast<DWORD>(REG_MULTI_SZ));
    const std::wstring expected = LR"({"y@example.com":{"installation_mode":"force_installed"}})";
    EXPECT_EQ(desired->data.size(), (expected.size() + 2) * sizeof(wchar_t));  // Two terminators
    EXPECT_EQ(text_of(desired), expected);
}

TEST(ExtensionsPlanTest, RegistrationsGoWholeAndSharedSourcesOnce) {
    Fixture fixture;
    ExtensionSource registration;
    registration.kind = ExtensionSourceKind::ExternalRegistry;
    registration.browsers = {BrowserKind::Chrome};
    registration.key = machine(std::wstring(LR"(SOFTWARE\WOW6432Node\Google\Chrome\Extensions\)") + kA);
    registration.extension_id = kA;
    fixture.add_source(registration);
    ExtensionSource firefox;
    firefox.kind = ExtensionSourceKind::FirefoxRegistry;
    firefox.browsers = {BrowserKind::Firefox};
    firefox.key = machine(LR"(SOFTWARE\Mozilla\Firefox\Extensions)");
    firefox.value_name = L"x@example.com";
    firefox.extension_id = L"x@example.com";
    fixture.add_source(firefox);
    // One machine-wide registration, two users' profiles.
    fixture.entry(L"id-1", kA).user_sid = L"S-1-5-21-1";
    fixture.entry(L"id-2", kA).user_sid = L"S-1-5-21-2";
    fixture.entry(L"id-x", L"x@example.com");
    fixture.entry(L"id-store", kB);  // No source

    const auto one = plan(fixture.list, {L"id-1"});
    ASSERT_EQ(one.plan.items.size(), 1u);
    EXPECT_EQ(std::get<RegistryKeyLocation>(one.plan.items[0].location).key.path, registration.key.path);
    EXPECT_EQ(one.planned, std::vector<std::wstring>{L"id-1"});
    EXPECT_EQ(one.affected, (std::vector<std::wstring>{L"id-1", L"id-2"}));

    // By extension id: both entries, the key once.
    const auto both = plan(fixture.list, {kA, L"id-x"});
    EXPECT_TRUE(both.issues.empty());
    EXPECT_EQ(both.planned, (std::vector<std::wstring>{L"id-1", L"id-2", L"id-x"}));
    ASSERT_EQ(both.plan.items.size(), 2u);
    const auto& value = std::get<RegistryValueLocation>(both.plan.items[1].location);
    EXPECT_EQ(value.value_name, L"x@example.com");
}

// The user's own copy stays when only a registration goes: noted for every affected entry it holds for.
TEST(ExtensionsPlanTest, EntriesKeepingTheUsersCopyAreNoted) {
    Fixture fixture;
    ExtensionSource registration;
    registration.kind = ExtensionSourceKind::ExternalRegistry;
    registration.browsers = {BrowserKind::Chrome};
    registration.key = machine(std::wstring(LR"(SOFTWARE\WOW6432Node\Google\Chrome\Extensions\)") + kA);
    registration.extension_id = kA;
    fixture.add_source(registration);
    fixture.entry(L"id-registry", kA).install = ExtensionInstall::ExternalRegistry;
    auto& store = fixture.entry(L"id-store", kA);
    store.install = ExtensionInstall::Store;
    store.keeps_user_copy = true;

    const auto planned = plan(fixture.list, {L"id-registry"});
    EXPECT_TRUE(planned.issues.empty());
    EXPECT_EQ(planned.affected, (std::vector<std::wstring>{L"id-registry", L"id-store"}));
    ASSERT_EQ(planned.notes.size(), 1u);
    EXPECT_EQ(planned.notes[0].id, L"id-store");
    EXPECT_EQ(planned.notes[0].code, ExtensionPlanNoteCode::KeepsUserCopy);
    EXPECT_EQ(to_string(planned.notes[0].code), "keeps-user-copy");
    // Still removable when asked for by itself.
    const auto own = plan(fixture.list, {L"id-store"});
    EXPECT_TRUE(own.issues.empty());
    EXPECT_EQ(own.plan.items.size(), 1u);
    ASSERT_EQ(own.notes.size(), 1u);
    EXPECT_EQ(own.notes[0].id, L"id-store");
}

TEST(ExtensionsPlanTest, RefusalsPlanNothingForTheirIds) {
    Fixture fixture;
    fixture.force_list({{L"1", L"A"}});
    ExtensionSource json;
    json.kind = ExtensionSourceKind::PoliciesJson;
    json.browsers = {BrowserKind::Firefox};
    json.file = LR"(C:\Program Files\Mozilla Firefox\distribution\policies.json)";
    json.extension_id = L"j@example.com";
    fixture.add_source(json);
    ExtensionSource subkey;
    subkey.kind = ExtensionSourceKind::Settings;
    subkey.browsers = {BrowserKind::Chrome};
    subkey.key = machine(std::wstring(kChromePolicy) + L"\\ExtensionSettings\\" + kC + L"," + kD);
    subkey.settings_subkey = true;
    subkey.settings_key = std::wstring(kC) + L"," + kD;
    subkey.extension_id = kC;
    fixture.add_source(subkey);
    fixture.entry(L"id-a", L"A");
    fixture.entry(L"id-store", kB);
    fixture.entry(L"id-json", L"j@example.com", ExtensionRemoval::ManagedByPoliciesJson);
    fixture.entry(L"id-sub", kC, ExtensionRemoval::NotSupported);

    const auto refused = plan(fixture.list, {L"nothing", L"id-store", L"id-json", L"id-sub", L"id-a"});
    ASSERT_EQ(refused.issues.size(), 4u);
    EXPECT_EQ(refused.issues[0].code, Code::UnknownId);
    EXPECT_EQ(refused.issues[0].id, L"nothing");
    EXPECT_EQ(refused.issues[1].code, Code::NotRemovable);
    EXPECT_EQ(refused.issues[2].code, Code::ManagedByPoliciesJson);
    EXPECT_EQ(refused.issues[3].code, Code::NotSupported);
    EXPECT_EQ(refused.planned, std::vector<std::wstring>{L"id-a"});  // The caller refuses the whole request

    // A protection layer that refuses the policy.
    const auto guarded = plan(fixture.list, {L"id-a"}, [](const Location& location) {
        const auto* edit = std::get_if<ValueEditLocation>(&location);
        return edit && edit->value_name == L"1" ? ProtectionReason::NotEditable : ProtectionReason::None;
    });
    ASSERT_EQ(guarded.issues.size(), 1u);
    EXPECT_EQ(guarded.issues[0].code, Code::Protected);
    EXPECT_TRUE(guarded.plan.items.empty());
}

// The machine's Chrome policies, an external registration and a Firefox registration in a scratch key
// standing in for HKLM: listed, removed for real, and undone exactly.
TEST(ExtensionsChangesTest, RemovalEditsThePoliciesAndUndoPutsThemBack) {
    testing::ScratchKey hklm;
    testing::TempFolder temp;
    hklm.set_string(kForceList, L"1", std::wstring(kA) + L";https://example.com/a");
    hklm.set_string(kForceList, L"2", kB);
    hklm.set_string(kForceList, L"3", kC);
    hklm.set_string(kChromePolicy, L"ExtensionSettings",
                    std::wstring(LR"({")") + kB + LR"(":{"installation_mode":"force_installed"},")" + kD +
                        LR"(":{"installation_mode":"normal_installed"}})");
    hklm.set_string(std::wstring(LR"(SOFTWARE\WOW6432Node\Google\Chrome\Extensions\)") + kB, L"update_url",
                    L"https://clients2.google.com/service/update2/crx");
    hklm.set_string(LR"(SOFTWARE\Mozilla\Firefox\Extensions)", L"x@example.com", LR"(C:\Contoso\x.xpi)");
    const std::string before = testing::dump_key(hklm.get(), L"SOFTWARE");

    BrowserExtensionsEnvironment environment;
    environment.scan.machine = hklm.get();
    environment.firefox_folders = std::vector<std::wstring>{};
    environment.managed = false;
    const auto list = list_browser_extensions(environment);
    ASSERT_EQ(list.extensions.size(), 5u);  // A, B, C, D in no profile yet; x@example.com
    for (const auto& entry : list.extensions) {
        EXPECT_FALSE(entry.in_profile);
        EXPECT_EQ(entry.removal, ExtensionRemoval::Removable) << platform::to_utf8(entry.extension_id);
    }

    const auto protection = [](const Location& location) {
        return leftovers::HardFloor::live().check(location);
    };
    auto planned = plan_remove_extensions(list, std::vector<std::wstring>{kB, L"x@example.com"}, protection);
    ASSERT_TRUE(planned.issues.empty());
    planned.plan.description = L"Remove 2 browser extensions";
    ASSERT_EQ(planned.plan.items.size(), 5u);  // "2" and "3" of the list, the JSON value, the key, the value

    GUID guid{};
    ::CoCreateGuid(&guid);
    changes::StoreOptions store_options;
    store_options.root = temp / L"Store";
    store_options.mutex_name = std::format(L"Local\\Avel.Tests.{:08x}", guid.Data1);
    const std::wstring user = testing::current_user_sid_text();
    store_options.policy = {L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + user + L")",
                            {user, L"BA", L"SY"}};
    platform::FolderStatus status{};
    const auto store = changes::ChangeStore::open(store_options, status);
    ASSERT_TRUE(store);
    changes::ExecuteOptions options;
    options.registry_root = [&](Hive hive, const std::wstring&) {
        return hive == Hive::Machine ? hklm.get() : nullptr;
    };
    options.protection = protection;  // The real floor, with its allow-list for extension policies

    const auto result = changes::execute(*store, planned.plan, options);
    ASSERT_FALSE(result.set_id.empty());
    for (std::size_t i = 0; i < result.items.size(); ++i) {
        EXPECT_EQ(result.items[i].status.state, changes::ItemState::Done) << i;
    }
    const auto value = [&](const std::wstring& key, const std::wstring& name) -> std::optional<RawValue> {
        const auto opened = hklm.create(key);
        RawValue read;
        return platform::query_raw_value(opened.get(), name, read) == ERROR_SUCCESS ? std::optional(read)
                                                                                    : std::nullopt;
    };
    EXPECT_EQ(text_of(value(kForceList, L"1")), std::wstring(kA) + L";https://example.com/a");
    EXPECT_EQ(text_of(value(kForceList, L"2")), kC);
    EXPECT_FALSE(value(kForceList, L"3"));
    EXPECT_EQ(text_of(value(kChromePolicy, L"ExtensionSettings")),
              std::wstring(LR"({")") + kD + LR"(":{"installation_mode":"normal_installed"}})");
    EXPECT_FALSE(value(LR"(SOFTWARE\Mozilla\Firefox\Extensions)", L"x@example.com"));
    HKEY raw = nullptr;
    EXPECT_EQ(
        ::RegOpenKeyExW(hklm.get(),
                        (std::wstring(LR"(SOFTWARE\WOW6432Node\Google\Chrome\Extensions\)") + kB).c_str(), 0,
                        KEY_READ, &raw),
        ERROR_FILE_NOT_FOUND);

    const auto undone = changes::restore(*store, result.set_id, {}, options);
    ASSERT_TRUE(undone.found);
    EXPECT_EQ(undone.state, changes::SetState::Restored);
    EXPECT_EQ(testing::dump_key(hklm.get(), L"SOFTWARE"), before);
}

// A list that changed after it was listed: the stale edits are skipped, nothing is overwritten.
TEST(ExtensionsChangesTest, AListChangedSinceListingIsLeftAlone) {
    testing::ScratchKey hklm;
    testing::TempFolder temp;
    hklm.set_string(kForceList, L"1", kA);
    hklm.set_string(kForceList, L"2", kB);
    BrowserExtensionsEnvironment environment;
    environment.scan.machine = hklm.get();
    environment.firefox_folders = std::vector<std::wstring>{};
    environment.managed = false;
    const auto list = list_browser_extensions(environment);
    auto planned = plan_remove_extensions(list, std::vector<std::wstring>{kA});
    ASSERT_EQ(planned.plan.items.size(), 2u);
    planned.plan.description = L"Remove browser extension";
    hklm.set_string(kForceList, L"2", kC);  // An administrator changed the list meanwhile

    GUID guid{};
    ::CoCreateGuid(&guid);
    changes::StoreOptions store_options;
    store_options.root = temp / L"Store";
    store_options.mutex_name = std::format(L"Local\\Avel.Tests.{:08x}", guid.Data1);
    const std::wstring user = testing::current_user_sid_text();
    store_options.policy = {L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + user + L")",
                            {user, L"BA", L"SY"}};
    platform::FolderStatus status{};
    const auto store = changes::ChangeStore::open(store_options, status);
    ASSERT_TRUE(store);
    changes::ExecuteOptions options;
    options.registry_root = [&](Hive hive, const std::wstring&) {
        return hive == Hive::Machine ? hklm.get() : nullptr;
    };
    options.protection = [](const Location& location) {
        return leftovers::HardFloor::live().check(location);
    };
    const auto result = changes::execute(*store, planned.plan, options);
    ASSERT_EQ(result.items.size(), 2u);
    // "1" still held A as planned, so it took "2"'s planned B; "2" changed, and stays as it is now.
    EXPECT_EQ(result.items[0].status.state, changes::ItemState::Done);
    EXPECT_EQ(result.items[1].status.state, changes::ItemState::Skipped);
    EXPECT_EQ(result.items[1].status.reason, changes::Reason::Changed);
    changes::restore(*store, result.set_id, {}, options);
    const auto opened = hklm.create(kForceList);
    RawValue read;
    ASSERT_EQ(platform::query_raw_value(opened.get(), L"1", read), ERROR_SUCCESS);
    EXPECT_EQ(text_of(read), kA);
    ASSERT_EQ(platform::query_raw_value(opened.get(), L"2", read), ERROR_SUCCESS);
    EXPECT_EQ(text_of(read), kC);
}

}  // namespace
}  // namespace avel::tools
