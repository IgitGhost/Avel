// The Startup manager's list against a fixture: Run, RunOnce and RunOnceEx values in scratch keys
// standing in for HKLM and a user's hive, Startup folders in a temporary folder tree, and injected
// tasks and services (registering real ones would change the system). A fake signer stands in for
// the Authenticode check. One test lists this machine, read-only.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "core/inventory/inventory.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/tools/startup.h"
#include "scratch_registry.h"
#include "temp_root.h"

namespace avel::tools {
namespace {

namespace fs = std::filesystem;
using leftovers::FileLocation;
using leftovers::Location;
using leftovers::RegistryKeyLocation;
using leftovers::RegistryValueLocation;
using leftovers::ServiceLocation;
using leftovers::TaskLocation;
using platform::Hive;
using platform::RegistryAddress;

constexpr wchar_t kSid[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kOtherSid[] = L"S-1-5-21-1-2-3-1002";
constexpr wchar_t kRun[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)";
constexpr wchar_t kRun32[] = LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run)";
constexpr wchar_t kRunOnce[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce)";
constexpr wchar_t kRunOnceEx[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnceEx)";
constexpr wchar_t kUserRun[] = LR"(Software\Microsoft\Windows\CurrentVersion\Run)";
constexpr wchar_t kUserRun32[] = LR"(Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Run)";
constexpr wchar_t kUserRunOnce[] = LR"(Software\Microsoft\Windows\CurrentVersion\RunOnce)";
constexpr wchar_t kApproved[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\)";
constexpr wchar_t kUserApproved[] = LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\)";

// Task Manager's StartupApproved bytes: the first one, then zeros (enabled) or a FILETIME (disabled).
std::vector<std::byte> approval_bytes(unsigned first) {
    std::vector<std::byte> bytes(12);
    bytes[0] = std::byte(first);
    if (first & 1) {
        bytes[4] = std::byte{0xb5};
        bytes[11] = std::byte{0x01};
    }
    return bytes;
}

class StartupTest : public ::testing::Test {
protected:
    void SetUp() override {
        com_ = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        root_ = testing::long_temp_directory() /
                (L"avel-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-startup");
        fs::remove_all(root_);
        app_ = path(LR"(PF\Contoso\app.exe)");
        file(LR"(PF\Contoso\app.exe)");
        file(LR"(PF\Contoso\svc.dll)");
        file(LR"(Users\fixture\tool.exe)");
        file(LR"(Microsoft\mssvc.exe)");
        file(LR"(MicrosoftApp\app.exe)");
        fs::create_directories(root_ / L"Gone");  // Holds nothing: gone.exe and gone.ps1 are missing

        wchar_t windows[MAX_PATH] = {};
        ::GetWindowsDirectoryW(windows, MAX_PATH);
        windows_ = platform::normalize_path(windows);
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        if (SUCCEEDED(com_)) {
            ::CoUninitialize();
        }
    }

    std::wstring path(std::wstring_view relative) const {
        return platform::normalize_path((root_ / relative).wstring());
    }

    void file(std::wstring_view relative) const {
        fs::create_directories((root_ / relative).parent_path());
        std::ofstream(root_ / relative) << "fixture";
    }

