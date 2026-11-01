// The Startup manager's changes (tools/startup_changes): the planner on hand-built entries (each
// kind, each action, every issue), one disable and one enable run end to end through the executor
// on scratch registry roots and undone exactly, and the services Avel disabled as a temporary
// change store records them. Nothing here touches real tasks, services or the live registry.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/changes/executor.h"
#include "core/leftovers/protection.h"
#include "core/leftovers/startup_approved.h"
#include "core/platform/nt_registry.h"
#include "core/platform/paths.h"
#include "core/tools/startup.h"
#include "core/tools/startup_changes.h"
#include "file_tree.h"
#include "registry_dump.h"
#include "scratch_registry.h"

namespace avel::tools {
namespace {

using leftovers::ApprovalList;
using leftovers::Category;
using leftovers::FileLocation;
using leftovers::Location;
using leftovers::RegistryKeyLocation;
using leftovers::RegistryValueLocation;
using leftovers::ServiceLocation;
using leftovers::ServiceStart;
using leftovers::ServiceStartLocation;
using leftovers::StartupApprovedLocation;
using leftovers::TaskLocation;
using leftovers::TaskStateLocation;
using platform::Hive;
using platform::RawValue;
using platform::RegistryAddress;
using Code = StartupPlanIssueCode;

constexpr wchar_t kSid[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kRun[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)";

FILETIME filetime(std::uint64_t value) {
    return {static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)};
}

const FILETIME kEarlier = filetime(0x01DB2A3B4C5D6E7Full);  // When Task Manager disabled an entry
const FILETIME kNow = filetime(0x01DC000011112222ull);      // When the plans below disable

// Entries as list_startup makes them.
StartupEntry approved_entry(std::wstring id, StartupKind kind, Location primary, ApprovalList list,
                            std::wstring value_name, std::optional<RawValue> approval) {
    StartupEntry entry;
    entry.id = std::move(id);
    entry.kind = kind;
    entry.name = value_name;
    entry.command = LR"("C:\Program Files\Contoso\app.exe")";
    entry.delete_locations.push_back(std::move(primary));
    const RegistryAddress key = leftovers::approval_key(list, L"");
    if (approval) {
        entry.delete_locations.push_back(RegistryValueLocation{key, value_name});
        entry.state =
            leftovers::approval_disabled(*approval) ? StartupState::Disabled : StartupState::Enabled;
    }
    entry.approval = StartupApproval{key, value_name, std::move(approval)};
    return entry;
}

StartupEntry run_entry(std::wstring id, std::wstring name, std::optional<RawValue> approval) {
    return approved_entry(std::move(id), StartupKind::Run,
                          RegistryValueLocation{{Hive::Machine, L"", kRun}, name}, ApprovalList::Run, name,
                          std::move(approval));
}

StartupEntry folder_entry(std::wstring id, std::wstring file, std::optional<RawValue> approval) {
    return approved_entry(
        std::move(id), StartupKind::StartupFolder,
        FileLocation{LR"(C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Startup\)" + file, false},
        ApprovalList::StartupFolder, file, std::move(approval));
}

StartupEntry task_entry(std::wstring id, std::wstring path, bool enabled) {
    StartupEntry entry;
    entry.id = std::move(id);
    entry.kind = StartupKind::Task;
    entry.name = path;
    entry.task_enabled = enabled;
    entry.state = enabled ? StartupState::Enabled : StartupState::Disabled;
    entry.delete_locations.push_back(TaskLocation{std::move(path), false});
    return entry;
}

StartupEntry service_entry(std::wstring id, std::wstring name, std::uint32_t start, bool delayed) {
    StartupEntry entry;
    entry.id = std::move(id);
    entry.kind = StartupKind::Service;
    entry.name = name;
    entry.service_start = start;
    entry.service_delayed = delayed;
    entry.state = start == SERVICE_AUTO_START ? StartupState::Enabled : StartupState::Disabled;
    entry.disabled_by_avel = start != SERVICE_AUTO_START;
    entry.delete_locations.push_back(ServiceLocation{std::move(name), false});
    return entry;
}

// Entries with no state of their own: delete only.
StartupEntry run_once_entry(std::wstring id) {
    StartupEntry entry;
    entry.id = std::move(id);
    entry.kind = StartupKind::RunOnce;
    entry.name = L"Finish";
    entry.delete_locations.push_back(RegistryValueLocation{
        {Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce)"}, L"Finish"});
    return entry;
}

StartupEntry run_once_ex_entry(std::wstring id) {
    StartupEntry entry;
    entry.id = std::move(id);
    entry.kind = StartupKind::RunOnceEx;
    entry.name = L"0001";
    entry.delete_locations.push_back(RegistryKeyLocation{
        {Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnceEx\0001)"}});
    return entry;
}

StartupEntry user_wow64_run_entry(std::wstring id) {
    StartupEntry entry;
    entry.id = std::move(id);
    entry.kind = StartupKind::Run;
    entry.user_sid = kSid;
    entry.wow64 = true;
    entry.name = L"Tool32";
    entry.delete_locations.push_back(RegistryValueLocation{
        {Hive::User, kSid, LR"(Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Run)"}, L"Tool32"});
    return entry;  // No approval: a user's 32-bit Run key has no list
}

std::vector<std::wstring> ids(std::initializer_list<const wchar_t*> list) {
    return std::vector<std::wstring>(list.begin(), list.end());
}

StartupPlan plan(const std::vector<StartupEntry>& entries, StartupAction action,
                 std::initializer_list<const wchar_t*> asked,
                 ServiceDisable disable = ServiceDisable::Manual) {
    return plan_startup(entries, action, ids(asked), disable, kNow);
}

TEST(StartupPlanTest, RunAndStartupFolderEntriesChangeAsTaskManagerDoes) {
    const std::vector<StartupEntry> entries = {
        run_entry(L"0000000001", L"Contoso", std::nullopt),  // Enabled: no value yet
        run_entry(L"0000000002", L"Fabrikam", leftovers::disabled_approval(L"Fabrikam", kEarlier)),
        folder_entry(L"0000000003", L"Tool.lnk", leftovers::enabled_approval(L"Tool.lnk")),
    };
    const auto disabled = plan(entries, StartupAction::Disable, {L"0000000001", L"0000000003"});
    EXPECT_TRUE(disabled.issues.empty());
    EXPECT_EQ(disabled.planned, ids({L"0000000001", L"0000000003"}));
    ASSERT_EQ(disabled.plan.items.size(), 2u);
    EXPECT_TRUE(disabled.plan.description.empty());  // The caller's

    const auto* contoso = std::get_if<StartupApprovedLocation>(&disabled.plan.items[0].location);
    ASSERT_NE(contoso, nullptr);
    EXPECT_TRUE(platform::same_address(contoso->key, leftovers::approval_key(ApprovalList::Run, L"")));
    EXPECT_EQ(contoso->value_name, L"Contoso");
    EXPECT_FALSE(contoso->expected);  // The value is absent
    ASSERT_TRUE(contoso->desired);
    EXPECT_EQ(contoso->desired->data, leftovers::disabled_approval(L"Contoso", kNow).data);
    EXPECT_EQ(disabled.plan.items[0].category, Category::Registry);
    EXPECT_EQ(disabled.plan.items[0].confidence, leftovers::Confidence::Unknown);
    EXPECT_EQ(disabled.plan.items[0].score, 0);

    const auto* tool = std::get_if<StartupApprovedLocation>(&disabled.plan.items[1].location);
    ASSERT_NE(tool, nullptr);
    EXPECT_TRUE(platform::same_address(tool->key, leftovers::approval_key(ApprovalList::StartupFolder, L"")));
    ASSERT_TRUE(tool->expected);
    EXPECT_EQ(tool->expected->data, leftovers::enabled_approval(L"Tool.lnk").data);

    const auto enabled = plan(entries, StartupAction::Enable, {L"0000000002"});
    EXPECT_TRUE(enabled.issues.empty());
    ASSERT_EQ(enabled.plan.items.size(), 1u);
    const auto* fabrikam = std::get_if<StartupApprovedLocation>(&enabled.plan.items[0].location);
    ASSERT_NE(fabrikam, nullptr);
    ASSERT_TRUE(fabrikam->expected);
    EXPECT_EQ(fabrikam->expected->data, leftovers::disabled_approval(L"Fabrikam", kEarlier).data);
    ASSERT_TRUE(fabrikam->desired);
    EXPECT_EQ(fabrikam->desired->data, leftovers::enabled_approval(L"Fabrikam").data);
    // What the planner writes passes the floor's allow-list.
    EXPECT_EQ(leftovers::HardFloor::live().check(enabled.plan.items[0].location),
              leftovers::ProtectionReason::None);
}

TEST(StartupPlanTest, TasksFlipTheirEnabledFlag) {
    const std::vector<StartupEntry> entries = {task_entry(L"0000000001", LR"(\Contoso\Updater)", true),
                                               task_entry(L"0000000002", LR"(\Contoso\Sync)", false)};
    const auto disabled = plan(entries, StartupAction::Disable, {L"0000000001"});
    ASSERT_EQ(disabled.plan.items.size(), 1u);
    const auto* updater = std::get_if<TaskStateLocation>(&disabled.plan.items[0].location);
    ASSERT_NE(updater, nullptr);
    EXPECT_EQ(updater->path, LR"(\Contoso\Updater)");
    EXPECT_TRUE(updater->enabled_before);
    EXPECT_FALSE(updater->enabled_after);
    EXPECT_EQ(disabled.plan.items[0].category, Category::Tasks);

    const auto enabled = plan(entries, StartupAction::Enable, {L"0000000002"});
    ASSERT_EQ(enabled.plan.items.size(), 1u);
    const auto* sync = std::get_if<TaskStateLocation>(&enabled.plan.items[0].location);
    ASSERT_NE(sync, nullptr);
    EXPECT_FALSE(sync->enabled_before);
    EXPECT_TRUE(sync->enabled_after);
}

TEST(StartupPlanTest, ServicesChangeOnlyTheirStartType) {
    const std::vector<StartupEntry> entries = {
        service_entry(L"0000000001", L"ContosoSvc", SERVICE_AUTO_START, true),
        service_entry(L"0000000002", L"ContosoManual", SERVICE_DEMAND_START, true),
        service_entry(L"0000000003", L"ContosoOff", SERVICE_DISABLED, false),
    };
    const auto manual = plan(entries, StartupAction::Disable, {L"0000000001"});
    ASSERT_EQ(manual.plan.items.size(), 1u);
    const auto* change = std::get_if<ServiceStartLocation>(&manual.plan.items[0].location);
    ASSERT_NE(change, nullptr);
    EXPECT_EQ(change->name, L"ContosoSvc");
    EXPECT_EQ(change->before, (ServiceStart{SERVICE_AUTO_START, true}));
    EXPECT_EQ(change->after, (ServiceStart{SERVICE_DEMAND_START, true}));  // Delayed kept
    EXPECT_EQ(manual.plan.items[0].category, Category::Services);

    const auto off = plan(entries, StartupAction::Disable, {L"0000000001"}, ServiceDisable::Disabled);
    ASSERT_EQ(off.plan.items.size(), 1u);
    EXPECT_EQ(std::get<ServiceStartLocation>(off.plan.items[0].location).after,
              (ServiceStart{SERVICE_DISABLED, true}));

    // A service Avel set to Manual can still be set to Disabled.
    const auto further = plan(entries, StartupAction::Disable, {L"0000000002"}, ServiceDisable::Disabled);
    ASSERT_EQ(further.plan.items.size(), 1u);
    EXPECT_EQ(std::get<ServiceStartLocation>(further.plan.items[0].location).before,
              (ServiceStart{SERVICE_DEMAND_START, true}));

    const auto enabled = plan(entries, StartupAction::Enable, {L"0000000002", L"0000000003"});
    ASSERT_EQ(enabled.plan.items.size(), 2u);
    EXPECT_EQ(std::get<ServiceStartLocation>(enabled.plan.items[0].location).after,
              (ServiceStart{SERVICE_AUTO_START, true}));
    EXPECT_EQ(std::get<ServiceStartLocation>(enabled.plan.items[1].location).after,
              (ServiceStart{SERVICE_AUTO_START, false}));
}

TEST(StartupPlanTest, AnEntryAlreadyInTheStateIsReportedNotPlanned) {
    const std::vector<StartupEntry> entries = {
        run_entry(L"0000000001", L"Fabrikam", leftovers::disabled_approval(L"Fabrikam", kEarlier)),
        run_entry(L"0000000002", L"Contoso", leftovers::enabled_approval(L"Contoso")),
        task_entry(L"0000000003", LR"(\Contoso\Updater)", true),
        service_entry(L"0000000004", L"ContosoManual", SERVICE_DEMAND_START, false),
        service_entry(L"0000000005", L"ContosoOff", SERVICE_DISABLED, false),
        service_entry(L"0000000006", L"ContosoSvc", SERVICE_AUTO_START, false),
    };
    const auto disable = plan(entries, StartupAction::Disable, {L"0000000001", L"0000000004", L"0000000005"});
    EXPECT_TRUE(disable.plan.items.empty());
    EXPECT_TRUE(disable.planned.empty());
    ASSERT_EQ(disable.issues.size(), 3u);
    for (const auto& issue : disable.issues) {
        EXPECT_EQ(issue.code, Code::AlreadyInState) << platform::to_utf8(issue.id);
        EXPECT_FALSE(is_error(issue.code));
    }
    // Disabled covers Manual, not the other way round.
    const auto off = plan(entries, StartupAction::Disable, {L"0000000005"}, ServiceDisable::Disabled);
    ASSERT_EQ(off.issues.size(), 1u);
    EXPECT_EQ(off.issues[0].code, Code::AlreadyInState);

    const auto enable = plan(entries, StartupAction::Enable, {L"0000000002", L"0000000003", L"0000000006"});
    EXPECT_TRUE(enable.plan.items.empty());
    ASSERT_EQ(enable.issues.size(), 3u);
    for (const auto& issue : enable.issues) {
        EXPECT_EQ(issue.code, Code::AlreadyInState);
    }
}

TEST(StartupPlanTest, UnknownProtectedAndUnsupportedEntriesAreRefused) {
    auto windows = run_entry(L"00000000aa", L"SecurityHealth", std::nullopt);
    windows.protection = StartupProtection::WindowsOwn;
    const std::vector<StartupEntry> entries = {
        windows, run_once_entry(L"00000000bb"), run_once_ex_entry(L"00000000cc"),
        user_wow64_run_entry(L"00000000dd"), run_entry(L"00000000ee", L"Contoso", std::nullopt)};
    for (const auto action : {StartupAction::Disable, StartupAction::Enable}) {
        const auto result = plan(entries, action,
                                 {L"0123456789", L"00000000aa", L"00000000bb", L"00000000cc", L"00000000dd"});
        EXPECT_TRUE(result.plan.items.empty());
        ASSERT_EQ(result.issues.size(), 5u);
        EXPECT_EQ(result.issues[0].id, L"0123456789");
        EXPECT_EQ(result.issues[0].code, Code::UnknownId);
        EXPECT_EQ(result.issues[1].code, Code::Protected);
        EXPECT_EQ(result.issues[2].code, Code::NotSupported);
        EXPECT_EQ(result.issues[3].code, Code::NotSupported);
        EXPECT_EQ(result.issues[4].code, Code::NotSupported);
        for (const auto& issue : result.issues) {
            EXPECT_TRUE(is_error(issue.code));
        }
    }
    // Windows' own entries can't be deleted either; the others can.
    const auto deleted =
        plan(entries, StartupAction::Delete, {L"00000000aa", L"00000000bb", L"00000000cc", L"00000000dd"});
    ASSERT_EQ(deleted.issues.size(), 1u);
    EXPECT_EQ(deleted.issues[0].code, Code::Protected);
    EXPECT_EQ(deleted.planned, ids({L"00000000bb", L"00000000cc", L"00000000dd"}));
    ASSERT_EQ(deleted.plan.items.size(), 3u);
    EXPECT_TRUE(std::holds_alternative<RegistryKeyLocation>(deleted.plan.items[1].location));
}

TEST(StartupPlanTest, DeleteTakesTheDeleteLocationsAsTheyAre) {
    auto run = run_entry(L"0000000001", L"Contoso", leftovers::disabled_approval(L"Contoso", kEarlier));
    run.program_id = L"contoso-id";
    const std::vector<StartupEntry> entries = {
        run, folder_entry(L"0000000002", L"Tool.lnk", std::nullopt),
        task_entry(L"0000000003", LR"(\Contoso\Updater)", true),
        service_entry(L"0000000004", L"ContosoSvc", SERVICE_AUTO_START, false)};
    const auto result = plan_startup(
        entries, StartupAction::Delete, ids({L"0000000001", L"0000000002", L"0000000003", L"0000000004"}),
        ServiceDisable::Manual, kNow, [](const std::wstring& id) { return L"Name of " + id; });
    EXPECT_TRUE(result.issues.empty());
    std::vector<std::wstring> expected;
    for (const auto& entry : entries) {
        for (const auto& location : entry.delete_locations) {
            expected.push_back(leftovers::identity(location));
        }
    }
    ASSERT_EQ(result.plan.items.size(), expected.size());  // The run value and its approval: not added twice
    EXPECT_EQ(result.plan.items.size(), 5u);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(leftovers::identity(result.plan.items[i].location), expected[i]) << i;
        EXPECT_EQ(result.plan.items[i].category, leftovers::category_of(result.plan.items[i].location)) << i;
    }
    EXPECT_EQ(result.plan.items[0].programs, ids({L"Name of contoso-id"}));
    EXPECT_EQ(result.plan.items[1].programs, ids({L"Name of contoso-id"}));
    EXPECT_TRUE(result.plan.items[2].programs.empty());
    EXPECT_EQ(result.plan.items[3].category, Category::Tasks);
    EXPECT_EQ(result.plan.items[4].category, Category::Services);
}

TEST(StartupPlanTest, IdsIgnoreCaseAndArePlannedOnce) {
    const std::vector<StartupEntry> entries = {task_entry(L"00000000ab", LR"(\Contoso\Updater)", true)};
    const auto result = plan(entries, StartupAction::Disable, {L"00000000AB", L"00000000ab"});
    EXPECT_TRUE(result.issues.empty());
    EXPECT_EQ(result.planned, ids({L"00000000ab"}));
    EXPECT_EQ(result.plan.items.size(), 1u);
    // Without a program_name, an owner is named by its id.
    auto owned = entries;
    owned[0].program_id = L"contoso-id";
    EXPECT_EQ(plan(owned, StartupAction::Disable, {L"00000000ab"}).plan.items[0].programs,
              ids({L"contoso-id"}));
}

// A change store in a temporary folder that only this test's processes may use.
std::unique_ptr<changes::ChangeStore> temporary_store(
    const std::wstring& root, std::function<std::chrono::system_clock::time_point()> clock) {
    changes::StoreOptions options;
    GUID guid{};
    ::CoCreateGuid(&guid);
    options.root = root;
    options.mutex_name = std::format(L"Local\\Avel.Tests.{:08x}", guid.Data1);
    const std::wstring user = testing::current_user_sid_text();
    options.policy = {L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + user + L")",
                      {user, L"BA", L"SY"}};
    if (clock) {
        options.clock = std::move(clock);
    }
    platform::FolderStatus status{};
    auto store = changes::ChangeStore::open(options, status);
    EXPECT_TRUE(store);
    return store;
}

// The machine's Run key in a scratch key standing in for HKLM: Contoso (no StartupApproved value
// yet) and Fabrikam (disabled by Task Manager earlier). Listed, changed and undone for real.
TEST(StartupChangesTest, DisablingAndEnablingRunValuesIsExactAndUndone) {
    testing::ScratchKey machine;
    testing::TempFolder temp;
    machine.set_string(kRun, L"Contoso", LR"("C:\Program Files\Contoso\app.exe" /tray)");
    machine.set_string(kRun, L"Fabrikam", LR"(C:\Program Files\Fabrikam\fab.exe)");
    {
        const auto approved = machine.create(leftovers::approval_key(ApprovalList::Run, L"").path);
        ASSERT_EQ(
            platform::set_raw_value(approved.get(), leftovers::disabled_approval(L"Fabrikam", kEarlier)),
            ERROR_SUCCESS);
    }
    const auto list = [&] {
        StartupEnvironment environment;
        environment.scan.machine = machine.get();
        wchar_t windows[MAX_PATH] = {};
        ::GetWindowsDirectoryW(windows, MAX_PATH);
        environment.scan.machine_folders.windows = platform::normalize_path(windows);
        environment.tasks = std::vector<platform::ScheduledTask>{};
        environment.services = std::vector<platform::ServiceInfo>{};
        return list_startup(environment).entries;
    };
    const auto find = [](const std::vector<StartupEntry>& entries,
                         std::wstring_view name) -> const StartupEntry* {
        for (const auto& entry : entries) {
            if (entry.kind == StartupKind::Run && entry.name == name) {
                return &entry;
            }
        }
        return nullptr;
    };
    const auto store = temporary_store(temp / L"Store", {});
    ASSERT_TRUE(store);
    changes::ExecuteOptions options;
    options.registry_root = [&](Hive hive, const std::wstring&) {
        return hive == Hive::Machine ? machine.get() : nullptr;
    };
    options.protection = [](const Location& location) {
        return leftovers::HardFloor::live().check(location);
    };
    const auto value = [&](const std::wstring& name) -> std::optional<RawValue> {
        HKEY raw = nullptr;
        if (::RegOpenKeyExW(machine.get(), leftovers::approval_key(ApprovalList::Run, L"").path.c_str(), 0,
                            KEY_READ, &raw) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        const wil::unique_hkey key(raw);
        RawValue read;
        return platform::query_raw_value(key.get(), name, read) == ERROR_SUCCESS ? std::optional(read)
                                                                                 : std::nullopt;
    };
    const std::string before = testing::dump_key(machine.get(), L"SOFTWARE");

    // Disable both: Contoso is planned, Fabrikam is disabled already.
    auto entries = list();
    const auto* contoso = find(entries, L"Contoso");
    const auto* fabrikam = find(entries, L"Fabrikam");
    ASSERT_NE(contoso, nullptr);
    ASSERT_NE(fabrikam, nullptr);
    EXPECT_EQ(contoso->state, StartupState::Enabled);
    EXPECT_EQ(fabrikam->state, StartupState::Disabled);
    auto disable = plan_startup(entries, StartupAction::Disable, std::vector{contoso->id, fabrikam->id},
                                ServiceDisable::Manual, kNow);
    ASSERT_EQ(disable.issues.size(), 1u);
    EXPECT_EQ(disable.issues[0].code, Code::AlreadyInState);
    ASSERT_EQ(disable.plan.items.size(), 1u);
    disable.plan.description = L"Disable startup entry Contoso";
    const auto disabled = changes::execute(*store, disable.plan, options);
    ASSERT_FALSE(disabled.set_id.empty());
    EXPECT_EQ(disabled.items[0].status.state, changes::ItemState::Done);
    const auto written = value(L"Contoso");
    ASSERT_TRUE(written);
    EXPECT_EQ(written->type, static_cast<DWORD>(REG_BINARY));
    EXPECT_EQ(written->data, leftovers::disabled_approval(L"Contoso", kNow).data);  // 03 00 00 00 + FILETIME
    entries = list();
    ASSERT_NE(find(entries, L"Contoso"), nullptr);
    EXPECT_EQ(find(entries, L"Contoso")->state, StartupState::Disabled);

    // Enable Fabrikam: 02 and zeros, as Task Manager writes it.
    auto enable = plan_startup(entries, StartupAction::Enable, std::vector{find(entries, L"Fabrikam")->id},
                               ServiceDisable::Manual, kNow);
    ASSERT_TRUE(enable.issues.empty());
    const auto enabled = changes::execute(*store, enable.plan, options);
    ASSERT_FALSE(enabled.set_id.empty());
    EXPECT_EQ(enabled.items[0].status.state, changes::ItemState::Done);
    ASSERT_TRUE(value(L"Fabrikam"));
    EXPECT_EQ(value(L"Fabrikam")->data, leftovers::enabled_approval(L"Fabrikam").data);
    EXPECT_EQ(find(list(), L"Fabrikam")->state, StartupState::Enabled);

    // Undo both, newest first: the registry is exactly as it was.
    for (const auto& id : {enabled.set_id, disabled.set_id}) {
        const auto undone = changes::restore(*store, id, {}, options);
        ASSERT_TRUE(undone.found);
        EXPECT_EQ(undone.state, changes::SetState::Restored);
    }
    EXPECT_EQ(testing::dump_key(machine.get(), L"SOFTWARE"), before);
    EXPECT_FALSE(value(L"Contoso"));
}

// The services Avel disabled, from sets recorded by hand: done items count, restored and skipped
// ones don't, the newest set that changed a service decides, and a set a live run holds is skipped.
TEST(StartupChangesTest, ServicesDisabledByAvelComeFromTheStore) {
    testing::TempFolder temp;
    std::chrono::system_clock::time_point now =
        std::chrono::sys_days{std::chrono::year{2026} / 11 / 2} + std::chrono::hours(12);
    const auto store = temporary_store(temp / L"Store", [&now] { return now; });
    ASSERT_TRUE(store);
    EXPECT_TRUE(services_disabled_by_avel(*store).empty());

    const auto start = [](const wchar_t* name, std::uint32_t before, std::uint32_t after) {
        return ServiceStartLocation{name, {before, false}, {after, false}};
    };
    using State = changes::ItemState;
    const auto record = [&](std::vector<std::pair<Location, State>> items) {
        changes::ChangePlan recorded;
        recorded.description = L"Startup changes";
        for (const auto& [location, state] : items) {
            recorded.items.push_back(
                {location, leftovers::category_of(location), {}, leftovers::Confidence::Unknown, 0});
        }
        auto set = store->create(recorded);
        EXPECT_TRUE(set);
        for (std::size_t i = 0; set && i < items.size(); ++i) {
            EXPECT_TRUE(set->record(i, {State::BackedUp}));
            EXPECT_TRUE(set->record(i, {items[i].second}));
        }
        EXPECT_TRUE(set && set->write_summary(changes::SetState::Complete));
        now += std::chrono::minutes(1);
    };
    record({{start(L"Alpha", SERVICE_AUTO_START, SERVICE_DEMAND_START), State::Done},
            {start(L"Bravo", SERVICE_AUTO_START, SERVICE_DISABLED), State::Done},
            {start(L"Charlie", SERVICE_AUTO_START, SERVICE_DEMAND_START), State::Restored},
            {start(L"Delta", SERVICE_AUTO_START, SERVICE_DEMAND_START), State::Skipped},
            {start(L"Echo", SERVICE_AUTO_START, SERVICE_DEMAND_START), State::RestoreFailed},
            {TaskStateLocation{LR"(\Contoso\Updater)", true, false}, State::Done}});
    // Bravo enabled again later (named in another case); Foxtrot set from Manual to Disabled.
    record({{start(L"bravo", SERVICE_DISABLED, SERVICE_AUTO_START), State::Done},
            {start(L"Foxtrot", SERVICE_DEMAND_START, SERVICE_DISABLED), State::Done}});
    // Alpha's enable was undone: the older disable still holds.
    record({{start(L"Alpha", SERVICE_DEMAND_START, SERVICE_AUTO_START), State::Restored}});
    // A set still running isn't read.
    changes::ChangePlan live_plan;
    live_plan.items.push_back({start(L"Golf", SERVICE_AUTO_START, SERVICE_DEMAND_START),
                               Category::Services,
                               {},
                               leftovers::Confidence::Unknown,
                               0});
    const auto live = store->create(live_plan);
    ASSERT_TRUE(live);
    EXPECT_TRUE(live->record(0, {State::Done}));

    std::vector<std::wstring> services;
    {
        const auto guard = store->lock();
        ASSERT_TRUE(guard);
        services = services_disabled_by_avel(*store);
    }
    std::sort(services.begin(), services.end());
    EXPECT_EQ(services, ids({L"Alpha", L"Echo", L"Foxtrot"}));
}

}  // namespace
}  // namespace avel::tools
