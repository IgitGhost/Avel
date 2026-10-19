// The ChangeSet store in a temporary folder: plan, journal (including a torn last line), summary,
// locks, listing, purging with a fake clock, and the root's trust checks.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <format>
#include <fstream>
#include <thread>

#include "core/changes/serialization.h"
#include "core/changes/store.h"
#include "core/platform/file_ops.h"
#include "file_tree.h"

namespace avel::changes {
namespace {

using namespace std::chrono_literals;

class ChangeStoreTest : public ::testing::Test {
protected:
    StoreOptions options(const std::wstring& root) {
        GUID guid{};
        ::CoCreateGuid(&guid);
        StoreOptions result;
        result.root = root;
        result.mutex_name = std::format(L"Local\\Avel.Tests.{:08x}", guid.Data1);
        const std::wstring user = testing::current_user_sid_text();
        result.policy = {L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + user + L")", {user, L"BA", L"SY"}};
        result.clock = [this] { return now_; };
        return result;
    }

    std::unique_ptr<ChangeStore> open_store() {
        platform::FolderStatus status{};
        auto store = ChangeStore::open(options(temp_ / L"Changes"), status);
        EXPECT_EQ(status, platform::FolderStatus::Ok);
        return store;
    }

    static ChangePlan sample_plan() {
        ChangePlan plan;
        plan.description = L"Leftovers of Contoso Fixture";
        plan.items.push_back({leftovers::RegistryKeyLocation{{platform::Hive::Machine, L"", LR"(SOFTWARE\Contoso)"}},
                              leftovers::Category::Registry, {L"Contoso Fixture"}, leftovers::Confidence::VeryGood, 8});
        plan.items.push_back({leftovers::FileLocation{LR"(C:\Program Files\Contoso)", true},
                              leftovers::Category::Files, {L"Contoso Fixture"}, leftovers::Confidence::Good, 4});
        plan.items.push_back({leftovers::RegistryValueLocation{{platform::Hive::User, L"S-1-5-21-1", LR"(Software\Run)"},
                                                               std::wstring(L"odd \xD800 name")},
                              leftovers::Category::Registry, {}, leftovers::Confidence::Good, 6});
        return plan;
    }

    testing::TempFolder temp_;
    std::chrono::system_clock::time_point now_ = std::chrono::sys_days{std::chrono::year{2026} / 10 / 19} + 12h;
};

TEST_F(ChangeStoreTest, ASetKeepsItsPlanAndJournalAcrossReopening) {
    auto store = open_store();
    ASSERT_TRUE(store);
    std::wstring id;
    {
        auto set = store->create(sample_plan());
        ASSERT_TRUE(set);
        id = set->id();
        EXPECT_TRUE(id.starts_with(L"20261019-120000-"));
        EXPECT_TRUE(set->record(0, {ItemState::BackedUp}));
        EXPECT_TRUE(set->record(0, {ItemState::Done}));
        EXPECT_TRUE(set->record(1, {ItemState::Skipped, Reason::InUse, 32}));
        // A live set can't be opened twice.
        EXPECT_FALSE(store->open_set(id));
        EXPECT_TRUE(set->write_summary(SetState::Complete, 1234));
    }
    const auto set = store->open_set(id);
    ASSERT_TRUE(set);
    ASSERT_EQ(set->plan().items.size(), 3u);
    EXPECT_EQ(set->plan().description, L"Leftovers of Contoso Fixture");
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(leftovers::identity(set->plan().items[i].location), leftovers::identity(sample_plan().items[i].location));
    }
    EXPECT_EQ(set->plan().items[1].confidence, leftovers::Confidence::Good);
    EXPECT_EQ(set->plan().items[0].programs, std::vector<std::wstring>{L"Contoso Fixture"});
    EXPECT_EQ(set->statuses()[0].state, ItemState::Done);
    EXPECT_EQ(set->statuses()[1].state, ItemState::Skipped);
    EXPECT_EQ(set->statuses()[1].reason, Reason::InUse);
    EXPECT_EQ(set->statuses()[1].code, 32u);
    EXPECT_EQ(set->statuses()[2].state, ItemState::Pending);
    EXPECT_EQ(set->created(), now_);