    void shortcut(std::wstring_view relative, const std::wstring& target,
                  const std::wstring& arguments) const {
        fs::create_directories((root_ / relative).parent_path());
        wil::com_ptr_nothrow<IShellLinkW> link;
        ASSERT_HRESULT_SUCCEEDED(
            ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())));
        ASSERT_HRESULT_SUCCEEDED(link->SetPath(target.c_str()));
        ASSERT_HRESULT_SUCCEEDED(link->SetArguments(arguments.c_str()));
        const auto persist = link.try_query<IPersistFile>();
        ASSERT_TRUE(persist);
        ASSERT_HRESULT_SUCCEEDED(persist->Save((root_ / relative).c_str(), TRUE));
    }

    static void set_binary(const testing::ScratchKey& hive, std::wstring_view key, const std::wstring& name,
                           std::vector<std::byte> bytes) {
        const auto opened = hive.create(key);
        ASSERT_EQ(platform::set_raw_value(opened.get(), {name, REG_BINARY, std::move(bytes)}), ERROR_SUCCESS);
    }

    std::wstring common_startup() const {
        return path(LR"(PD\Microsoft\Windows\Start Menu\Programs\Startup)");
    }
    std::wstring user_startup() const {
        return path(LR"(Users\fixture\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup)");
    }

    StartupEnvironment environment() const {
        StartupEnvironment environment;
        auto& scan = environment.scan;
        scan.machine = machine_.get();
        scan.machine_folders.windows = windows_;
        scan.machine_folders.program_files = path(L"PF");
        scan.machine_folders.program_data = path(L"PD");
        scan.machine_folders.common_startup = common_startup();
        scan.machine_folders.users = path(L"Users");
        scan.machine_folders.all = {scan.machine_folders.program_files, scan.machine_folders.program_data,
                                    scan.machine_folders.users};

        leftovers::UserScope user;
        user.profile = {kSid, L"PC\\fixture", path(LR"(Users\fixture)"), true};
        user.hive = user_.get();
        user.folders = platform::user_folders(user.hive, user.profile);
        scan.users.push_back(std::move(user));
        scan.current_user_sid = kSid;

        environment.tasks = std::vector<platform::ScheduledTask>{};
        environment.services = std::vector<platform::ServiceInfo>{};
        environment.programs = programs_;
        return environment;
    }

    // What is under the Windows folder, and the fixture's "Microsoft" service, are Windows' own; the
    // fixture's "MicrosoftApp" is one of Microsoft's applications (signed through its application
    // root), which isn't.
    SignerFunction fake_signer() const {
        return [windows = windows_, microsoft = path(LR"(Microsoft\mssvc.exe)"),
                application = path(LR"(MicrosoftApp\app.exe)")](const std::wstring& file) {
            StartupSigner signer;
            signer.checked = true;
            signer.valid = true;
            signer.windows = platform::path_is_under(file, windows) || platform::paths_equal(file, microsoft);
            signer.name = signer.windows                             ? L"Microsoft Windows"
                          : platform::paths_equal(file, application) ? L"Microsoft Corporation"
                                                                     : L"Contoso Ltd";
            return signer;
        };
    }

    static const StartupEntry* find(const StartupList& list, StartupKind kind, std::wstring_view name,
                                    std::wstring_view sid = L"") {
        for (const auto& entry : list.entries) {
            if (entry.kind == kind && platform::iequals(entry.name, name) &&
                platform::iequals(entry.user_sid, sid)) {
                return &entry;
            }
        }
        return nullptr;
    }

    static std::size_t count(const StartupList& list, StartupKind kind, std::wstring_view name) {
        return static_cast<std::size_t>(
            std::count_if(list.entries.begin(), list.entries.end(), [&](const StartupEntry& entry) {
                return entry.kind == kind && platform::iequals(entry.name, name);
            }));
    }

    static std::wstring identity_of(const StartupEntry& entry, std::size_t index = 0) {
        return index < entry.delete_locations.size() ? leftovers::identity(entry.delete_locations[index])
                                                     : L"";
    }

    HRESULT com_ = E_FAIL;
    fs::path root_;
    std::wstring app_;
    std::wstring windows_;
    testing::ScratchKey machine_;
    testing::ScratchKey user_;
    std::vector<inventory::Program> programs_;
};

TEST_F(StartupTest, RunAndRunOnceValuesPerScopeAndView) {
    machine_.set_string(kRun, L"Contoso", L"\"" + app_ + L"\" /background");
    machine_.set_string(kRun32, L"Contoso32", app_);
    machine_.set_string(kRunOnce, L"ContosoOnce", app_ + L" /finish");
    user_.set_string(kUserRun, L"UserTool", LR"(%USERPROFILE%\tool.exe --tray)", REG_EXPAND_SZ);
    user_.set_string(kUserRun32, L"UserTool32", LR"(%USERPROFILE%\tool.exe)");
    user_.set_string(kUserRunOnce, L"UserOnce", app_);
    machine_.set_string(kRun, L"Empty", L"  ");  // Runs nothing: not listed
    machine_.set_dword(kRun, L"Number", 1);      // Not a command line: not listed

    const auto list = list_startup(environment());
    EXPECT_TRUE(list.diagnostics.empty());

    const auto* run = find(list, StartupKind::Run, L"Contoso");
    ASSERT_NE(run, nullptr);
    EXPECT_TRUE(run->user_sid.empty());
    EXPECT_FALSE(run->wow64);
    EXPECT_EQ(run->command, L"\"" + app_ + L"\" /background");
    EXPECT_EQ(run->value_type, static_cast<std::uint32_t>(REG_SZ));
    EXPECT_EQ(run->target, app_);
    EXPECT_FALSE(run->target_missing);
    EXPECT_EQ(run->state, StartupState::Enabled);
    ASSERT_EQ(run->delete_locations.size(), 1u);
    EXPECT_EQ(identity_of(*run),
              leftovers::identity(RegistryValueLocation{{Hive::Machine, L"", kRun}, L"Contoso"}));

    const auto* run32 = find(list, StartupKind::Run, L"Contoso32");
    ASSERT_NE(run32, nullptr);
    EXPECT_TRUE(run32->wow64);
    EXPECT_EQ(run32->target, app_);
    EXPECT_EQ(identity_of(*run32),
              leftovers::identity(RegistryValueLocation{{Hive::Machine, L"", kRun32}, L"Contoso32"}));

    const auto* once = find(list, StartupKind::RunOnce, L"ContosoOnce");
    ASSERT_NE(once, nullptr);
    EXPECT_FALSE(once->approval);  // RunOnce has no StartupApproved list: delete only

    const auto* user = find(list, StartupKind::Run, L"UserTool", kSid);
    ASSERT_NE(user, nullptr);
    EXPECT_EQ(user->user_name, L"PC\\fixture");
    EXPECT_EQ(user->command, LR"(%USERPROFILE%\tool.exe --tray)");  // As written
    EXPECT_EQ(user->value_type, static_cast<std::uint32_t>(REG_EXPAND_SZ));
    EXPECT_EQ(user->target, path(LR"(Users\fixture\tool.exe)"));  // Expanded for its user
    EXPECT_EQ(identity_of(*user),
              leftovers::identity(RegistryValueLocation{{Hive::User, kSid, kUserRun}, L"UserTool"}));

    const auto* user32 = find(list, StartupKind::Run, L"UserTool32", kSid);
    ASSERT_NE(user32, nullptr);
    EXPECT_TRUE(user32->wow64);
    EXPECT_FALSE(user32->approval);
    EXPECT_NE(find(list, StartupKind::RunOnce, L"UserOnce", kSid), nullptr);

    EXPECT_EQ(count(list, StartupKind::Run, L"Empty"), 0u);
    EXPECT_EQ(count(list, StartupKind::Run, L"Number"), 0u);
}

