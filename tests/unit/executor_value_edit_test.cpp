// Value edits through the executor, in volatile scratch keys standing in for HKLM and a user's
// hive (never the real environment): compare-and-set, the skip when the value changed since
// planning, exact undo, undo by the inverse operation, recovery after a crash at any step, and the
// WM_SETTINGCHANGE hook.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <cstring>
#include <format>
#include <map>
#include <memory>

#include "core/changes/executor.h"
#include "core/leftovers/value_edits.h"
#include "core/platform/nt_registry.h"
#include "file_tree.h"
#include "registry_dump.h"
#include "scratch_registry.h"

namespace avel::changes {
namespace {

using leftovers::Category;
using leftovers::Confidence;
using leftovers::TracedValue;
using leftovers::ValueEditKind;
using leftovers::ValueEditLocation;
using leftovers::ValueSpot;
using platform::Hive;
using platform::RawValue;
using platform::RegistryAddress;

constexpr wchar_t kSid[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kEnvironment[] = LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)";
constexpr wchar_t kSharedDlls[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)";
constexpr wchar_t kSystemPath[] = LR"(%SystemRoot%\system32;%SystemRoot%)";

RawValue string_value(const std::wstring& name, std::wstring_view text, DWORD type = REG_SZ) {
    RawValue value{name, type, std::vector<std::byte>((text.size() + 1) * sizeof(wchar_t))};
    std::memcpy(value.data.data(), text.data(), text.size() * sizeof(wchar_t));
    return value;
}

RawValue count_value(const std::wstring& name, std::uint32_t count) {
    RawValue value{name, REG_DWORD, std::vector<std::byte>(4)};
    std::memcpy(value.data.data(), &count, 4);
    return value;
}

// The values a trace saved and what the "installer" did with them, in a fresh copy for each run.
class World {
public:
    World() {
        // Before the install.
        set(machine_, kEnvironment, string_value(L"Path", kSystemPath, REG_EXPAND_SZ));
        set(machine_, kEnvironment, string_value(L"CONTOSO_THEME", L"dark"));
        set(machine_, kSharedDlls, count_value(LR"(C:\Shared\common.dll)", 1));
        set(user_, L"Environment", string_value(L"Path", LR"(%USERPROFILE%\bin)", REG_EXPAND_SZ));
        set(user_, L"Environment", string_value(L"Unrelated", L"stays"));
        before_ = {read(machine_, kEnvironment, L"Path"), read(machine_, kEnvironment, L"CONTOSO_THEME"),
                   read(machine_, kSharedDlls, LR"(C:\Shared\common.dll)"),
                   read(user_, L"Environment", L"Path")};

        // The install: it appends to both Paths, creates a variable, changes another, raises a
        // shared DLL's count and creates its own.
        set(machine_, kEnvironment,
            string_value(L"Path", kSystemPath + std::wstring(LR"(;C:\Contoso\bin)"), REG_EXPAND_SZ));
        set(machine_, kEnvironment, string_value(L"CONTOSO_HOME", LR"(C:\Contoso)"));
        set(machine_, kEnvironment, string_value(L"CONTOSO_THEME", L"light"));
        set(machine_, kSharedDlls, count_value(LR"(C:\Shared\common.dll)", 2));
        set(machine_, kSharedDlls, count_value(LR"(C:\Shared\contoso.dll)", 1));
        set(user_, L"Environment",
            string_value(L"Path", LR"(%USERPROFILE%\bin;C:\Contoso\user)", REG_EXPAND_SZ));
        changes_ = {
            change(ValueSpot::MachineEnvironment, machine_address(kEnvironment), L"Path", before_[0]),
            change(ValueSpot::MachineEnvironment, machine_address(kEnvironment), L"CONTOSO_HOME",
                   std::nullopt),
            change(ValueSpot::MachineEnvironment, machine_address(kEnvironment), L"CONTOSO_THEME",
                   before_[1]),
            change(ValueSpot::SharedDlls, machine_address(kSharedDlls), LR"(C:\Shared\common.dll)",
                   before_[2]),
            change(ValueSpot::SharedDlls, machine_address(kSharedDlls), LR"(C:\Shared\contoso.dll)",
                   std::nullopt),
            change(ValueSpot::UserEnvironment, {Hive::User, kSid, L"Environment"}, L"Path", before_[3]),
        };

        StoreOptions options;
        GUID guid{};
        ::CoCreateGuid(&guid);
        options.root = temp_ / L"Store";
        options.mutex_name = std::format(L"Local\\Avel.Tests.{:08x}", guid.Data1);
        const std::wstring user = testing::current_user_sid_text();
        options.policy = {L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + user + L")",
                          {user, L"BA", L"SY"}};
        platform::FolderStatus status{};
        store_ = ChangeStore::open(options, status);
        EXPECT_TRUE(store_);
    }