    const auto summary = set->summary();
    ASSERT_TRUE(summary);
    EXPECT_EQ(summary->state, SetState::Complete);
    EXPECT_EQ(summary->done, 1u);
    EXPECT_EQ(summary->skipped, 1u);
    EXPECT_EQ(summary->quarantined_bytes, 1234u);
}

TEST_F(ChangeStoreTest, ATornJournalLineIsIgnored) {
    auto store = open_store();
    std::wstring id;
    {
        auto set = store->create(sample_plan());
        ASSERT_TRUE(set);
        id = set->id();
        set->record(0, {ItemState::BackedUp});
    }
    {
        // A crash in the middle of writing a line.
        std::ofstream journal(temp_ / (L"Changes\\" + id + L"\\journal.jsonl"), std::ios::app | std::ios::binary);
        journal << R"({"item":0,"state":"do)";
    }
    {
        auto set = store->open_set(id);
        ASSERT_TRUE(set);
        EXPECT_EQ(set->statuses()[0].state, ItemState::BackedUp);
        set->record(0, {ItemState::Done});
    }
    const auto set = store->open_set(id);
    ASSERT_TRUE(set);
    EXPECT_EQ(set->statuses()[0].state, ItemState::Done);
}

TEST_F(ChangeStoreTest, ListsNewestFirstAndPurgesOldFinishedSets) {
    auto store = open_store();
    std::vector<std::wstring> ids;
    for (int i = 0; i < 3; ++i) {
        auto set = store->create(sample_plan());
        ASSERT_TRUE(set);
        ids.push_back(set->id());
        if (i != 1) {
            set->write_summary(SetState::Complete);  // The second stays "running"
        }
        now_ += 24h;
    }
    const auto sets = store->list();
    ASSERT_EQ(sets.size(), 3u);
    EXPECT_EQ(sets[0].id, ids[2]);
    EXPECT_EQ(sets[2].id, ids[0]);
    EXPECT_EQ(sets[1].state, SetState::Running);

    now_ += 24h * 28;  // The first is 31 days old now, the third 29
    EXPECT_EQ(store->purge(), 1u);
    EXPECT_EQ(store->list().size(), 2u);
    EXPECT_EQ(::GetFileAttributesW((temp_ / (L"Changes\\" + ids[0])).c_str()), INVALID_FILE_ATTRIBUTES);
    now_ += 24h * 10;
    EXPECT_EQ(store->purge(), 1u);  // Never the running one; recovery deals with it first
    ASSERT_EQ(store->list().size(), 1u);
    EXPECT_EQ(store->list()[0].id, ids[1]);
}

TEST_F(ChangeStoreTest, QuarantineOnTheStoresVolumeIsInsideTheSet) {
    auto store = open_store();
    auto set = store->create(sample_plan());
    ASSERT_TRUE(set);
    const auto volume = platform::volume_root(temp_.path());
    ASSERT_TRUE(volume);
    EXPECT_EQ(set->quarantine_path(1, *volume), set->folder() + L"\\files\\0001");
    EXPECT_EQ(set->item_folder(2), set->folder() + L"\\items\\0002");
    EXPECT_NE(::GetFileAttributesW(set->item_folder(2).c_str()), INVALID_FILE_ATTRIBUTES);
}

TEST_F(ChangeStoreTest, TheLockSerializesStores) {
    auto store = open_store();
    auto guard = store->lock(0ms);
    ASSERT_TRUE(guard);
    // Anyone else (another thread here, another process in real life) has to wait.
    const auto try_elsewhere = [&] {
        bool acquired = false;
        std::thread([&] {
            wil::unique_mutex_nothrow other(::CreateMutexW(nullptr, FALSE, store->options().mutex_name.c_str()));
            const DWORD result = ::WaitForSingleObject(other.get(), 0);
            acquired = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
            if (acquired) {
                ::ReleaseMutex(other.get());
            }
        }).join();
        return acquired;
    };
    EXPECT_FALSE(try_elsewhere());
    guard.reset();
    EXPECT_TRUE(try_elsewhere());
}

TEST_F(ChangeStoreTest, RefusesRootsItCantTrust) {
    platform::FolderStatus status{};
    // Owned by someone the policy doesn't accept.
    auto strict = options(temp_ / L"Strict");
    testing::make_folder(temp_ / L"Strict");
    strict.policy.owners = {L"SY"};
    EXPECT_FALSE(ChangeStore::open(strict, status));
    EXPECT_EQ(status, platform::FolderStatus::ForeignOwner);

    // A junction to somewhere else.
    testing::make_folder(temp_ / L"Elsewhere");
    ASSERT_TRUE(testing::make_junction(temp_ / L"Linked", temp_ / L"Elsewhere"));
    EXPECT_FALSE(ChangeStore::open(options(temp_ / L"Linked"), status));
    EXPECT_EQ(status, platform::FolderStatus::ReparsePoint);

    // Below a junction.
    EXPECT_FALSE(ChangeStore::open(options(temp_ / L"Linked\\Changes"), status));
    EXPECT_EQ(status, platform::FolderStatus::Redirected);

    // A file in the way.
    testing::write_file(temp_ / L"File", "x");
    EXPECT_FALSE(ChangeStore::open(options(temp_ / L"File"), status));
    EXPECT_EQ(status, platform::FolderStatus::NotFolder);
}

}  // namespace
}  // namespace avel::changes