TEST_F(StartupTest, StartupApprovedLowBitMeansDisabled) {
    const std::wstring approved = std::wstring(kApproved) + L"Run";
    for (const auto* name : {L"Two", L"Three", L"One", L"Six", L"Absent"}) {
        machine_.set_string(kRun, name, app_);
    }
    set_binary(machine_, approved, L"Two", approval_bytes(0x02));
    set_binary(machine_, approved, L"Three", approval_bytes(0x03));
    set_binary(machine_, approved, L"One", approval_bytes(0x01));
    set_binary(machine_, approved, L"Six", approval_bytes(0x06));
    set_binary(machine_, approved, L"Unrelated", approval_bytes(0x03));

    const auto list = list_startup(environment());
    const auto state = [&](std::wstring_view name) {
        const auto* entry = find(list, StartupKind::Run, name);
        return entry ? entry->state : StartupState::Enabled;
    };
    EXPECT_EQ(state(L"Two"), StartupState::Enabled);
    EXPECT_EQ(state(L"Three"), StartupState::Disabled);
    EXPECT_EQ(state(L"One"), StartupState::Disabled);
    EXPECT_EQ(state(L"Six"), StartupState::Enabled);
    EXPECT_EQ(state(L"Absent"), StartupState::Enabled);

    const auto* three = find(list, StartupKind::Run, L"Three");
    ASSERT_NE(three, nullptr);
    ASSERT_TRUE(three->approval);
    EXPECT_TRUE(platform::same_address(three->approval->key, {Hive::Machine, L"", approved}));
    EXPECT_EQ(three->approval->value_name, L"Three");
    ASSERT_TRUE(three->approval->value);
    EXPECT_EQ(three->approval->value->type, static_cast<DWORD>(REG_BINARY));
    EXPECT_EQ(three->approval->value->data, approval_bytes(0x03));
    // Deleting the entry takes its StartupApproved value along.
    ASSERT_EQ(three->delete_locations.size(), 2u);
    EXPECT_EQ(identity_of(*three, 1),
              leftovers::identity(RegistryValueLocation{{Hive::Machine, L"", approved}, L"Three"}));

    const auto* absent = find(list, StartupKind::Run, L"Absent");
    ASSERT_NE(absent, nullptr);
    ASSERT_TRUE(absent->approval);  // Where Task Manager would write it
    EXPECT_FALSE(absent->approval->value);
    EXPECT_EQ(absent->delete_locations.size(), 1u);
}