    ChangeStore& store() { return *store_; }
    int broadcasts() const { return *broadcasts_; }

    ExecuteOptions options() const {
        ExecuteOptions options;
        options.registry_root = [this](Hive hive, const std::wstring&) {
            return hive == Hive::Machine ? machine_.get() : user_.get();
        };
        options.protection = [](const leftovers::Location& location) {
            return leftovers::HardFloor::live().check(location);  // The real floor, with its allow-list
        };
        options.environment_changed = [count = broadcasts_] { ++*count; };
        return options;
    }

    // The plan for the values as they are now, as the trace scanner will make it.
    ChangePlan plan() const {
        ChangePlan plan;
        plan.description = L"Contoso Fixture by its trace";
        for (const auto& traced : changes_) {
            const auto planned = leftovers::plan_value_edit(traced, live(traced));
            EXPECT_TRUE(planned.edit) << platform::to_utf8(traced.name);
            if (planned.edit) {
                const bool restore = planned.basis == leftovers::ValueEditBasis::Restore;
                const Confidence confidence = restore ? Confidence::Questionable : Confidence::VeryGood;
                plan.items.push_back({*planned.edit, Category::Registry, {L"Contoso Fixture"}, confidence,
                                      restore ? 0 : 5});
            }
        }
        return plan;
    }

    std::optional<RawValue> live(const TracedValue& traced) const {
        return read(traced.key.hive == Hive::Machine ? machine_ : user_, traced.key.path, traced.name);
    }
    std::optional<RawValue> machine_value(const wchar_t* key, const std::wstring& name) const {
        return read(machine_, key, name);
    }
    std::optional<RawValue> user_value(const std::wstring& name) const {
        return read(user_, L"Environment", name);
    }
    void set_machine(const wchar_t* key, const RawValue& value) const { set(machine_, key, value); }
    void set_user(const RawValue& value) const { set(user_, L"Environment", value); }
    void delete_machine(const wchar_t* key, const std::wstring& name) const {
        const auto opened = machine_.create(key);
        EXPECT_EQ(::RegDeleteValueW(opened.get(), name.c_str()), ERROR_SUCCESS);
    }

    std::string dump() const {
        return testing::dump_key(machine_.get(), L"SYSTEM") + testing::dump_key(machine_.get(), L"SOFTWARE") +
               testing::dump_key(user_.get(), L"Environment");
    }

private:
    static RegistryAddress machine_address(const wchar_t* path) { return {Hive::Machine, L"", path}; }

    TracedValue change(ValueSpot spot, RegistryAddress key, std::wstring name,
                       std::optional<RawValue> before) const {
        TracedValue traced{spot, key, name, std::move(before), std::nullopt};
        traced.after = live(traced);
        return traced;
    }

    static void set(const testing::ScratchKey& root, std::wstring_view path, const RawValue& value) {
        const auto key = root.create(path);
        EXPECT_EQ(platform::set_raw_value(key.get(), value), ERROR_SUCCESS);
    }

