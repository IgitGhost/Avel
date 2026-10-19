// The executor against a fixture: registry keys and values in volatile scratch keys (standing in
// for HKLM and a user's hive) and files in a temporary folder, with a store in that folder too.
// Removal then undo must give back exactly what was there, also after a crash at any step.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <format>
#include <memory>

#include "core/changes/executor.h"
#include "core/platform/file_ops.h"
#include "file_tree.h"
#include "registry_dump.h"
#include "scratch_registry.h"

namespace avel::changes {
namespace {

using leftovers::Category;
using leftovers::Confidence;
using platform::Hive;
using namespace std::chrono_literals;

constexpr wchar_t kSid[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kRun[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)";

// One copy of everything a plan touches, made fresh for each run.
class World {
public:
    World() {
        machine_.set_string(LR"(SOFTWARE\Contoso\Fixture)", L"InstallDir", program());
        machine_.set_dword(LR"(SOFTWARE\Contoso\Fixture\Settings)", L"Level", 3);
        machine_.set_string(LR"(SOFTWARE\Contoso)", L"", L"publisher key");
        machine_.set_string(kRun, L"ContosoUpdater", L"\"" + program() + LR"(\app.exe" /background)");
        machine_.set_string(kRun, L"Unrelated", L"keep me");
        user_.set_string(LR"(Software\Contoso Fixture)", L"Theme", L"dark");
        testing::write_file(program() + L"\\app.exe", "MZ fixture");
        testing::write_file(program() + L"\\sub\\lib.dll", "MZ lib");
        testing::write_file(temp_ / L"ProgramData\\Contoso\\settings.ini", "[x]");
        testing::write_file(temp_ / L"ProgramData\\Contoso\\keep.txt", "not planned");

        StoreOptions options;
        GUID guid{};
        ::CoCreateGuid(&guid);
        options.root = temp_ / L"Store";
        options.mutex_name = std::format(L"Local\\Avel.Tests.{:08x}", guid.Data1);
        const std::wstring user = testing::current_user_sid_text();
        options.policy = {L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + user + L")", {user, L"BA", L"SY"}};
        platform::FolderStatus status{};
        store_ = ChangeStore::open(options, status);
        EXPECT_TRUE(store_);
    }

    std::wstring program() const { return temp_ / L"Program Files\\Contoso"; }
    ChangeStore& store() { return *store_; }

    ExecuteOptions options() const {
        ExecuteOptions options;
        options.registry_root = [this](Hive hive, const std::wstring&) {
            return hive == Hive::Machine ? machine_.get() : user_.get();
        };
        options.protection = [](const leftovers::Location& location) {
            return leftovers::HardFloor::live().check(location);  // The real floor, always
        };
        return options;
    }

    ChangePlan plan() const {
        const auto item = [](leftovers::Location location, Category category) {
            return PlannedItem{std::move(location), category, {L"Contoso Fixture"}, Confidence::Good, 4};
        };
        ChangePlan plan;
        plan.description = L"Leftovers of Contoso Fixture";
        plan.items = {
            item(leftovers::RegistryKeyLocation{{Hive::Machine, L"", LR"(SOFTWARE\Contoso\Fixture)"}}, Category::Registry),
            item(leftovers::RegistryValueLocation{{Hive::Machine, L"", kRun}, L"ContosoUpdater"}, Category::Registry),
            item(leftovers::RegistryKeyLocation{{Hive::User, kSid, LR"(Software\Contoso Fixture)"}}, Category::Registry),
            item(leftovers::FileLocation{program(), true}, Category::Files),
            item(leftovers::FileLocation{temp_ / L"ProgramData\\Contoso\\settings.ini", false}, Category::Files),
            // Inside item 3: goes with it.
            item(leftovers::FileLocation{program() + L"\\sub", true}, Category::Files),
            // Refused by the floor before anything else is looked at.
            item(leftovers::FileLocation{LR"(C:\Windows\Avel.Tests.DoesNotExist)", true}, Category::Files),
            item(leftovers::FirewallRuleLocation{L"{rule}"}, Category::Firewall),
            item(leftovers::PackageLocation{L"Contoso.Fixture_1.0.0.0_x64__abc", kSid}, Category::Packages),
            item(leftovers::RegistryKeyLocation{{Hive::Machine, L"", LR"(SOFTWARE\Missing)"}}, Category::Registry),
        };
        return plan;
    }

    // Everything the plan could touch. The folders holding removed items are left out: their
    // last-write time changes when something leaves them and comes back, as with any removal.
    std::string dump() const {
        return testing::dump_key(machine_.get(), L"SOFTWARE") + testing::dump_key(user_.get(), L"Software") +
               testing::dump_tree(program()) + testing::dump_tree(temp_ / L"ProgramData\\Contoso\\settings.ini") +
               testing::dump_tree(temp_ / L"ProgramData\\Contoso\\keep.txt");
    }

private:
    testing::TempFolder temp_;
    testing::ScratchKey machine_;
    testing::ScratchKey user_;
    std::unique_ptr<ChangeStore> store_;
};

TEST(ExecutorTest, RemovesThePlanAndUndoesItExactly) {
    World world;
    const std::string before = world.dump();
    const auto result = execute(world.store(), world.plan(), world.options());
    ASSERT_FALSE(result.set_id.empty());
    ASSERT_FALSE(result.stopped);
    const auto state = [&](std::size_t i) { return result.items[i].status; };
    for (const std::size_t i : {0u, 1u, 2u, 3u, 4u}) {
        EXPECT_EQ(state(i).state, ItemState::Done) << i;
    }
    EXPECT_EQ(state(5).reason, Reason::Contained);
    EXPECT_EQ(state(6).reason, Reason::Protected);
    EXPECT_EQ(state(7).reason, Reason::ReportOnly);
    EXPECT_EQ(state(8).reason, Reason::NotUndoable);
    EXPECT_EQ(state(9).reason, Reason::Missing);
    EXPECT_EQ(result.quarantined_bytes, 10u + 6 + 3);

    EXPECT_EQ(::GetFileAttributesW(world.program().c_str()), INVALID_FILE_ATTRIBUTES);
    const std::string removed = world.dump();
    EXPECT_EQ(removed.find("ContosoUpdater"), std::string::npos);
    EXPECT_NE(removed.find(testing::hex_of(L"Unrelated")), std::string::npos);
    EXPECT_NE(removed.find("size=11"), std::string::npos);  // keep.txt, not planned, stays

    const auto summary = world.store().list().front();
    EXPECT_EQ(summary.state, SetState::Complete);
    EXPECT_EQ(summary.done, 5u);
    EXPECT_EQ(summary.quarantined_bytes, 19u);

    const auto undone = restore(world.store(), result.set_id, {}, world.options());
    ASSERT_TRUE(undone.found);
    EXPECT_EQ(undone.items.size(), 5u);
    EXPECT_EQ(undone.state, SetState::Restored);
    EXPECT_EQ(world.dump(), before);
    EXPECT_EQ(world.store().list().front().state, SetState::Restored);
}

TEST(ExecutorTest, UndoesSelectedItemsAndNeverReplacesWhatsNew) {
    World world;
    const std::string before = world.dump();
    const auto result = execute(world.store(), world.plan(), world.options());
    ASSERT_FALSE(result.set_id.empty());

    // Only the program's folder first.
    auto undone = restore(world.store(), result.set_id, {3}, world.options());
    EXPECT_EQ(undone.items.size(), 1u);
    EXPECT_EQ(undone.state, SetState::PartlyRestored);
    EXPECT_NE(::GetFileAttributesW(world.program().c_str()), INVALID_FILE_ATTRIBUTES);

    // The user's key reappeared meanwhile (the program was reinstalled): it stays as it is now.
    const auto options = world.options();
    HKEY user = options.registry_root(Hive::User, kSid);
    HKEY reinstalled = nullptr;
    ASSERT_EQ(::RegCreateKeyExW(user, LR"(Software\Contoso Fixture)", 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS,
                                nullptr, &reinstalled, nullptr),
              ERROR_SUCCESS);
    ::RegSetValueExW(reinstalled, L"New", 0, REG_SZ, reinterpret_cast<const BYTE*>(L"1"), 4);
    ::RegCloseKey(reinstalled);

    undone = restore(world.store(), result.set_id, {}, world.options());
    EXPECT_EQ(undone.state, SetState::PartlyRestored);
    for (const auto& [index, item] : undone.items) {
        EXPECT_EQ(item.status.state, index == 2 ? ItemState::RestoreSkipped : ItemState::Restored) << index;
    }
    const std::string after = world.dump();
    EXPECT_NE(after, before);
    EXPECT_NE(after.find(testing::hex_of(L"New")), std::string::npos);
    EXPECT_EQ(after.find(testing::hex_of(L"Theme")), std::string::npos);
}

TEST(ExecutorTest, FilesInUseStayAndComeBackWithTheRest) {
    World world;
    const testing::DumpOptions without_folder_ids{false};
    const auto dump = [&] { return testing::dump_tree(world.program(), without_folder_ids); };
    const std::string before = dump();
    std::wstring set_id;
    {
        wil::unique_hfile busy(::CreateFileW((world.program() + L"\\sub\\lib.dll").c_str(), GENERIC_READ,
                                             FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
        ASSERT_TRUE(busy);
        const auto result = execute(world.store(), world.plan(), world.options());
        set_id = result.set_id;
        EXPECT_EQ(result.items[3].status.state, ItemState::Partial);
        ASSERT_EQ(result.items[3].left_behind.size(), 1u);
        ASSERT_EQ(result.items[3].holders.size(), 1u);
        EXPECT_EQ(result.items[3].holders.front().pid, ::GetCurrentProcessId());
    }
    const auto undone = restore(world.store(), set_id, {3}, world.options());
    ASSERT_EQ(undone.items.size(), 1u);
    EXPECT_EQ(undone.items.front().second.status.state, ItemState::Restored);
    EXPECT_EQ(dump(), before);
}

TEST(ExecutorTest, ACrashAtAnyStepRecoversAndUndoesToTheOriginal) {
    // How many journal records a whole run writes.
    std::size_t records = 0;
    {
        World world;
        const auto result = execute(world.store(), world.plan(), world.options());
        for (const auto& item : result.items) {
            records += item.status.state == ItemState::Done || item.status.state == ItemState::Failed ? 2 : 1;
        }
    }
    ASSERT_GT(records, 10u);
    for (std::size_t step = 1; step < records; ++step) {
        SCOPED_TRACE(std::format("stopped after record {}", step));
        World world;
        const std::string before = world.dump();
        auto options = world.options();
        options.stop_after = step;
        const auto result = execute(world.store(), world.plan(), options);
        ASSERT_TRUE(result.stopped);
        EXPECT_EQ(world.store().list().front().state, SetState::Running);

        const auto recovered = recover(world.store(), world.options());
        ASSERT_EQ(recovered.size(), 1u);
        {
            const auto set = world.store().open_set(result.set_id);
            ASSERT_TRUE(set);
            for (const auto& status : set->statuses()) {
                EXPECT_NE(status.state, ItemState::BackedUp);
                EXPECT_NE(status.state, ItemState::Pending);
            }
            EXPECT_EQ(set->summary()->state, SetState::Complete);
        }  // Releases the set's lock for the undo

        restore(world.store(), result.set_id, {}, world.options());
        EXPECT_EQ(world.dump(), before);
    }
}

}  // namespace
}  // namespace avel::changes