TEST_F(StartupTest, ThirtyTwoBitRunValuesPairWithRun32OfTheMachineOnly) {
    machine_.set_string(kRun32, L"Tool", app_);
    machine_.set_string(kRun, L"Tool", app_);
    set_binary(machine_, std::wstring(kApproved) + L"Run32", L"Tool", approval_bytes(0x03));
    set_binary(machine_, std::wstring(kApproved) + L"Run", L"Tool", approval_bytes(0x02));
    // A user's 32-bit Run key: Windows keeps no state for it, a Run32 list there is ignored.
    user_.set_string(kUserRun32, L"Tool", app_);
    set_binary(user_, std::wstring(kUserApproved) + L"Run32", L"Tool", approval_bytes(0x03));

    const auto list = list_startup(environment());
    const StartupEntry* machine64 = nullptr;
    const StartupEntry* machine32 = nullptr;
    for (const auto& entry : list.entries) {
        if (entry.kind == StartupKind::Run && entry.name == L"Tool" && entry.user_sid.empty()) {
            (entry.wow64 ? machine32 : machine64) = &entry;
        }
    }
    ASSERT_NE(machine64, nullptr);
    ASSERT_NE(machine32, nullptr);
    ASSERT_TRUE(machine64->approval);
    EXPECT_EQ(machine64->state, StartupState::Enabled);
    EXPECT_TRUE(platform::iends_with(machine64->approval->key.path, LR"(\StartupApproved\Run)"));
    EXPECT_EQ(machine32->state, StartupState::Disabled);
    ASSERT_TRUE(machine32->approval);
    EXPECT_TRUE(platform::iends_with(machine32->approval->key.path, LR"(\StartupApproved\Run32)"));
    EXPECT_NE(machine64->id, machine32->id);

    const auto* user = find(list, StartupKind::Run, L"Tool", kSid);
    ASSERT_NE(user, nullptr);
    EXPECT_TRUE(user->wow64);
    EXPECT_EQ(user->state, StartupState::Enabled);
    EXPECT_FALSE(user->approval);
}

TEST_F(StartupTest, StartupFolderShortcutsAndFilesMatchedByFileName) {
    shortcut(LR"(PD\Microsoft\Windows\Start Menu\Programs\Startup\Contoso.lnk)", app_, L"/tray --quiet");
    file(LR"(PD\Microsoft\Windows\Start Menu\Programs\Startup\script.cmd)");
    file(LR"(PD\Microsoft\Windows\Start Menu\Programs\Startup\desktop.ini)");    // Not an entry
    file(LR"(PD\Microsoft\Windows\Start Menu\Programs\Startup\Sub\inner.cmd)");  // Windows doesn't run it
    shortcut(LR"(Users\fixture\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup\Tool.lnk)",
             path(LR"(Users\fixture\tool.exe)"), L"");
    set_binary(machine_, std::wstring(kApproved) + L"StartupFolder", L"Contoso.lnk", approval_bytes(0x03));
    set_binary(user_, std::wstring(kUserApproved) + L"StartupFolder", L"Tool.lnk", approval_bytes(0x02));
    // The user's list doesn't apply to the common folder, nor the machine's to the user's.
    set_binary(user_, std::wstring(kUserApproved) + L"StartupFolder", L"script.cmd", approval_bytes(0x03));

    const auto list = list_startup(environment());
    EXPECT_EQ(count(list, StartupKind::StartupFolder, L"desktop.ini"), 0u);
    EXPECT_EQ(count(list, StartupKind::StartupFolder, L"inner.cmd"), 0u);

    const auto* contoso = find(list, StartupKind::StartupFolder, L"Contoso.lnk");
    ASSERT_NE(contoso, nullptr);
    EXPECT_TRUE(contoso->user_sid.empty());
    EXPECT_EQ(contoso->command, platform::exec_command(app_, L"/tray --quiet"));
    EXPECT_EQ(contoso->target, app_);
    EXPECT_EQ(contoso->state, StartupState::Disabled);
    ASSERT_TRUE(contoso->approval);
    EXPECT_TRUE(platform::same_address(contoso->approval->key,
                                       {Hive::Machine, L"", std::wstring(kApproved) + L"StartupFolder"}));
    EXPECT_EQ(identity_of(*contoso),
              leftovers::identity(FileLocation{common_startup() + LR"(\Contoso.lnk)", false}));
    EXPECT_EQ(contoso->delete_locations.size(), 2u);

    const auto* script = find(list, StartupKind::StartupFolder, L"script.cmd");
    ASSERT_NE(script, nullptr);
    EXPECT_EQ(script->target, common_startup() + LR"(\script.cmd)");
    EXPECT_EQ(script->state, StartupState::Enabled);

    const auto* tool = find(list, StartupKind::StartupFolder, L"Tool.lnk", kSid);
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->target, path(LR"(Users\fixture\tool.exe)"));
    EXPECT_EQ(tool->state, StartupState::Enabled);
    ASSERT_TRUE(tool->approval);
    EXPECT_TRUE(tool->approval->value);
    EXPECT_TRUE(platform::same_address(tool->approval->key,
                                       {Hive::User, kSid, std::wstring(kUserApproved) + L"StartupFolder"}));
    EXPECT_EQ(identity_of(*tool), leftovers::identity(FileLocation{user_startup() + LR"(\Tool.lnk)", false}));
}

