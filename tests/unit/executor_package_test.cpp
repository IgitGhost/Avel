// Packaged apps' items through the executor (ADR 0008): a user's registration, the provisioning and
// the removal for every user. Windows' packages are never touched here: FakePackages stands in
// for PackageManager, as Windows behaved in the Phase 6b spike (a package goes from this PC with
// its last user unless it is provisioned; a registration needs it staged). The Sandbox's `apps`
// scenario changes real packages.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <format>
#include <map>
#include <memory>
#include <set>

#include "core/changes/executor.h"
#include "core/changes/serialization.h"
#include "core/platform/text.h"
#include "file_tree.h"

namespace avel::changes {
namespace {

using leftovers::Category;
using leftovers::Confidence;
using leftovers::PackageLocation;
using leftovers::PackageProvisioningLocation;
using leftovers::PackageUserLocation;
using platform::PackageOperation;

constexpr wchar_t kMe[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kOther[] = L"S-1-5-21-1-2-3-1002";
constexpr wchar_t kFamily[] = L"Contoso.App_abc123";
constexpr wchar_t kFull[] = L"Contoso.App_1.0.0.0_x64__abc123";
constexpr wchar_t kNewer[] = L"Contoso.App_1.1.0.0_x64__abc123";
constexpr wchar_t kSystemFamily[] = L"Contoso.Shell_abc123";
constexpr wchar_t kSystemFull[] = L"Contoso.Shell_1.0.0.0_neutral__abc123";

// The packages of a pretend PC, by family.
class FakePackages : public PackageSystem {
public:
    struct Family {
        std::wstring full_name;  // The version on this PC
        std::set<std::wstring> users;
        bool provisioned = false;
        bool present = true;  // Staged
        bool system = false;  // SignatureKind System
    };

    std::map<std::wstring, Family> families;
    int operations = 0;
    bool register_lies = false;     // Reports success while nothing gets registered
    bool remove_all_fails = false;  // RemovePackageAsync(RemoveForAllUsers) fails

    std::wstring current_user_sid() override { return kMe; }

    std::vector<std::wstring> family_users(const std::wstring& family) override {
        const auto found = families.find(family);
        if (found == families.end() || !found->second.present) {
            return {};
        }
        return {found->second.users.begin(), found->second.users.end()};
    }

    std::optional<std::wstring> provisioned_package(const std::wstring& family) override {
        const auto found = families.find(family);
        if (found == families.end() || !found->second.present || !found->second.provisioned) {
            return std::nullopt;
        }
        return found->second.full_name;
    }

    bool family_staged(const std::wstring& family) override {
        const auto found = families.find(family);
        return found != families.end() && found->second.present;
    }

    PackageOperation remove_for_current_user(const std::wstring& full_name) override {
        ++operations;
        Family* family = by_full_name(full_name);
        if (!family || !family->users.erase(kMe)) {
            return {static_cast<std::int32_t>(0x80073CF1), 0, false, {}};  // ERROR_PACKAGE_NOT_FOUND
        }
        if (family->users.empty() && !family->provisioned) {
            family->present = false;  // Windows deletes it with its last user
        }
        return {0, 0, true, full_name};
    }

    PackageOperation register_for_current_user(const std::wstring& name) override {
        ++operations;
        Family* family = staged(name);
        if (!family) {
            return {static_cast<std::int32_t>(0x80073CF9), static_cast<std::int32_t>(0x80070490), false, {}};
        }
        if (register_lies) {
            return {0, 0, false, {}};
        }
        family->users.insert(kMe);
        return {0, 0, true, family->full_name};
    }

    PackageOperation deprovision(const std::wstring& name) override {
        ++operations;
        Family* family = staged(name);
        if (!family) {
            return {static_cast<std::int32_t>(0x80070490), 0, false, {}};
        }
        family->provisioned = false;
        return {0, 0, true, family->full_name};
    }

    PackageOperation provision(const std::wstring& name) override {
        ++operations;
        Family* family = staged(name);
        if (!family) {
            return {static_cast<std::int32_t>(0x80070490), 0, false, {}};
        }
        family->provisioned = true;
        return {0, 0, true, family->full_name};
    }

    PackageOperation remove_for_all_users(const std::wstring& full_name) override {
        ++operations;
        Family* family = by_full_name(full_name);
        if (!family || remove_all_fails) {
            return {static_cast<std::int32_t>(0x80073CFA), 0, false, {}};
        }
        *family = Family{family->full_name, {}, false, false, family->system};
        return {0, 0, true, full_name};
    }

    // Who has what, for comparing before and after.
    std::string dump() const {
        std::string text;
        for (const auto& [name, family] : families) {
            text += platform::to_utf8(name) + " " + platform::to_utf8(family.full_name) +
                    (family.present ? " staged" : " gone") + (family.provisioned ? " provisioned" : "");
            for (const auto& user : family.users) {
                text += " " + platform::to_utf8(user);
            }
            text += "\n";
        }
        return text;
    }

private:
    Family* staged(const std::wstring& name) {
        const auto found = families.find(name);
        return found != families.end() && found->second.present ? &found->second : nullptr;
    }

    Family* by_full_name(const std::wstring& full_name) {
        for (auto& [name, family] : families) {
            if (family.present && family.full_name == full_name) {
                return &family;
            }
        }
        return nullptr;
    }
};

PlannedItem item(leftovers::Location location) {
    const Category category = leftovers::category_of(location);
    return {std::move(location), category, {L"Contoso App"}, Confidence::Unknown, 0};
}

// Contoso.App is registered for me and another user and isn't provisioned; Contoso.Shell is a
// System package registered for me.
class World {
public:
    World() : packages_(std::make_shared<FakePackages>()) {
        packages_->families[kFamily] = {kFull, {kMe, kOther}, false, true, false};
        packages_->families[kSystemFamily] = {kSystemFull, {kMe}, true, true, true};

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
    FakePackages& packages() { return *packages_; }

    // The real floor's rules, with the fake's packages as its facts.
    ExecuteOptions options() const {
        ExecuteOptions options;
        options.packages = packages_;
        leftovers::HardFloor::Data data{LR"(C:\Windows)", {}, {}};
        data.packages =
            [packages = packages_](const std::wstring& full_name) -> std::optional<leftovers::PackageFacts> {
            for (const auto& [name, family] : packages->families) {
                if (family.present && family.full_name == full_name) {
                    return leftovers::PackageFacts{family.system, false, false};
                }
            }
            return std::nullopt;
        };
        options.protection = [floor = std::make_shared<leftovers::HardFloor>(std::move(data))](
                                 const leftovers::Location& location) { return floor->check(location); };
        return options;
    }

    std::optional<Json> state_json(const std::wstring& set_id, std::size_t index) {
        const auto set = store_->open_set(set_id);
        if (!set) {
            return std::nullopt;
        }
        const auto text = read_file(set->item_folder(index) + L"\\state.json");
        return text ? std::optional<Json>(Json::parse(*text)) : std::nullopt;
    }

private:
    testing::TempFolder temp_;
    std::shared_ptr<FakePackages> packages_;
    std::unique_ptr<ChangeStore> store_;
};

ChangePlan plan_of(std::vector<leftovers::Location> locations) {
    ChangePlan plan;
    plan.description = L"Windows apps";
    for (auto& location : locations) {
        plan.items.push_back(item(std::move(location)));
    }
    return plan;
}

const PackageUserLocation kRemoveForMe{kFamily, kFull, kMe, true, false};
const PackageProvisioningLocation kDeprovision{kFamily, kFull, true, false};

TEST(ExecutorPackageTest, RemovesForMeAndRegistersAgainOnUndo) {
    World world;
    const std::string before = world.packages().dump();
    const auto result = execute(world.store(), plan_of({kRemoveForMe}), world.options());
    ASSERT_EQ(result.items.size(), 1u);
    EXPECT_EQ(result.items[0].status.state, ItemState::Done);
    EXPECT_FALSE(world.packages().registered(kFamily, kMe));
    EXPECT_TRUE(world.packages().registered(kFamily, kOther));

    // What made undo possible, and the package acted on.
    const auto state = world.state_json(result.set_id, 0);
    ASSERT_TRUE(state);
    EXPECT_EQ((*state)["before"], true);
    EXPECT_EQ((*state)["written"], false);
    EXPECT_EQ((*state)["full_name"], platform::to_utf8(kFull));
    EXPECT_EQ((*state)["other_users"], Json::array({platform::to_utf8(kOther)}));
    EXPECT_EQ((*state)["provisioned"], false);

    // An update replaced the version in the meantime: undo registers what is staged then.
    world.packages().families[kFamily].full_name = kNewer;
    const auto undone = restore(world.store(), result.set_id, {}, world.options());
    ASSERT_EQ(undone.items.size(), 1u);
    EXPECT_EQ(undone.items[0].second.status.state, ItemState::Restored);
    EXPECT_EQ(undone.state, SetState::Restored);
    EXPECT_TRUE(world.packages().registered(kFamily, kMe));
    const auto restored = world.state_json(result.set_id, 0);
    ASSERT_TRUE(restored);
    EXPECT_EQ((*restored)["restored_full_name"], platform::to_utf8(kNewer));
    EXPECT_EQ((*restored)["other_users"], (*state)["other_users"]);
    world.packages().families[kFamily].full_name = kFull;
    EXPECT_EQ(world.packages().dump(), before);
}

TEST(ExecutorPackageTest, DeprovisionsAndProvisionsAgainOnUndo) {
    World world;
    world.packages().families[kFamily].provisioned = true;
    const std::string before = world.packages().dump();
    const auto result = execute(world.store(), plan_of({kDeprovision}), world.options());
    EXPECT_EQ(result.items[0].status.state, ItemState::Done);
    EXPECT_FALSE(world.packages().provisioned_package(kFamily));
    EXPECT_TRUE(world.packages().registered(kFamily, kMe));  // Users keep it
    const auto state = world.state_json(result.set_id, 0);
    ASSERT_TRUE(state);
    EXPECT_EQ((*state)["full_name"], platform::to_utf8(kFull));
    EXPECT_FALSE(state->contains("other_users"));

    const auto undone = restore(world.store(), result.set_id, {}, world.options());
    EXPECT_EQ(undone.state, SetState::Restored);
    EXPECT_EQ(world.packages().dump(), before);
}

// ADR 0008, decision A: Windows has no public way to change another user's registration.
TEST(ExecutorPackageTest, AnotherUsersRegistrationIsRefused) {
    World world;
    const std::string before = world.packages().dump();
    const auto result = execute(
        world.store(), plan_of({PackageUserLocation{kFamily, kFull, kOther, true, false}}), world.options());
    EXPECT_EQ(result.items[0].status.state, ItemState::Skipped);
    EXPECT_EQ(result.items[0].status.reason, Reason::OtherUser);
    EXPECT_EQ(world.packages().operations, 0);
    EXPECT_EQ(world.packages().dump(), before);
}

TEST(ExecutorPackageTest, AStateThatChangedSincePlanningIsSkipped) {
    World world;
    world.packages().families[kFamily].users.erase(kMe);  // Removed elsewhere since the plan
    const std::string before = world.packages().dump();
    const auto result = execute(world.store(), plan_of({kRemoveForMe, kDeprovision}), world.options());
    for (const auto& entry : result.items) {
        EXPECT_EQ(entry.status.state, ItemState::Skipped);
        EXPECT_EQ(entry.status.reason, Reason::Changed);
    }
    EXPECT_EQ(world.packages().operations, 0);
    EXPECT_EQ(world.packages().dump(), before);
}

// Windows deletes a package with its last user's registration unless it is provisioned: that
// removal couldn't be undone, so it is a removal for every user, never this kind.
TEST(ExecutorPackageTest, ARemovalThatWouldDeleteThePackageIsRefused) {
    World world;
    world.packages().families[kFamily].users.erase(kOther);
    const auto result = execute(world.store(), plan_of({kRemoveForMe}), world.options());
    EXPECT_EQ(result.items[0].status.state, ItemState::Skipped);
    EXPECT_EQ(result.items[0].status.reason, Reason::NotUndoable);
    EXPECT_TRUE(world.packages().registered(kFamily, kMe));

    // Provisioned, it stays on this PC: allowed.
    world.packages().families[kFamily].provisioned = true;
    const auto provisioned = execute(world.store(), plan_of({kRemoveForMe}), world.options());
    EXPECT_EQ(provisioned.items[0].status.state, ItemState::Done);
    EXPECT_TRUE(world.packages().family_staged(kFamily));
}

TEST(ExecutorPackageTest, UndoSkipsWhatChangedOrWentSinceTheChange) {
    {
        World world;
        const auto result = execute(world.store(), plan_of({kRemoveForMe}), world.options());
        ASSERT_EQ(result.items[0].status.state, ItemState::Done);
        world.packages().families[kFamily].users.insert(kMe);  // Installed again from the Store
        const int operations = world.packages().operations;
        const auto undone = restore(world.store(), result.set_id, {}, world.options());
        EXPECT_EQ(undone.items[0].second.status.state, ItemState::RestoreSkipped);
        EXPECT_EQ(undone.items[0].second.status.reason, Reason::Changed);
        EXPECT_EQ(world.packages().operations, operations);
    }
    {
        World world;
        const auto result = execute(world.store(), plan_of({kRemoveForMe}), world.options());
        ASSERT_EQ(result.items[0].status.state, ItemState::Done);
        // The other user removed it too, and Windows deleted it.
        world.packages().families[kFamily] = {kFull, {}, false, false, false};
        const auto undone = restore(world.store(), result.set_id, {}, world.options());
        EXPECT_EQ(undone.items[0].second.status.state, ItemState::RestoreSkipped);
        EXPECT_EQ(undone.items[0].second.status.reason, Reason::Missing);
        EXPECT_EQ(undone.state, SetState::Complete);
    }
    {
        World world;
        world.packages().families[kFamily].provisioned = true;
        const auto result = execute(world.store(), plan_of({kDeprovision}), world.options());
        ASSERT_EQ(result.items[0].status.state, ItemState::Done);
        world.packages().families[kFamily].provisioned = true;  // Provisioned again elsewhere
        const auto undone = restore(world.store(), result.set_id, {}, world.options());
        EXPECT_EQ(undone.items[0].second.status.reason, Reason::Changed);
    }
}

// ADR 0008: a registration can report success with nothing registered.
TEST(ExecutorPackageTest, SuccessIsJudgedByTheStateReadBack) {
    World world;
    const auto result = execute(world.store(), plan_of({kRemoveForMe}), world.options());
    ASSERT_EQ(result.items[0].status.state, ItemState::Done);
    world.packages().register_lies = true;
    const auto undone = restore(world.store(), result.set_id, {}, world.options());
    EXPECT_EQ(undone.items[0].second.status.state, ItemState::RestoreFailed);
    EXPECT_FALSE(world.packages().registered(kFamily, kMe));

    world.packages().register_lies = false;  // Retrying is safe
    const auto again = restore(world.store(), result.set_id, {}, world.options());
    EXPECT_EQ(again.items[0].second.status.state, ItemState::Restored);
    EXPECT_TRUE(world.packages().registered(kFamily, kMe));
}

TEST(ExecutorPackageTest, TheFloorRefusesWindowsOwnPackages) {
    World world;
    const std::string before = world.packages().dump();
    const auto result = execute(world.store(),
                                plan_of({PackageUserLocation{kSystemFamily, kSystemFull, kMe, true, false},
                                         PackageProvisioningLocation{kSystemFamily, kSystemFull, true, false},
                                         PackageLocation{kSystemFull, L"", true}}),
                                [&] {
                                    auto options = world.options();
                                    options.allow_irreversible = true;
                                    return options;
                                }());
    for (const auto& entry : result.items) {
        EXPECT_EQ(entry.status.reason, Reason::Protected);
    }
    EXPECT_EQ(world.packages().operations, 0);
    EXPECT_EQ(world.packages().dump(), before);
}

TEST(ExecutorPackageTest, RemovingForEveryUserIsIrreversibleAndHoldsTheFamilysItems) {
    World world;
    world.packages().families[kFamily].provisioned = true;
    const PackageLocation everyone{kFull, L"", true};
    const auto plan = plan_of({kRemoveForMe, everyone, kDeprovision});

    // Not allowed: the deep removal is left alone, and the others run on their own.
    const auto refused = execute(world.store(), plan, world.options());
    EXPECT_EQ(refused.items[1].status.reason, Reason::NotUndoable);
    EXPECT_EQ(refused.items[0].status.state, ItemState::Done);
    EXPECT_EQ(refused.items[2].status.state, ItemState::Done);
    ASSERT_EQ(restore(world.store(), refused.set_id, {}, world.options()).state, SetState::Restored);

    auto options = world.options();
    options.allow_irreversible = true;
    const auto result = execute(world.store(), plan, options);
    EXPECT_EQ(result.items[1].status.state, ItemState::Done);
    EXPECT_EQ(result.items[0].status.reason, Reason::Contained);
    EXPECT_EQ(result.items[2].status.reason, Reason::Contained);
    EXPECT_FALSE(world.packages().family_staged(kFamily));
    EXPECT_TRUE(world.packages().family_users(kFamily).empty());
    EXPECT_FALSE(world.packages().provisioned_package(kFamily));

    const int operations = world.packages().operations;
    const auto undone = restore(world.store(), result.set_id, {}, options);
    EXPECT_TRUE(undone.items.empty());
    EXPECT_EQ(world.packages().operations, operations);
}

TEST(ExecutorPackageTest, ARemovalForEveryUserThatStopsAfterDeprovisioningIsPartial) {
    World world;
    world.packages().families[kFamily].provisioned = true;
    world.packages().remove_all_fails = true;
    auto options = world.options();
    options.allow_irreversible = true;
    const auto result = execute(world.store(), plan_of({PackageLocation{kFull, L"", true}}), options);
    EXPECT_EQ(result.items[0].status.state, ItemState::Partial);
    EXPECT_EQ(result.items[0].status.code, 0x80073CFAu);
    EXPECT_FALSE(world.packages().provisioned_package(kFamily));
    EXPECT_TRUE(world.packages().registered(kFamily, kMe));
}

TEST(ExecutorPackageTest, ACrashAtAnyStepRecoversAndUndoesToTheStart) {
    // Two items, each records backed-up and done; for every user, backed-up then partial or done.
    const std::size_t records = 4;
    for (std::size_t step = 1; step < records; ++step) {
        SCOPED_TRACE(std::format("stopped after record {}", step));
        World world;
        world.packages().families[kFamily].provisioned = true;
        const std::string before = world.packages().dump();
        auto options = world.options();
        options.stop_after = step;
        const auto result = execute(world.store(), plan_of({kRemoveForMe, kDeprovision}), options);
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
        EXPECT_EQ(world.packages().dump(), before);
    }
}

TEST(ExecutorPackageTest, RecoverySettlesARemovalForEveryUserByLooking) {
    for (const bool removed : {false, true}) {
        SCOPED_TRACE(removed ? "removed" : "deprovisioned only");
        World world;
        world.packages().families[kFamily].provisioned = true;
        auto options = world.options();
        options.allow_irreversible = true;
        options.stop_after = 1;  // Backed up: the operations didn't run
        const auto result = execute(world.store(), plan_of({PackageLocation{kFull, L"", true}}), options);
        ASSERT_TRUE(result.stopped);
        // As if the process had died part way through.
        world.packages().deprovision(kFamily);
        if (removed) {
            world.packages().remove_for_all_users(kFull);
        }
        ASSERT_EQ(recover(world.store(), world.options()).size(), 1u);
        const auto set = world.store().open_set(result.set_id);
        ASSERT_TRUE(set);
        EXPECT_EQ(set->statuses()[0].state, removed ? ItemState::Done : ItemState::Partial);
    }
}

}  // namespace
}  // namespace avel::changes
