// State changes through the executor (ADR 0007). Startup approvals live in volatile scratch keys
// standing in for HKLM and a user's hive: disabled and enabled as Task Manager does it, undone
// exactly, skipped when the value changed since planning or since the change, missing keys created
// and removed again, and recovery after a crash at any step. Tasks and services change the system,
// so only their read-only paths run here (a missing one, one in another state than planned, and
// the floor refusing Microsoft's); the Sandbox's `startup` scenario changes them for real.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <format>
#include <map>
#include <memory>

#include "core/changes/executor.h"
#include "core/changes/system_items.h"
#include "core/leftovers/startup_approved.h"
#include "core/platform/nt_registry.h"
#include "file_tree.h"
#include "registry_dump.h"
#include "scratch_registry.h"

namespace avel::changes {
namespace {

using leftovers::ApprovalList;
using leftovers::Category;
using leftovers::Confidence;
using leftovers::ServiceStart;
using leftovers::ServiceStartLocation;
using leftovers::StartupApprovedLocation;
using leftovers::TaskStateLocation;
using platform::Hive;
using platform::RawValue;
using platform::RegistryAddress;

constexpr wchar_t kSid[] = L"S-1-5-21-1-2-3-1001";

FILETIME filetime(std::uint64_t value) {
    return {static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)};
}

// A Run value disabled earlier, the way Task Manager wrote it.
const FILETIME kEarlier = filetime(0x01DB2A3B4C5D6E7Full);
// The time the plans below disable at.
const FILETIME kNow = filetime(0x01DC000011112222ull);

PlannedItem item(leftovers::Location location) {
    const Category category = leftovers::category_of(location);
    return {std::move(location), category, {L"Startup"}, Confidence::VeryGood, 5};
}

// The machine's Run list holds Contoso (enabled, 02 and zeros) and Fabrikam (disabled); its Run32
// list doesn't exist. The user's hive holds nothing at all.
class World {
public:
    World() {
        const auto run = leftovers::approval_key(ApprovalList::Run, L"");
        set(machine_, run.path, leftovers::enabled_approval(L"Contoso"));
        set(machine_, run.path, leftovers::disabled_approval(L"Fabrikam", kEarlier));

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

    ExecuteOptions options() const {
        ExecuteOptions options;
        options.registry_root = [this](Hive hive, const std::wstring&) {
            return hive == Hive::Machine ? machine_.get() : user_.get();
        };
        options.protection = [](const leftovers::Location& location) {
            return leftovers::HardFloor::live().check(location);  // The real floor, with its allow-list
        };
        return options;
    }

    // Disable Contoso, enable Fabrikam, disable Contoso32 in the missing Run32 list, and disable the
    // user's UserApp, whose hive has no StartupApproved keys (nor even Software) yet.
    ChangePlan plan() const {
        const auto run = leftovers::approval_key(ApprovalList::Run, L"");
        const auto run32 = leftovers::approval_key(ApprovalList::Run32, L"");
        const auto user_run = leftovers::approval_key(ApprovalList::Run, kSid);
        ChangePlan plan;
        plan.description = L"Startup changes";
        plan.items = {
            item(StartupApprovedLocation{run, L"Contoso", value(run, L"Contoso"),
                                         leftovers::disabled_approval(L"Contoso", kNow)}),
            item(StartupApprovedLocation{run, L"Fabrikam", value(run, L"Fabrikam"),
                                         leftovers::enabled_approval(L"Fabrikam")}),
            item(StartupApprovedLocation{run32, L"Contoso32", std::nullopt,
                                         leftovers::disabled_approval(L"Contoso32", kNow)}),
            item(StartupApprovedLocation{user_run, L"UserApp", std::nullopt,
                                         leftovers::disabled_approval(L"UserApp", kNow)}),
        };
        return plan;
    }

    std::optional<RawValue> value(const RegistryAddress& key, const std::wstring& name) const {
        return read(key.hive == Hive::Machine ? machine_ : user_, key.path, name);
    }
    void set_value(const RegistryAddress& key, const RawValue& value) const {
        set(key.hive == Hive::Machine ? machine_ : user_, key.path, value);
    }
    bool key_exists(const RegistryAddress& key) const {
        HKEY raw = nullptr;
        const HKEY root = key.hive == Hive::Machine ? machine_.get() : user_.get();
        if (::RegOpenKeyExW(root, key.path.c_str(), 0, KEY_READ, &raw) != ERROR_SUCCESS) {
            return false;
        }
        ::RegCloseKey(raw);
        return true;
    }
    void create_key(const RegistryAddress& key) const {
        (key.hive == Hive::Machine ? machine_ : user_).create(key.path);
    }

    std::string dump() const {
        return testing::dump_key(machine_.get(), L"SOFTWARE") + testing::dump_key(user_.get(), L"Software");
    }

private:
    static void set(const testing::ScratchKey& root, std::wstring_view path, const RawValue& value) {
        const auto key = root.create(path);
        EXPECT_EQ(platform::set_raw_value(key.get(), value), ERROR_SUCCESS);
    }

    static std::optional<RawValue> read(const testing::ScratchKey& root, const std::wstring& path,
                                        const std::wstring& name) {
        HKEY raw = nullptr;
        if (::RegOpenKeyExW(root.get(), path.c_str(), 0, KEY_READ, &raw) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        const wil::unique_hkey key(raw);
        RawValue value;
        if (platform::query_raw_value(key.get(), name, value) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        return value;
    }

    testing::TempFolder temp_;
    testing::ScratchKey machine_;
    testing::ScratchKey user_;
    std::unique_ptr<ChangeStore> store_;
};

std::vector<std::byte> bytes_of(const std::optional<RawValue>& value) {
    return value ? value->data : std::vector<std::byte>();
}

std::map<std::size_t, ItemStatus> statuses_of(const RestoreResult& result) {
    std::map<std::size_t, ItemStatus> statuses;
    for (const auto& [index, item] : result.items) {
        statuses[index] = item.status;
    }
    return statuses;
}

TEST(ExecutorStateChangeTest, DisablesAndEnablesAsTaskManagerDoesAndUndoesExactly) {
    World world;
    const std::string before = world.dump();
    const ChangePlan plan = world.plan();
    const auto result = execute(world.store(), plan, world.options());
    ASSERT_FALSE(result.set_id.empty());
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        EXPECT_EQ(result.items[i].status.state, ItemState::Done) << i;
    }

    // 03 00 00 00 and the FILETIME to disable; 02 and 11 zero bytes to enable.
    const auto run = leftovers::approval_key(ApprovalList::Run, L"");
    const auto contoso = world.value(run, L"Contoso");
    ASSERT_TRUE(contoso);
    EXPECT_EQ(contoso->type, static_cast<DWORD>(REG_BINARY));
    EXPECT_EQ(contoso->data, leftovers::disabled_approval(L"Contoso", kNow).data);
    EXPECT_TRUE(leftovers::approval_disabled(*contoso));
    EXPECT_EQ(bytes_of(world.value(run, L"Fabrikam")), leftovers::enabled_approval(L"Fabrikam").data);
    // The missing lists were created for their values.
    const auto run32 = leftovers::approval_key(ApprovalList::Run32, L"");
    const auto user_run = leftovers::approval_key(ApprovalList::Run, kSid);
    EXPECT_TRUE(world.key_exists(run32));
    EXPECT_TRUE(world.value(run32, L"Contoso32"));
    EXPECT_TRUE(world.value(user_run, L"UserApp"));

    const auto undone = restore(world.store(), result.set_id, {}, world.options());
    ASSERT_TRUE(undone.found);
    EXPECT_EQ(undone.items.size(), 4u);
    EXPECT_EQ(undone.state, SetState::Restored);
    for (const auto& [index, status] : statuses_of(undone)) {
        EXPECT_EQ(status.state, ItemState::Restored) << index;
    }
    // Byte for byte as before, and the keys the change created are gone again.
    EXPECT_EQ(world.dump(), before);
    EXPECT_FALSE(world.key_exists(run32));
    EXPECT_FALSE(world.key_exists({Hive::User, kSid, L"Software"}));
}

TEST(ExecutorStateChangeTest, AValueThatChangedSincePlanningIsSkipped) {
    World world;
    const ChangePlan plan = world.plan();
    // After the plan was made, the user disabled Contoso in Task Manager, and something wrote a
    // UserApp approval of its own.
    const auto run = leftovers::approval_key(ApprovalList::Run, L"");
    const auto user_run = leftovers::approval_key(ApprovalList::Run, kSid);
    world.set_value(run, leftovers::disabled_approval(L"Contoso", kEarlier));
    world.set_value(user_run, leftovers::enabled_approval(L"UserApp"));
    const std::string changed = world.dump();
    const auto result = execute(world.store(), plan, world.options());
    EXPECT_EQ(result.items[0].status.state, ItemState::Skipped);
    EXPECT_EQ(result.items[0].status.reason, Reason::Changed);
    EXPECT_EQ(result.items[3].status.state, ItemState::Skipped);
    EXPECT_EQ(result.items[3].status.reason, Reason::Changed);
    EXPECT_EQ(bytes_of(world.value(run, L"Contoso")),
              leftovers::disabled_approval(L"Contoso", kEarlier).data);
    EXPECT_EQ(bytes_of(world.value(user_run, L"UserApp")), leftovers::enabled_approval(L"UserApp").data);
    EXPECT_EQ(result.items[1].status.state, ItemState::Done);
    EXPECT_EQ(result.items[2].status.state, ItemState::Done);

    // Undo leaves the skipped ones alone too.
    restore(world.store(), result.set_id, {}, world.options());
    EXPECT_EQ(world.dump(), changed);
}

TEST(ExecutorStateChangeTest, UndoSkipsWhatChangedSinceTheChange) {
    World world;
    const std::string before = world.dump();
    const auto result = execute(world.store(), world.plan(), world.options());
    ASSERT_FALSE(result.set_id.empty());
    // Since: the user enabled Contoso again in Task Manager, and something added a value of its
    // own to the user's list that Avel created.
    const auto run = leftovers::approval_key(ApprovalList::Run, L"");
    const auto user_run = leftovers::approval_key(ApprovalList::Run, kSid);
    world.set_value(run, leftovers::enabled_approval(L"Contoso"));
    world.set_value(user_run, leftovers::enabled_approval(L"Other"));

    const auto undone = restore(world.store(), result.set_id, {}, world.options());
    auto statuses = statuses_of(undone);
    EXPECT_EQ(statuses[0].state, ItemState::RestoreSkipped);
    EXPECT_EQ(statuses[0].reason, Reason::Changed);
    EXPECT_EQ(bytes_of(world.value(run, L"Contoso")), leftovers::enabled_approval(L"Contoso").data);
    EXPECT_EQ(statuses[1].state, ItemState::Restored);
    EXPECT_EQ(statuses[2].state, ItemState::Restored);
    // UserApp's approval goes, but the list holds another value now, so it stays, with its parents.
    EXPECT_EQ(statuses[3].state, ItemState::Restored);
    EXPECT_FALSE(world.value(user_run, L"UserApp"));
    EXPECT_TRUE(world.value(user_run, L"Other"));
    EXPECT_EQ(undone.state, SetState::PartlyRestored);

    // Retrying changes nothing: Contoso still isn't what the change wrote.
    const std::string after = world.dump();
    const auto again = restore(world.store(), result.set_id, {0}, world.options());
    ASSERT_EQ(again.items.size(), 1u);
    EXPECT_EQ(again.items[0].second.status.reason, Reason::Changed);
    EXPECT_EQ(world.dump(), after);
    EXPECT_NE(world.dump(), before);
}

TEST(ExecutorStateChangeTest, ACrashAtAnyStepRecoversAndUndoesToTheStart) {
    // Every item records backed-up and done.
    const std::size_t records = 8;
    for (std::size_t step = 1; step < records; ++step) {
        SCOPED_TRACE(std::format("stopped after record {}", step));
        World world;
        const std::string before = world.dump();
        auto options = world.options();
        options.stop_after = step;
        const auto result = execute(world.store(), world.plan(), options);
        ASSERT_TRUE(result.stopped);

        ASSERT_EQ(recover(world.store(), world.options()).size(), 1u);
        {
            const auto set = world.store().open_set(result.set_id);
            ASSERT_TRUE(set);
            for (const auto& status : set->statuses()) {
                EXPECT_NE(status.state, ItemState::BackedUp);
                EXPECT_NE(status.state, ItemState::Pending);
            }
        }
        restore(world.store(), result.set_id, {}, world.options());
        EXPECT_EQ(world.dump(), before);
    }
}

TEST(ExecutorStateChangeTest, RecoveryRemovesAKeyACrashLeftEmpty) {
    // The run dies after backing up the user's change, having created the list but not yet
    // written its value.
    World world;
    auto options = world.options();
    options.stop_after = 7;  // The user's item is last: its backed-up record is the 7th
    const auto result = execute(world.store(), world.plan(), options);
    ASSERT_TRUE(result.stopped);
    world.create_key(leftovers::approval_key(ApprovalList::Run, kSid));

    ASSERT_EQ(recover(world.store(), world.options()).size(), 1u);
    const auto set = world.store().open_set(result.set_id);
    ASSERT_TRUE(set);
    EXPECT_EQ(set->statuses()[3].state, ItemState::Failed);
    EXPECT_EQ(set->statuses()[3].reason, Reason::Interrupted);
    EXPECT_FALSE(world.key_exists({Hive::User, kSid, L"Software"}));
}

TEST(ExecutorStateChangeTest, TheFloorRefusesApprovalsOutsideItsAllowList) {
    World world;
    const auto run = leftovers::approval_key(ApprovalList::Run, L"");
    RawValue short_value = leftovers::enabled_approval(L"Contoso");
    short_value.data.resize(4);
    ChangePlan plan;
    plan.description = L"Not allowed";
    plan.items = {
        // The Run key itself, not its approval list.
        item(
            StartupApprovedLocation{{Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)"},
                                    L"Contoso",
                                    std::nullopt,
                                    leftovers::enabled_approval(L"Contoso")}),
        // Not as Task Manager writes it.
        item(StartupApprovedLocation{run, L"Contoso", world.value(run, L"Contoso"), short_value}),
    };
    const std::string before = world.dump();
    const auto result = execute(world.store(), plan, world.options());
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        EXPECT_EQ(result.items[i].status.state, ItemState::Skipped) << i;
        EXPECT_EQ(result.items[i].status.reason, Reason::Protected) << i;
    }
    EXPECT_EQ(world.dump(), before);
}

// Read-only: a task and a service that don't exist are reported missing, and nothing is created.
TEST(ExecutorStateChangeTest, AMissingTaskOrServiceIsSkipped) {
    World world;
    GUID guid{};
    ::CoCreateGuid(&guid);
    const std::wstring name = std::format(L"Avel.Tests.Missing.{:08x}", guid.Data1);
    ChangePlan plan;
    plan.description = L"Missing";
    plan.items = {
        item(TaskStateLocation{L"\\" + name + L"\\Updater", true, false}),
        item(ServiceStartLocation{name, {SERVICE_AUTO_START, false}, {SERVICE_DEMAND_START, false}}),
    };
    const auto result = execute(world.store(), plan, world.options());
    for (std::size_t i = 0; i < plan.items.size(); ++i) {
        EXPECT_EQ(result.items[i].status.state, ItemState::Skipped) << i;
        EXPECT_EQ(result.items[i].status.reason, Reason::Missing) << i;
    }
    bool enabled = false;
    ItemStatus failure;
    EXPECT_FALSE(task_enabled(L"\\" + name + L"\\Updater", enabled, failure));
    EXPECT_EQ(failure.reason, Reason::Missing);
}

// Read-only: Windows' event log service is refused by the floor; and with the floor out of the
// way, a plan made for another start than the service has is skipped before anything changes.
TEST(ExecutorStateChangeTest, AServiceInAnotherStateOrMicrosoftsIsLeftAlone) {
    World world;
    ServiceStart live;
    ItemStatus failure;
    if (!service_start(L"EventLog", live, failure)) {
        GTEST_SKIP() << "EventLog can't be read here";
    }
    EXPECT_EQ(live.start_type, static_cast<std::uint32_t>(SERVICE_AUTO_START));
    const ServiceStart impossible{0xFFFF, false};  // Never the live state: the change can't happen
    ChangePlan plan;
    plan.description = L"EventLog";
    plan.items = {item(ServiceStartLocation{L"EventLog", impossible, {SERVICE_DEMAND_START, live.delayed}})};

    const auto refused = execute(world.store(), plan, world.options());
    EXPECT_EQ(refused.items[0].status.state, ItemState::Skipped);
    EXPECT_EQ(refused.items[0].status.reason, Reason::Protected);

    auto options = world.options();
    options.protection = [](const leftovers::Location&) { return leftovers::ProtectionReason::None; };
    const auto skipped = execute(world.store(), plan, options);
    EXPECT_EQ(skipped.items[0].status.state, ItemState::Skipped);
    EXPECT_EQ(skipped.items[0].status.reason, Reason::Changed);
    ServiceStart after;
    ASSERT_TRUE(service_start(L"EventLog", after, failure));
    EXPECT_EQ(after, live);
}

}  // namespace
}  // namespace avel::changes