TEST_F(StartupTest, RunOnceExSubkeysAreEntries) {
    machine_.set_string(std::wstring(kRunOnceEx) + LR"(\0001)", L"", L"Finishing Contoso setup");
    machine_.set_string(std::wstring(kRunOnceEx) + LR"(\0001)", L"1", L"||\"" + app_ + L"\" /finish");
    machine_.set_string(std::wstring(kRunOnceEx) + LR"(\0002)", L"1",
                        path(LR"(Gone\gone.dll)") + L"|DllRegisterServer");
    machine_.set_string(kRunOnceEx, L"Title", L"Contoso");  // The key's own values aren't entries

    const auto list = list_startup(environment());
    const auto* first = find(list, StartupKind::RunOnceEx, L"0001");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->command, L"||\"" + app_ + L"\" /finish");
    EXPECT_EQ(first->target, app_);
    EXPECT_FALSE(first->approval);
    ASSERT_EQ(first->delete_locations.size(), 1u);
    EXPECT_EQ(identity_of(*first), leftovers::identity(RegistryKeyLocation{
                                       {Hive::Machine, L"", std::wstring(kRunOnceEx) + LR"(\0001)"}}));

    const auto* second = find(list, StartupKind::RunOnceEx, L"0002");
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->target, path(LR"(Gone\gone.dll)"));
    EXPECT_TRUE(second->target_missing);
    EXPECT_EQ(std::count_if(list.entries.begin(), list.entries.end(),
                            [](const StartupEntry& entry) { return entry.kind == StartupKind::RunOnceEx; }),
              2);
}

TEST_F(StartupTest, MissingTargetsAndHostedFiles) {
    machine_.set_string(kRun, L"Gone", L"\"" + path(LR"(Gone\gone.exe)") + L"\" /x");
    machine_.set_string(kRun, L"Script",
                        L"powershell.exe -NoProfile -File \"" + path(LR"(Gone\gone.ps1)") + L"\"");
    machine_.set_string(kRun, L"Library", L"rundll32.exe \"" + path(LR"(PF\Contoso\svc.dll)") + L"\",Start");
    machine_.set_string(kRun, L"Present", app_);

    const auto list = list_startup(environment());
    const auto* gone = find(list, StartupKind::Run, L"Gone");
    ASSERT_NE(gone, nullptr);
    EXPECT_EQ(gone->target, path(LR"(Gone\gone.exe)"));
    EXPECT_TRUE(gone->target_missing);

    const auto* script = find(list, StartupKind::Run, L"Script");
    ASSERT_NE(script, nullptr);
    EXPECT_EQ(script->target, path(LR"(Gone\gone.ps1)"));  // The script, not powershell.exe
    EXPECT_TRUE(script->target_missing);

    const auto* library = find(list, StartupKind::Run, L"Library");
    ASSERT_NE(library, nullptr);
    EXPECT_EQ(library->target, path(LR"(PF\Contoso\svc.dll)"));
    EXPECT_FALSE(library->target_missing);

    const auto* present = find(list, StartupKind::Run, L"Present");
    ASSERT_NE(present, nullptr);
    EXPECT_FALSE(present->target_missing);
}

TEST_F(StartupTest, OwnerIsTheProgramWhoseFolderHoldsTheTarget) {
    inventory::Program contoso;
    contoso.id = L"reg:HKLM64:Contoso";
    contoso.display_name = L"Contoso";
    contoso.install_location = path(LR"(PF\Contoso)");
    programs_ = {contoso};
    machine_.set_string(kRun, L"Contoso", app_);
    machine_.set_string(kRun, L"Other", path(LR"(Users\fixture\tool.exe)"));

    const auto list = list_startup(environment());
    const auto* owned = find(list, StartupKind::Run, L"Contoso");
    ASSERT_NE(owned, nullptr);
    EXPECT_EQ(owned->program_id, L"reg:HKLM64:Contoso");
    const auto* other = find(list, StartupKind::Run, L"Other");
    ASSERT_NE(other, nullptr);
    EXPECT_TRUE(other->program_id.empty());
}