    static std::optional<RawValue> read(const testing::ScratchKey& root, std::wstring_view path,
                                        const std::wstring& name) {
        const auto key = root.create(path);
        RawValue value;
        if (platform::query_raw_value(key.get(), name, value) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        return value;
    }

    testing::TempFolder temp_;
    testing::ScratchKey machine_;
    testing::ScratchKey user_;
    std::vector<std::optional<RawValue>> before_;
    std::vector<TracedValue> changes_;
    std::shared_ptr<int> broadcasts_ = std::make_shared<int>(0);
    std::unique_ptr<ChangeStore> store_;
};

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

std::optional<std::uint32_t> count_of(const std::optional<RawValue>& value) {
    return value ? leftovers::shared_dll_count(*value) : std::nullopt;
}

TEST(ExecutorValueEditTest, EditsAndUndoesThemExactly) {
    World world;
    const std::string installed = world.dump();
    const ChangePlan plan = world.plan();
    ASSERT_EQ(plan.items.size(), 6u);
    const auto result = execute(world.store(), plan, world.options());
    ASSERT_FALSE(result.set_id.empty());
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        EXPECT_EQ(result.items[i].status.state, ItemState::Done) << i;
    }
    EXPECT_EQ(world.broadcasts(), 1);  // Once for the whole run

    // As before the install, for everything the installer did.
    const auto path = world.machine_value(kEnvironment, L"Path");
    ASSERT_TRUE(path);
    EXPECT_EQ(path->type, static_cast<DWORD>(REG_EXPAND_SZ));
    EXPECT_EQ(text_of(path), kSystemPath);
    EXPECT_EQ(path->data.size(), (std::wstring_view(kSystemPath).size() + 1) * sizeof(wchar_t));
    EXPECT_FALSE(world.machine_value(kEnvironment, L"CONTOSO_HOME"));
    EXPECT_EQ(text_of(world.machine_value(kEnvironment, L"CONTOSO_THEME")), L"dark");
    EXPECT_EQ(count_of(world.machine_value(kSharedDlls, LR"(C:\Shared\common.dll)")), 1u);
    EXPECT_FALSE(world.machine_value(kSharedDlls, LR"(C:\Shared\contoso.dll)"));
    EXPECT_EQ(text_of(world.user_value(L"Path")), LR"(%USERPROFILE%\bin)");
    EXPECT_EQ(text_of(world.user_value(L"Unrelated")), L"stays");