TEST_F(StartupTest, TasksWithALogonOrBootTriggerOutsideMicrosoft) {
    using platform::TaskTrigger;
    auto env = environment();
    env.tasks = std::vector<platform::ScheduledTask>{
        {LR"(\Contoso\At logon)",
         {L"\"" + app_ + L"\" /update"},
         false,
         {TaskTrigger::Other, TaskTrigger::Logon},
         true,
         L"Contoso"},
        {LR"(\Contoso\At boot)", {app_}, false, {TaskTrigger::Boot}, false, L""},
        {LR"(\Contoso\Daily)", {app_}, false, {TaskTrigger::Other}, true, L""},
        {LR"(\Contoso\No triggers)", {app_}, false, {}, true, L""},
        {LR"(\Microsoft\Windows\Contoso)", {app_}, false, {TaskTrigger::Logon}, true, L""},
        {LR"(\Contoso\Handler)", {}, true, {TaskTrigger::Logon}, true, L""},
    };
    const auto list = list_startup(env);
    const auto tasks = std::count_if(list.entries.begin(), list.entries.end(), [](const StartupEntry& entry) {
        return entry.kind == StartupKind::Task;
    });
    EXPECT_EQ(tasks, 3);

    const auto* logon = find(list, StartupKind::Task, LR"(\Contoso\At logon)");
    ASSERT_NE(logon, nullptr);
    EXPECT_EQ(logon->command, L"\"" + app_ + L"\" /update");
    EXPECT_EQ(logon->target, app_);
    EXPECT_TRUE(logon->task_enabled);
    EXPECT_EQ(logon->state, StartupState::Enabled);
    EXPECT_EQ(identity_of(*logon), leftovers::identity(TaskLocation{LR"(\Contoso\At logon)", false}));

    const auto* boot = find(list, StartupKind::Task, LR"(\Contoso\At boot)");
    ASSERT_NE(boot, nullptr);
    EXPECT_FALSE(boot->task_enabled);
    EXPECT_EQ(boot->state, StartupState::Disabled);

    // Only COM handlers: listed, with nothing to run.
    const auto* handler = find(list, StartupKind::Task, LR"(\Contoso\Handler)");
    ASSERT_NE(handler, nullptr);
    EXPECT_TRUE(handler->command.empty());
    EXPECT_TRUE(handler->target.empty());
    EXPECT_FALSE(handler->target_missing);
}

TEST_F(StartupTest, AutomaticServicesNotWindowsOwnAndTheOnesAvelDisabled) {
    const auto service = [](std::wstring name, std::uint32_t type, std::uint32_t start, std::wstring image,
                            std::wstring dll = L"", bool delayed = false) {
        platform::ServiceInfo info;
        info.name = std::move(name);
        info.display_name = L"Display " + info.name;
        info.type = type;
        info.start_type = start;
        info.delayed = delayed;
        info.image_path = std::move(image);
        info.service_dll = std::move(dll);
        return info;
    };
    const std::wstring image = L"\"" + app_ + L"\" -service";
    const std::wstring microsoft = path(LR"(Microsoft\mssvc.exe)");
    const std::wstring microsoft_app = path(LR"(MicrosoftApp\app.exe)");
    auto env = environment();
    env.signer = fake_signer();
    env.services = std::vector<platform::ServiceInfo>{
        service(L"ContosoAuto", SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, image),
        service(L"ContosoDelayed", SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, image, L"", true),
        service(L"ContosoManual", SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, image),
        service(L"ContosoDriver", SERVICE_KERNEL_DRIVER, SERVICE_AUTO_START,
                path(LR"(PF\Contoso\driver.sys)")),
        service(L"ContosoHosted", SERVICE_WIN32_SHARE_PROCESS, SERVICE_AUTO_START,
                LR"(%SystemRoot%\System32\svchost.exe -k ContosoGroup -p)", path(LR"(PF\Contoso\svc.dll)")),
        service(L"MicrosoftAuto", SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, microsoft),
        service(L"MicrosoftAppAuto", SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, microsoft_app),
        service(L"AvelManual", SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, image),
        service(L"AvelDisabled", SERVICE_WIN32_OWN_PROCESS, SERVICE_DISABLED, image),
        service(L"AvelAuto", SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, image),
        service(L"MicrosoftAvel", SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, microsoft),
    };
    env.avel_disabled_services = {L"avelmanual", L"AvelDisabled", L"AvelAuto", L"MicrosoftAvel"};
    const auto list = list_startup(env);

    const auto* automatic = find(list, StartupKind::Service, L"ContosoAuto");
    ASSERT_NE(automatic, nullptr);
    EXPECT_EQ(automatic->display_name, L"Display ContosoAuto");
    EXPECT_EQ(automatic->command, image);
    EXPECT_EQ(automatic->target, app_);
    EXPECT_EQ(automatic->service_start, static_cast<std::uint32_t>(SERVICE_AUTO_START));
    EXPECT_FALSE(automatic->service_delayed);
    EXPECT_EQ(automatic->state, StartupState::Enabled);
    EXPECT_FALSE(automatic->disabled_by_avel);
    EXPECT_TRUE(automatic->signer.checked);
    EXPECT_EQ(automatic->signer.name, L"Contoso Ltd");
    EXPECT_EQ(identity_of(*automatic), leftovers::identity(ServiceLocation{L"ContosoAuto", false}));

    const auto* delayed = find(list, StartupKind::Service, L"ContosoDelayed");
    ASSERT_NE(delayed, nullptr);
    EXPECT_TRUE(delayed->service_delayed);

    const auto* hosted = find(list, StartupKind::Service, L"ContosoHosted");
    ASSERT_NE(hosted, nullptr);
    EXPECT_EQ(hosted->target, path(LR"(PF\Contoso\svc.dll)"));  // The DLL, not svchost.exe

    EXPECT_EQ(find(list, StartupKind::Service, L"ContosoManual"), nullptr);
    EXPECT_EQ(find(list, StartupKind::Service, L"ContosoDriver"), nullptr);
    EXPECT_EQ(find(list, StartupKind::Service, L"MicrosoftAuto"), nullptr);
    // Microsoft's applications aren't Windows' own: listed, and not protected.
    const auto* application = find(list, StartupKind::Service, L"MicrosoftAppAuto");
    ASSERT_NE(application, nullptr);
    EXPECT_EQ(application->signer.name, L"Microsoft Corporation");
    EXPECT_FALSE(application->signer.windows);
    EXPECT_EQ(application->protection, StartupProtection::None);

    for (const auto* name : {L"AvelManual", L"AvelDisabled"}) {
        const auto* disabled = find(list, StartupKind::Service, name);
        ASSERT_NE(disabled, nullptr) << platform::to_utf8(name);
        EXPECT_EQ(disabled->state, StartupState::Disabled);
        EXPECT_TRUE(disabled->disabled_by_avel);
    }
    const auto* still_automatic = find(list, StartupKind::Service, L"AvelAuto");
    ASSERT_NE(still_automatic, nullptr);
    EXPECT_EQ(still_automatic->state, StartupState::Enabled);
    EXPECT_FALSE(still_automatic->disabled_by_avel);
    EXPECT_NE(find(list, StartupKind::Service, L"MicrosoftAvel"), nullptr);
}

TEST_F(StartupTest, WithoutASignerNothingIsWindowsOwn) {
    auto env = environment();
    env.services =
        std::vector<platform::ServiceInfo>{{L"MicrosoftAuto", L"", SERVICE_WIN32_OWN_PROCESS,
                                            SERVICE_AUTO_START, false, path(LR"(Microsoft\mssvc.exe)"), L""}};
    machine_.set_string(kRun, L"WindowsTool", LR"(%windir%\System32\svchost.exe)", REG_EXPAND_SZ);
    const auto list = list_startup(env);
    const auto* service = find(list, StartupKind::Service, L"MicrosoftAuto");
    ASSERT_NE(service, nullptr);
    EXPECT_FALSE(service->signer.checked);
    const auto* tool = find(list, StartupKind::Run, L"WindowsTool");
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->protection, StartupProtection::None);
}

TEST_F(StartupTest, WindowsOwnEntriesAreProtected) {
    machine_.set_string(kRun, L"SecurityHealth", LR"(%windir%\System32\svchost.exe)", REG_EXPAND_SZ);
    machine_.set_string(kRun, L"Shell",
                        L"cmd.exe /c start \"\" \"" + app_ + L"\"");  // Microsoft's host, not Windows' entry
    machine_.set_string(kRun, L"Contoso", app_);
    auto env = environment();
    env.signer = fake_signer();
    const auto list = list_startup(env);

    const auto* windows = find(list, StartupKind::Run, L"SecurityHealth");
    ASSERT_NE(windows, nullptr);
    EXPECT_TRUE(platform::paths_equal(windows->target, windows_ + LR"(\System32\svchost.exe)"));
    EXPECT_TRUE(windows->signer.windows);
    EXPECT_EQ(windows->protection, StartupProtection::WindowsOwn);

    const auto* shell = find(list, StartupKind::Run, L"Shell");
    ASSERT_NE(shell, nullptr);
    EXPECT_TRUE(platform::paths_equal(shell->target, windows_ + LR"(\System32\cmd.exe)"));
    EXPECT_EQ(shell->protection, StartupProtection::None);

    const auto* contoso = find(list, StartupKind::Run, L"Contoso");
    ASSERT_NE(contoso, nullptr);
    EXPECT_EQ(contoso->protection, StartupProtection::None);
}