    const auto undone = restore(world.store(), result.set_id, {}, world.options());
    ASSERT_TRUE(undone.found);
    EXPECT_EQ(undone.items.size(), 6u);
    EXPECT_EQ(undone.state, SetState::Restored);
    EXPECT_EQ(world.dump(), installed);
    EXPECT_EQ(world.broadcasts(), 2);
}

TEST(ExecutorValueEditTest, AValueThatChangedSincePlanningIsSkipped) {
    World world;
    const ChangePlan plan = world.plan();
    // The user adds an entry to the machine Path, and another program raises a count, after the
    // plan was made: those two edits must not go ahead.
    world.set_machine(
        kEnvironment,
        string_value(L"Path", kSystemPath + std::wstring(LR"(;C:\Contoso\bin;D:\Tools)"), REG_EXPAND_SZ));
    world.set_machine(kSharedDlls, count_value(LR"(C:\Shared\common.dll)", 3));
    const std::string changed = world.dump();
    const auto result = execute(world.store(), plan, world.options());
    EXPECT_EQ(result.items[0].status.state, ItemState::Skipped);
    EXPECT_EQ(result.items[0].status.reason, Reason::Changed);
    EXPECT_EQ(result.items[3].status.state, ItemState::Skipped);
    EXPECT_EQ(result.items[3].status.reason, Reason::Changed);
    EXPECT_EQ(text_of(world.machine_value(kEnvironment, L"Path")),
              kSystemPath + std::wstring(LR"(;C:\Contoso\bin;D:\Tools)"));
    EXPECT_EQ(count_of(world.machine_value(kSharedDlls, LR"(C:\Shared\common.dll)")), 3u);
    EXPECT_EQ(result.items[1].status.state, ItemState::Done);

    // Undo leaves the skipped ones alone too.
    restore(world.store(), result.set_id, {}, world.options());
    EXPECT_EQ(world.dump(), changed);
}

TEST(ExecutorValueEditTest, UndoAppliesTheInverseWhenTheValueChangedSince) {
    World world;
    const auto result = execute(world.store(), world.plan(), world.options());
    ASSERT_FALSE(result.set_id.empty());
    // Since the removal: the user appended to both Paths and set the theme again, another program
    // raised the shared count, and a reinstall created CONTOSO_HOME again.
    world.set_machine(kEnvironment,
                      string_value(L"Path", kSystemPath + std::wstring(LR"(;D:\Tools)"), REG_SZ));
    world.set_user(string_value(L"Path", LR"(%USERPROFILE%\bin;D:\Mine)", REG_EXPAND_SZ));
    world.set_machine(kEnvironment, string_value(L"CONTOSO_THEME", L"blue"));
    world.set_machine(kSharedDlls, count_value(LR"(C:\Shared\common.dll)", 4));
    world.set_machine(kEnvironment, string_value(L"CONTOSO_HOME", LR"(D:\Contoso)"));
    // ... and the count the removal deleted was created again by another installer.
    world.set_machine(kSharedDlls, count_value(LR"(C:\Shared\contoso.dll)", 1));

    const auto undone = restore(world.store(), result.set_id, {}, world.options());
    std::map<std::size_t, ItemStatus> statuses;
    for (const auto& [index, item] : undone.items) {
        statuses[index] = item.status;
    }
    EXPECT_EQ(statuses[0].state, ItemState::Restored);  // Path: the entry back after its neighbour
    const auto path = world.machine_value(kEnvironment, L"Path");
    EXPECT_EQ(text_of(path), kSystemPath + std::wstring(LR"(;C:\Contoso\bin;D:\Tools)"));
    EXPECT_EQ(path->type, static_cast<DWORD>(REG_SZ));        // The type the user's tool left
    EXPECT_EQ(statuses[1].state, ItemState::RestoreSkipped);  // CONTOSO_HOME: a new one is there
    EXPECT_EQ(statuses[1].reason, Reason::Exists);
    EXPECT_EQ(text_of(world.machine_value(kEnvironment, L"CONTOSO_HOME")), LR"(D:\Contoso)");
    EXPECT_EQ(statuses[2].state, ItemState::RestoreSkipped);  // The theme: the user's choice stays
    EXPECT_EQ(statuses[2].reason, Reason::Changed);
    EXPECT_EQ(text_of(world.machine_value(kEnvironment, L"CONTOSO_THEME")), L"blue");
    EXPECT_EQ(statuses[3].state, ItemState::Restored);  // The count: raised by what was taken off
    EXPECT_EQ(count_of(world.machine_value(kSharedDlls, LR"(C:\Shared\common.dll)")), 5u);
    EXPECT_EQ(statuses[4].state, ItemState::Restored);  // Deleted count: added to the new one
    EXPECT_EQ(count_of(world.machine_value(kSharedDlls, LR"(C:\Shared\contoso.dll)")), 2u);
    EXPECT_EQ(statuses[5].state, ItemState::Restored);
    EXPECT_EQ(text_of(world.user_value(L"Path")), LR"(%USERPROFILE%\bin;C:\Contoso\user;D:\Mine)");
    EXPECT_EQ(undone.state, SetState::PartlyRestored);

    // Retrying the skipped ones changes nothing: their values still aren't what the edits wrote.
    const std::string after = world.dump();
    restore(world.store(), result.set_id, {1, 2}, world.options());
    EXPECT_EQ(world.dump(), after);
}

TEST(ExecutorValueEditTest, UndoSkipsAnInverseThatDoesntApply) {
    World world;
    const auto result = execute(world.store(), world.plan(), world.options());
    // Meanwhile the shared count was deleted, and the user put the Contoso entry back into their
    // Path themselves (spelled their way).
    world.delete_machine(kSharedDlls, LR"(C:\Shared\common.dll)");
    world.set_user(string_value(L"Path", LR"(%USERPROFILE%\bin;c:\contoso\USER\)", REG_EXPAND_SZ));
    const std::string before_undo = world.dump();
    const auto undone = restore(world.store(), result.set_id, {3, 5}, world.options());
    ASSERT_EQ(undone.items.size(), 2u);
    for (const auto& [index, item] : undone.items) {
        EXPECT_EQ(item.status.state, ItemState::RestoreSkipped) << index;
        EXPECT_EQ(item.status.reason, index == 5 ? Reason::Exists : Reason::Changed) << index;
    }
    EXPECT_EQ(world.dump(), before_undo);
}

TEST(ExecutorValueEditTest, OnlyEnvironmentEditsAreBroadcast) {
    World world;
    ChangePlan plan = world.plan();
    std::erase_if(plan.items, [](const PlannedItem& item) {
        return leftovers::value_spot(std::get<ValueEditLocation>(item.location).key) != ValueSpot::SharedDlls;
    });
    ASSERT_EQ(plan.items.size(), 2u);
    const auto result = execute(world.store(), plan, world.options());
    EXPECT_EQ(result.items[0].status.state, ItemState::Done);
    EXPECT_EQ(world.broadcasts(), 0);
    restore(world.store(), result.set_id, {}, world.options());
    EXPECT_EQ(world.broadcasts(), 0);
}

TEST(ExecutorValueEditTest, TheFloorRefusesEditsOutsideItsAllowList) {
    World world;
    ChangePlan plan;
    plan.description = L"Not allowed";
    const auto edit = [](RegistryAddress key, std::wstring name, ValueEditKind kind) {
        ValueEditLocation location{std::move(key), name, kind, string_value(name, L"x"), std::nullopt, {}, 0};
        return PlannedItem{location, Category::Registry, {}, Confidence::VeryGood, 5};
    };
    plan.items = {
        edit({Hive::Machine, L"", LR"(SOFTWARE\Contoso)"}, L"Path", ValueEditKind::DeleteValue),
        edit({Hive::Machine, L"", kEnvironment}, L"ComSpec", ValueEditKind::RestoreValue),
        edit({Hive::Machine, L"", kEnvironment}, L"Path", ValueEditKind::DeleteValue),
        edit({Hive::Machine, L"", kSharedDlls}, LR"(C:\Shared\common.dll)", ValueEditKind::RestoreValue),
    };
    const std::string before = world.dump();
    const auto result = execute(world.store(), plan, world.options());
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        EXPECT_EQ(result.items[i].status.reason, Reason::Protected) << i;
    }
    EXPECT_EQ(world.dump(), before);
}

TEST(ExecutorValueEditTest, ACrashAtAnyStepRecoversAndUndoesToTheInstalledState) {
    // Every edit records backed-up and done.
    const std::size_t records = 12;
    for (std::size_t step = 1; step < records; ++step) {
        SCOPED_TRACE(std::format("stopped after record {}", step));
        World world;
        const std::string installed = world.dump();
        auto options = world.options();
        options.stop_after = step;
        const auto result = execute(world.store(), world.plan(), options);
        ASSERT_TRUE(result.stopped);
        EXPECT_EQ(world.broadcasts(), 0);  // The run "died" before its end

        ASSERT_EQ(recover(world.store(), world.options()).size(), 1u);
        {
            const auto set = world.store().open_set(result.set_id);
            ASSERT_TRUE(set);
            for (const auto& status : set->statuses()) {
                EXPECT_NE(status.state, ItemState::BackedUp);
                EXPECT_NE(status.state, ItemState::Pending);
            }
        }
        // The edits of the machine environment come first, so after the first write the recovery
        // announces the change.
        EXPECT_EQ(world.broadcasts(), step >= 2 ? 1 : 0);

        restore(world.store(), result.set_id, {}, world.options());
        EXPECT_EQ(world.dump(), installed);
    }
}

}  // namespace
}  // namespace avel::changes