TEST_F(StartupTest, IdsAreStableAndDistinct) {
    machine_.set_string(kRun, L"Contoso", app_);
    machine_.set_string(kRun32, L"Contoso", app_);
    machine_.set_string(kRunOnce, L"Contoso", app_);
    user_.set_string(kUserRun, L"Contoso", app_);
    shortcut(LR"(PD\Microsoft\Windows\Start Menu\Programs\Startup\Contoso.lnk)", app_, L"");
    auto env = environment();
    env.tasks = std::vector<platform::ScheduledTask>{
        {LR"(\Contoso)", {app_}, false, {platform::TaskTrigger::Logon}, true, L""}};
    env.services = std::vector<platform::ServiceInfo>{
        {L"Contoso", L"Contoso", SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, false, app_, L""}};

    const auto first = list_startup(env);
    const auto second = list_startup(env);
    ASSERT_EQ(first.entries.size(), 7u);
    ASSERT_EQ(second.entries.size(), first.entries.size());
    std::set<std::wstring> ids;
    for (std::size_t i = 0; i < first.entries.size(); ++i) {
        const auto& entry = first.entries[i];
        EXPECT_EQ(entry.id, second.entries[i].id);
        EXPECT_EQ(entry.id.size(), 10u);
        EXPECT_EQ(entry.id, startup_id(entry.delete_locations.front()));
        ids.insert(entry.id);
    }
    EXPECT_EQ(ids.size(), first.entries.size());
    // Case doesn't change an id: identities are case-insensitive.
    EXPECT_EQ(startup_id(TaskLocation{LR"(\Contoso)", false}),
              startup_id(TaskLocation{LR"(\CONTOSO)", false}));
}

TEST_F(StartupTest, AUserWithoutAHiveStillHasTheirStartupFolder) {
    auto env = environment();
    leftovers::UserScope other;
    other.profile = {kOtherSid, L"PC\\other", path(LR"(Users\other)"), false};
    other.folders = platform::user_folders(nullptr, other.profile);
    env.scan.users.push_back(std::move(other));
    file(LR"(Users\other\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup\run.cmd)");

    const auto list = list_startup(env);
    const auto* entry = find(list, StartupKind::StartupFolder, L"run.cmd", kOtherSid);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->user_name, L"PC\\other");
    EXPECT_EQ(entry->state, StartupState::Enabled);
    ASSERT_TRUE(entry->approval);
    EXPECT_FALSE(entry->approval->value);
}

// Lists this machine (read-only) and checks what holds on any machine. Set
// AVEL_TEST_PRINT_STARTUP=1 to print the list.
TEST(StartupLiveTest, ListsThisMachine) {
    const bool print = ::GetEnvironmentVariableW(L"AVEL_TEST_PRINT_STARTUP", nullptr, 0) > 0;
    // The owners are worth seeing in the printout; the inventory takes a few seconds.
    const auto programs = print ? inventory::scan_inventory().programs : std::vector<inventory::Program>{};
    const auto started = std::chrono::steady_clock::now();
    StartupList list;
    {
        const leftovers::LiveEnvironment live;
        StartupEnvironment environment;
        environment.scan = live.environment();
        environment.programs = programs;
        list = list_startup(environment);
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    std::cout << list.entries.size() << " startup entries in " << elapsed.count() << " ms\n";

    std::set<std::wstring> ids;
    for (const auto& entry : list.entries) {
        EXPECT_FALSE(entry.delete_locations.empty());
        EXPECT_FALSE(entry.name.empty());
        EXPECT_TRUE(ids.insert(entry.id).second) << platform::to_utf8(entry.name);
        if (entry.kind == StartupKind::Task) {
            EXPECT_FALSE(platform::is_windows_task(entry.name));
        }
    }
    if (print) {
        for (const auto& diagnostic : list.diagnostics) {
            std::cout << "diagnostic " << to_string(diagnostic.code) << " " << diagnostic.error << "\n";
        }
        for (const auto& entry : list.entries) {
            std::cout << platform::to_utf8(entry.id) << " " << to_string(entry.kind)
                      << (entry.wow64 ? "/32" : "") << " "
                      << (entry.user_sid.empty() ? "machine" : platform::to_utf8(entry.user_name)) << " "
                      << to_string(entry.state) << (entry.disabled_by_avel ? "(avel)" : "") << " "
                      << to_string(entry.protection) << " [" << platform::to_utf8(entry.name) << "] -> "
                      << platform::to_utf8(entry.target) << (entry.target_missing ? " (missing)" : "")
                      << " | " << platform::to_utf8(entry.product.product_name) << " / "
                      << platform::to_utf8(entry.product.company_name) << " | program "
                      << platform::to_utf8(entry.program_id) << " | approval "
                      << (entry.approval && entry.approval->value
                              ? std::to_string(
                                    std::to_integer<int>(entry.approval->value->data.empty()
                                                             ? std::byte{0}
                                                             : entry.approval->value->data.front()))
                              : std::string(entry.approval ? "none" : "-"))
                      << "\n";
        }
    }
}

}  // namespace
}  // namespace avel::tools
