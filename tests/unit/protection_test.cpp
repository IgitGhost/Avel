#include <gtest/gtest.h>

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

#include "core/leftovers/protection.h"
#include "core/platform/known_folders.h"
#include "core/platform/text.h"
#include "core/platform/user_profiles.h"

namespace avel::leftovers {
namespace {

using platform::Hive;
using platform::RegistryAddress;
using Reason = ProtectionReason;

Location folder(std::wstring_view path) {
    return FileLocation{std::wstring(path), true};
}
Location key(Hive hive, std::wstring_view path) {
    return RegistryKeyLocation{{hive, hive == Hive::User ? L"S-1-5-21-1-2-3-1001" : L"", std::wstring(path)}};
}
Location value(Hive hive, std::wstring_view path, std::wstring_view name) {
    return RegistryValueLocation{{hive, hive == Hive::User ? L"S-1-5-21-1-2-3-1001" : L"", std::wstring(path)},
                                 std::wstring(name)};
}

HardFloor fixture_floor() {
    return HardFloor({LR"(C:\Windows)",
                      {{LR"(C:\Program Files)", Reason::ProgramFiles},
                       {LR"(C:\ProgramData)", Reason::ProgramData},
                       {LR"(C:\Users)", Reason::UserProfiles},
                       {LR"(C:\Users\me)", Reason::UserProfiles}},
                      {{LR"(C:\Program Files\WindowsApps)", Reason::SystemArea},
                       {LR"(C:\Users\me\OneDrive)", Reason::CloudFolder}}});
}

struct FileCase {
    std::wstring_view path;
    Reason reason;
};

constexpr FileCase kFloorFiles[] = {
    {LR"(C:\)", Reason::DriveRoot},
    {LR"(\\server\share)", Reason::DriveRoot},
    {LR"(relative\path)", Reason::InvalidPath},
    {LR"()", Reason::InvalidPath},
    {LR"(C:\Windows\System32\contoso.dll)", Reason::WindowsFolder},
    {LR"(C:\Users\me\..\..\Windows)", Reason::WindowsFolder},
    {LR"(C:\Program Files)", Reason::ProgramFiles},
    {LR"(C:\Program Files\Contoso)", Reason::None},
    {LR"(C:\ProgramData)", Reason::ProgramData},
    {LR"(C:\ProgramData\Contoso)", Reason::None},
    {LR"(C:\Users)", Reason::UserProfiles},
    {LR"(C:\Users\me)", Reason::UserProfiles},
    {LR"(C:\Users\me\AppData\Roaming\Contoso)", Reason::None},
    {LR"(C:\Program Files\WindowsApps\Contoso_1.0_x64__abc)", Reason::SystemArea},
    {LR"(C:\Users\me\OneDrive\Documents\Contoso)", Reason::CloudFolder},
    {LR"(D:\System Volume Information\x)", Reason::SystemArea},
    {LR"(D:\$Recycle.Bin)", Reason::SystemArea},
    {LR"(D:\Games\Contoso)", Reason::None},
};

TEST(HardFloorTest, GuardsSystemFoldersProfilesAndTheirAncestors) {
    const HardFloor floor = fixture_floor();
    for (const auto& item : kFloorFiles) {
        EXPECT_EQ(floor.check(folder(item.path)), item.reason) << platform::to_utf8(item.path);
    }
}

TEST(HardFloorTest, GuardsCoreAndSystemRegistryKeys) {
    const HardFloor floor = fixture_floor();
    EXPECT_EQ(floor.check(key(Hive::Machine, LR"(SYSTEM\CurrentControlSet\Services\Contoso)")), Reason::SystemRegistry);
    EXPECT_EQ(floor.check(value(Hive::Machine, LR"(SYSTEM\CurrentControlSet\Services\Contoso)", L"ImagePath")),
              Reason::SystemRegistry);
    EXPECT_EQ(floor.check(key(Hive::Machine, L"SOFTWARE")), Reason::CoreRegistryKey);
    EXPECT_EQ(floor.check(key(Hive::Machine, LR"(SOFTWARE\Microsoft)")), Reason::CoreRegistryKey);
    EXPECT_EQ(floor.check(key(Hive::Machine, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall)")),
              Reason::CoreRegistryKey);
    EXPECT_EQ(floor.check(key(Hive::Machine, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso)")),
              Reason::None);
    EXPECT_EQ(floor.check(key(Hive::Machine, LR"(SOFTWARE\WOW6432Node)")), Reason::CoreRegistryKey);
    EXPECT_EQ(floor.check(key(Hive::Machine, LR"(SOFTWARE\WOW6432Node\Contoso)")), Reason::None);
    EXPECT_EQ(floor.check(key(Hive::Machine, LR"(SOFTWARE\Classes)")), Reason::CoreRegistryKey);
    EXPECT_EQ(floor.check(key(Hive::Machine, LR"(SOFTWARE\Classes\WOW6432Node\CLSID)")), Reason::CoreRegistryKey);
    EXPECT_EQ(floor.check(key(Hive::Machine, LR"(SOFTWARE\Classes\CLSID\{A7E1A7E1-0000-4000-8000-000000000001})")),
              Reason::None);
    EXPECT_EQ(floor.check(value(Hive::Machine, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)", L"Contoso")),
              Reason::None);
    EXPECT_EQ(floor.check(key(Hive::Machine, LR"(SOFTWARE\Avel\Settings)")), Reason::AvelData);
    EXPECT_EQ(floor.check(key(Hive::User, L"Software")), Reason::CoreRegistryKey);
    EXPECT_EQ(floor.check(key(Hive::User, L"Environment")), Reason::CoreRegistryKey);
    EXPECT_EQ(floor.check(key(Hive::User, LR"(Software\Contoso)")), Reason::None);
    EXPECT_EQ(floor.check(key(Hive::User, LR"(Software\Microsoft\Windows\CurrentVersion\Run)")), Reason::CoreRegistryKey);
}

TEST(HardFloorTest, GuardsWindowsInstallersContainersButNotOneProductsKeys) {
    const HardFloor floor = fixture_floor();
    constexpr wchar_t kInstaller[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Installer)";
    const auto machine = [&](std::wstring path) { return floor.check(key(Hive::Machine, path)); };
    for (const auto* path :
         {LR"(SOFTWARE\Classes\Installer)", LR"(SOFTWARE\Classes\Installer\Products)", LR"(SOFTWARE\Classes\Installer\Features)",
          LR"(SOFTWARE\Classes\Installer\UpgradeCodes)", LR"(SOFTWARE\Classes\Installer\Patches)",
          LR"(SOFTWARE\Classes\Installer\Dependencies)"}) {
        EXPECT_EQ(machine(path), Reason::CoreRegistryKey) << platform::to_utf8(path);
    }
    for (const auto* suffix : {L"", LR"(\UserData)", LR"(\UserData\S-1-5-18)", LR"(\UserData\S-1-5-18\Products)",
                               LR"(\UserData\S-1-5-18\Components)", LR"(\UserData\S-1-5-21-1-2-3-1001\Products)",
                               LR"(\Managed)", LR"(\Managed\S-1-5-21-1-2-3-1001\Installer\Products)", LR"(\Folders)",
                               LR"(\Secure)"}) {
        EXPECT_EQ(machine(kInstaller + std::wstring(suffix)), Reason::CoreRegistryKey) << platform::to_utf8(suffix);
    }
    // One product's keys may go.
    constexpr wchar_t kProduct[] = L"96F071321C0420726200000010000000";
    EXPECT_EQ(machine(LR"(SOFTWARE\Classes\Installer\Products\)" + std::wstring(kProduct)), Reason::None);
    EXPECT_EQ(machine(LR"(SOFTWARE\Classes\Installer\UpgradeCodes\)" + std::wstring(kProduct)), Reason::None);
    EXPECT_EQ(machine(kInstaller + std::wstring(LR"(\UserData\S-1-5-18\Products\)") + kProduct), Reason::None);
    EXPECT_EQ(machine(kInstaller + std::wstring(LR"(\UserData\S-1-5-18\Components\)") + kProduct), Reason::None);
    EXPECT_EQ(machine(kInstaller + std::wstring(LR"(\Managed\S-1-5-21-1-2-3-1001\Installer\Features\)") + kProduct),
              Reason::None);
    // A user's own registration.
    EXPECT_EQ(floor.check(key(Hive::User, LR"(Software\Microsoft\Installer)")), Reason::CoreRegistryKey);
    EXPECT_EQ(floor.check(key(Hive::User, LR"(Software\Microsoft\Installer\Products)")), Reason::CoreRegistryKey);
    EXPECT_EQ(floor.check(key(Hive::User, LR"(Software\Microsoft\Installer\Products\)" + std::wstring(kProduct))),
              Reason::None);
}

TEST(HardFloorTest, GuardsWindowsTasks) {
    const HardFloor floor = fixture_floor();
    EXPECT_EQ(floor.check(TaskLocation{LR"(\Microsoft\Windows\Defrag\ScheduledDefrag)"}), Reason::WindowsTask);
    EXPECT_EQ(floor.check(TaskLocation{LR"(\Contoso\Updater)"}), Reason::None);
    // Task folders: the root and Windows' own never go.
    EXPECT_EQ(floor.check(TaskLocation{LR"(\Contoso)", true}), Reason::None);
    EXPECT_EQ(floor.check(TaskLocation{LR"(\Contoso\Nested)", true}), Reason::None);
    for (const auto* path : {L"", L"\\", L"\\Microsoft", L"\\microsoft", L"\\Microsoft\\Windows"}) {
        EXPECT_EQ(floor.check(TaskLocation{path, true}), Reason::WindowsTask) << platform::to_utf8(path);
    }
}

TEST(HardFloorTest, GuardsTheTaskSchedulersOwnKeys) {
    const HardFloor floor = fixture_floor();
    constexpr wchar_t kTaskCache[] = LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache)";
    for (const auto* suffix : {L"", LR"(\Tree\Contoso)", LR"(\Tree\Contoso\Updater)", LR"(\Tasks\{0E4D5C66})",
                               LR"(\Plain\{0E4D5C66})"}) {
        EXPECT_EQ(floor.check(key(Hive::Machine, kTaskCache + std::wstring(suffix))), Reason::CoreRegistryKey)
            << platform::to_utf8(suffix);
    }
    EXPECT_EQ(
        floor.check(value(Hive::Machine, kTaskCache + std::wstring(LR"(\Tree\Contoso\Updater)"), L"Id")),
        Reason::CoreRegistryKey);
    EXPECT_EQ(floor.check(
                  key(Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCacheX)")),
              Reason::None);
}

TEST(HardFloorTest, GuardsWhatWindowsKeepsForRegisteredPackages) {
    const HardFloor floor = fixture_floor();
    constexpr wchar_t kPackage[] = L"NotepadPlusPlus_1.0.0.0_neutral__7njy0v32s6xk6";
    const std::wstring local_settings = LR"(Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion)";
    for (const std::wstring& path : std::vector<std::wstring>{
             LR"(Classes\PackagedCom)", LR"(Classes\PackagedCom\Package\)" + std::wstring(kPackage),
             LR"(Classes\PackagedCom\ClassIndex\{B298D29A-A6ED-11DE-BA8C-A68E55D89593}\)" + std::wstring(kPackage),
             LR"(Classes\ActivatableClasses\Package\)" + std::wstring(kPackage),
             LR"(Classes\Extensions\ContractId\Windows.Launch\PackageId\)" + std::wstring(kPackage),
             local_settings + LR"(\AppModel\Repository\Packages\)" + kPackage,
             local_settings + LR"(\AppContainer\Mappings\S-1-15-2-1-2-3)",
             LR"(Classes\AppX4hxtad77fbk3jkkeerkrm0ze94wjf3s9\Shell\Open)",
         }) {
        EXPECT_EQ(floor.check(key(Hive::User, L"Software\\" + path)), Reason::PackageRegistration)
            << platform::to_utf8(path);
        EXPECT_EQ(floor.check(key(Hive::Machine, L"SOFTWARE\\" + path)), Reason::PackageRegistration)
            << platform::to_utf8(path);
        EXPECT_EQ(floor.check(value(Hive::User, L"Software\\" + path, L"DllPath")), Reason::PackageRegistration)
            << platform::to_utf8(path);
    }
    constexpr wchar_t kCurrentVersion[] = LR"(Software\Microsoft\Windows\CurrentVersion)";
    for (const auto* below : {LR"(\AppHost\IndexedDB\Contoso_abc)", LR"(\BackgroundAccessApplications\Contoso_abc)"}) {
        const std::wstring path = kCurrentVersion + std::wstring(below);
        EXPECT_EQ(floor.check(key(Hive::User, path)), Reason::PackageRegistration) << platform::to_utf8(path);
    }
    for (const auto* path : {LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\AppModel\StateChange)",
                             LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Appx\AppxAllUserStore)",
                             LR"(SOFTWARE\Microsoft\SecurityManager\CapAuthz\ApplicationsEx\Contoso_abc)",
                             LR"(SOFTWARE\Microsoft\SecurityManager)",  // An ancestor
                             LR"(SOFTWARE\Classes\WOW6432Node\PackagedCom)"}) {
        EXPECT_EQ(floor.check(key(Hive::Machine, path)), Reason::PackageRegistration) << platform::to_utf8(path);
    }
    // A package's ProgID named in a file type's list goes with the package too; others' don't.
    EXPECT_EQ(floor.check(value(Hive::User, LR"(Software\Classes\.txt\OpenWithProgids)",
                                L"AppX4hxtad77fbk3jkkeerkrm0ze94wjf3s9")),
              Reason::PackageRegistration);
    EXPECT_EQ(floor.check(value(Hive::User, LR"(Software\Classes\.txt\OpenWithProgids)", L"Contoso.Text")),
              Reason::None);
    // Their ancestors may not go either, but a value in one isn't the package's; nor are look-alikes.
    EXPECT_EQ(floor.check(key(Hive::User, LR"(Software\Classes\Extensions)")), Reason::PackageRegistration);
    EXPECT_EQ(floor.check(value(Hive::User, LR"(Software\Classes\Extensions)", L"Contoso")), Reason::None);
    for (const auto* path : {LR"(Software\Classes\PackagedComX)", LR"(Software\Classes\Contoso.Document)",
                             LR"(Software\Classes\AppXShort)", LR"(Software\Contoso\Classes\PackagedCom)",
                             LR"(Software\Classes\Directory\shellex\DragDropHandlers\Contoso)"}) {
        EXPECT_EQ(floor.check(key(Hive::User, path)), Reason::None) << platform::to_utf8(path);
    }
}

ValueEditLocation edit_of(platform::RegistryAddress key, std::wstring name, ValueEditKind kind) {
    return {std::move(key), std::move(name), kind, std::nullopt, std::nullopt, {}, 0};
}

TEST(HardFloorTest, AllowsValueEditsOnlyInTheEnvironmentAndSharedDlls) {
    const HardFloor floor = fixture_floor();
    const RegistryAddress machine{Hive::Machine, L"",
                                  LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)"};
    const RegistryAddress user{Hive::User, L"S-1-5-21-1-2-3-1001", L"Environment"};
    const RegistryAddress shared{Hive::Machine, L"",
                                 LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)"};
    const RegistryAddress shared32{Hive::Machine, L"",
                                   LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\SharedDLLs)"};
    const auto check = [&](const RegistryAddress& key, const wchar_t* name, ValueEditKind kind) {
        return floor.check(edit_of(key, name, kind));
    };
    // The environment: lists lose entries, a created variable goes, a changed one comes back.
    EXPECT_EQ(check(machine, L"Path", ValueEditKind::RemovePathEntries), Reason::None);
    EXPECT_EQ(check(machine, L"PATHEXT", ValueEditKind::RemovePathEntries), Reason::None);
    EXPECT_EQ(check(user, L"Path", ValueEditKind::RemovePathEntries), Reason::None);
    EXPECT_EQ(check(user, L"Path", ValueEditKind::DeleteValue), Reason::None);
    EXPECT_EQ(check(machine, L"CONTOSO_HOME", ValueEditKind::DeleteValue), Reason::None);
    EXPECT_EQ(check(machine, L"CONTOSO_THEME", ValueEditKind::RestoreValue), Reason::None);
    EXPECT_EQ(check(machine, L"PYTHONPATH", ValueEditKind::DeleteValue), Reason::None);
    // ... but never Windows' own variables, the machine's lists as a whole, or a count.
    EXPECT_EQ(check(machine, L"Path", ValueEditKind::DeleteValue), Reason::NotEditable);
    EXPECT_EQ(check(machine, L"Path", ValueEditKind::RestoreValue), Reason::NotEditable);
    EXPECT_EQ(check(user, L"PATHEXT", ValueEditKind::RestoreValue), Reason::NotEditable);
    EXPECT_EQ(check(machine, L"PYTHONPATH", ValueEditKind::RestoreValue), Reason::NotEditable);
    EXPECT_EQ(check(machine, L"ComSpec", ValueEditKind::RestoreValue), Reason::NotEditable);
    EXPECT_EQ(check(user, L"TEMP", ValueEditKind::DeleteValue), Reason::NotEditable);
    EXPECT_EQ(check(machine, L"windir", ValueEditKind::RemovePathEntries), Reason::NotEditable);
    EXPECT_EQ(check(machine, L"CONTOSO_HOME", ValueEditKind::DecrementSharedDll), Reason::NotEditable);
    // SharedDLLs: counts go down, or go.
    for (const auto& key : {shared, shared32}) {
        EXPECT_EQ(check(key, LR"(C:\Shared\x.dll)", ValueEditKind::DecrementSharedDll), Reason::None);
        EXPECT_EQ(check(key, LR"(C:\Shared\x.dll)", ValueEditKind::DeleteValue), Reason::None);
        EXPECT_EQ(check(key, LR"(C:\Shared\x.dll)", ValueEditKind::RestoreValue), Reason::NotEditable);
        EXPECT_EQ(check(key, LR"(C:\Shared\x.dll)", ValueEditKind::RemovePathEntries), Reason::NotEditable);
    }
    // Anywhere else, nothing: other keys, other control sets, a user's classes or volatile environment.
    for (const auto& key :
         {RegistryAddress{Hive::Machine, L"", LR"(SOFTWARE\Contoso)"},
          RegistryAddress{Hive::Machine, L"", LR"(SYSTEM\ControlSet001\Control\Session Manager\Environment)"},
          RegistryAddress{Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)"},
          RegistryAddress{Hive::Machine, L"", L"Environment"},
          RegistryAddress{Hive::User, L"S-1-5-21-1-2-3-1001_Classes", L"Environment"},
          RegistryAddress{Hive::User, L"S-1-5-21-1-2-3-1001", L"Volatile Environment"},
          RegistryAddress{Hive::User, L"S-1-5-21-1-2-3-1001",
                          LR"(Software\Microsoft\Windows\CurrentVersion\SharedDLLs)"}}) {
        for (const auto kind : {ValueEditKind::RemovePathEntries, ValueEditKind::DecrementSharedDll,
                                ValueEditKind::DeleteValue, ValueEditKind::RestoreValue}) {
            EXPECT_EQ(floor.check(edit_of(key, L"Contoso", kind)), Reason::NotEditable)
                << platform::to_utf8(platform::display_path(key));
        }
    }
    // The other layer has nothing to say about edits.
    const ProtectedPolicy policy({LR"(C:\Users\me\AppData\Roaming)"});
    EXPECT_EQ(policy.check(edit_of(machine, L"Path", ValueEditKind::RemovePathEntries)), Reason::None);
}

platform::RawValue approval_bytes(std::initializer_list<int> values, DWORD type = REG_BINARY) {
    platform::RawValue value{L"Contoso", type, {}};
    for (const int byte : values) {
        value.data.push_back(static_cast<std::byte>(byte));
    }
    return value;
}

TEST(HardFloorTest, AllowsStartupApprovalsOnlyInTheirKeysAsTaskManagerWritesThem) {
    const HardFloor floor = fixture_floor();
    const auto disabled = approval_bytes({3, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8});
    const auto enabled = approval_bytes({2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    const auto check = [&](const RegistryAddress& key, std::optional<platform::RawValue> desired) {
        return floor.check(StartupApprovedLocation{key, L"Contoso", std::nullopt, std::move(desired)});
    };
    for (const auto* list : {L"Run", L"Run32", L"StartupFolder"}) {
        const std::wstring tail =
            LR"(\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\)" + std::wstring(list);
        for (const auto& key : {RegistryAddress{Hive::Machine, L"", L"SOFTWARE" + tail},
                                RegistryAddress{Hive::User, L"S-1-5-21-1-2-3-1001", L"Software" + tail}}) {
            EXPECT_EQ(check(key, disabled), Reason::None) << platform::to_utf8(key.path);
            EXPECT_EQ(check(key, enabled), Reason::None);
            EXPECT_EQ(check(key, std::nullopt), Reason::None);  // The approval deleted
            // Only 02 or 03, 12 bytes of REG_BINARY.
            EXPECT_EQ(check(key, approval_bytes({1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})), Reason::NotEditable);
            EXPECT_EQ(check(key, approval_bytes({6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})), Reason::NotEditable);
            EXPECT_EQ(check(key, approval_bytes({3, 0, 0, 0})), Reason::NotEditable);
            EXPECT_EQ(check(key, approval_bytes({3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0})),
                      Reason::NotEditable);
            EXPECT_EQ(check(key, approval_bytes({2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, REG_DWORD)),
                      Reason::NotEditable);
        }
    }
    // Nowhere else: the Run keys themselves, the list's parent or a key below it, other lists, the
    // WOW6432Node view, a user's classes.
    for (const auto& key : {
             RegistryAddress{Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)"},
             RegistryAddress{Hive::User, L"S-1-5-21-1-2-3-1001",
                             LR"(Software\Microsoft\Windows\CurrentVersion\Run)"},
             RegistryAddress{Hive::Machine, L"",
                             LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved)"},
             RegistryAddress{
                 Hive::Machine, L"",
                 LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run\Contoso)"},
             RegistryAddress{
                 Hive::Machine, L"",
                 LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\RunOnce)"},
             RegistryAddress{
                 Hive::Machine, L"",
                 LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)"},
             RegistryAddress{Hive::User, L"S-1-5-21-1-2-3-1001_Classes",
                             LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)"},
         }) {
        EXPECT_EQ(check(key, disabled), Reason::NotEditable)
            << platform::to_utf8(platform::display_path(key));
    }
    // An approval is no raw value: the other layer has nothing to say about it.
    const ProtectedPolicy policy({LR"(C:\Users\me\AppData\Roaming)"});
    EXPECT_EQ(policy.check(StartupApprovedLocation{{Hive::Machine, L"", L"SOFTWARE\\Microsoft"}, L"x"}),
              Reason::None);
}

TEST(HardFloorTest, GuardsWindowsTasksStates) {
    const HardFloor floor = fixture_floor();
    EXPECT_EQ(floor.check(TaskStateLocation{LR"(\Microsoft\Windows\Defrag\ScheduledDefrag)", true, false}),
              Reason::WindowsTask);
    EXPECT_EQ(floor.check(TaskStateLocation{LR"(\microsoft\Office\Updater)", false, true}),
              Reason::WindowsTask);
    EXPECT_EQ(floor.check(TaskStateLocation{LR"(\Contoso\Updater)", true, false}), Reason::None);
    EXPECT_EQ(floor.check(TaskStateLocation{LR"(\MicrosoftEdgeUpdateTaskMachineUA)", true, false}),
              Reason::None);
}

TEST(HardFloorTest, GuardsDriversAndWindowsServices) {
    HardFloor::Data data{LR"(C:\Windows)", {}, {}};
    data.services = [](const std::wstring& name) {
        using Status = ServiceFacts::Status;
        if (name == L"ContosoSvc") return ServiceFacts{Status::Found, false, false};
        if (name == L"ContosoDriver") return ServiceFacts{Status::Found, true, false};
        if (name == L"WindowsSvc") return ServiceFacts{Status::Found, false, true};
        if (name == L"Gone") return ServiceFacts{Status::Missing, false, false};
        return ServiceFacts{Status::Unreadable, false, false};
    };
    const HardFloor floor(data);
    const auto change = [&](const wchar_t* name, std::uint32_t after) {
        return floor.check(ServiceStartLocation{name, {SERVICE_AUTO_START, false}, {after, false}});
    };
    for (const DWORD after : {SERVICE_AUTO_START, SERVICE_DEMAND_START, SERVICE_DISABLED}) {
        EXPECT_EQ(change(L"ContosoSvc", after), Reason::None) << after;
        EXPECT_EQ(change(L"ContosoDriver", after), Reason::WindowsService) << after;
        EXPECT_EQ(change(L"WindowsSvc", after), Reason::WindowsService) << after;
        EXPECT_EQ(change(L"Gone", after), Reason::None) << after;  // The executor reports it missing
        EXPECT_EQ(change(L"Locked", after), Reason::WindowsService) << after;
    }
    // Never to boot or system start, or anything else.
    for (const std::uint32_t after :
         std::initializer_list<std::uint32_t>{SERVICE_BOOT_START, SERVICE_SYSTEM_START, 5, 0xFFFFFFFF}) {
        EXPECT_EQ(change(L"ContosoSvc", after), Reason::WindowsService) << after;
    }
    // Removing a service is ServiceLocation's business, and stays as it was.
    EXPECT_EQ(floor.check(ServiceLocation{L"WindowsSvc"}), Reason::None);
}

// Read-only queries of the live system: Windows' event log service (a svchost.exe service whose
// ServiceDll Windows signs through a catalog) is refused; a service that doesn't exist isn't.
TEST(HardFloorTest, LiveFloorGuardsWindowsServices) {
    const ServiceFacts facts = live_service_facts(L"EventLog");
    if (facts.status != ServiceFacts::Status::Found) {
        GTEST_SKIP() << "EventLog can't be read here";
    }
    EXPECT_FALSE(facts.driver);
    EXPECT_TRUE(facts.windows);
    EXPECT_TRUE(live_service_facts(L"Tcpip").driver);
    EXPECT_EQ(live_service_facts(L"Avel.Tests.No.Such.Service").status, ServiceFacts::Status::Missing);
    const HardFloor& floor = HardFloor::live();
    EXPECT_EQ(floor.check(ServiceStartLocation{
                  L"EventLog", {SERVICE_AUTO_START, false}, {SERVICE_DEMAND_START, false}}),
              Reason::WindowsService);
    EXPECT_EQ(floor.check(ServiceStartLocation{
                  L"Avel.Tests.No.Such.Service", {SERVICE_AUTO_START, false}, {SERVICE_DEMAND_START, false}}),
              Reason::None);
}

// Microsoft's applications' services (signed through its application root) aren't Windows' own:
// their start may change like anyone's. Read-only; skipped where none of them is installed.
TEST(HardFloorTest, LiveFloorLetsMicrosoftsApplicationServicesChange) {
    const HardFloor& floor = HardFloor::live();
    bool found = false;
    for (const auto* name : {L"edgeupdate", L"MicrosoftEdgeElevationService", L"SQLWriter", L"WSLService"}) {
        const ServiceFacts facts = live_service_facts(name);
        if (facts.status != ServiceFacts::Status::Found) {
            continue;
        }
        found = true;
        EXPECT_FALSE(facts.driver) << platform::to_utf8(name);
        EXPECT_FALSE(facts.windows) << platform::to_utf8(name);
        EXPECT_EQ(floor.check(
                      ServiceStartLocation{name, {SERVICE_AUTO_START, false}, {SERVICE_DEMAND_START, false}}),
                  Reason::None)
            << platform::to_utf8(name);
    }
    if (!found) {
        GTEST_SKIP() << "None of Microsoft's application services is installed here";
    }
}

TEST(HardFloorTest, LiveFloorGuardsThisMachine) {
    const HardFloor& floor = HardFloor::live();
    const auto machine = platform::machine_folders();
    EXPECT_EQ(floor.check(folder(machine.windows)), Reason::WindowsFolder);
    EXPECT_EQ(floor.check(folder(machine.program_files)), Reason::ProgramFiles);
    EXPECT_EQ(floor.check(folder(machine.users)), Reason::UserProfiles);
    EXPECT_EQ(floor.check(folder(machine.program_files + LR"(\Avel.Tests.Nonexistent)")), Reason::None);
    // Every user's profile, from ProfileList: the current user's among them.
    std::wstring home(MAX_PATH, L'\0');
    home.resize(::GetEnvironmentVariableW(L"USERPROFILE", home.data(), MAX_PATH));
    if (!home.empty() && platform::istarts_with(platform::current_user_sid(), L"S-1-5-21-")) {
        EXPECT_EQ(floor.check(folder(home)), Reason::UserProfiles);
        EXPECT_EQ(floor.check(folder(home + LR"(\AppData\Local)")), Reason::UserProfiles);
        EXPECT_EQ(floor.check(folder(home + LR"(\AppData\Local\Packages\Contoso.App_1a2b3c4d5e6f)")),
                  Reason::SystemArea);
    }
}

TEST(ProtectedPolicyTest, GuardsKnownFoldersAndSharedNames) {
    const ProtectedPolicy policy({LR"(C:\Users\me\AppData\Roaming\)",
                                  LR"(C:\ProgramData\Microsoft\Windows\Start Menu\Programs)"});
    EXPECT_EQ(policy.check(folder(LR"(C:\Users\me\AppData)")), Reason::KnownFolder);
    EXPECT_EQ(policy.check(folder(LR"(C:\Users\me\AppData\Roaming)")), Reason::KnownFolder);
    EXPECT_EQ(policy.check(folder(LR"(C:\Users\me\AppData\Roaming\Contoso)")), Reason::None);
    EXPECT_EQ(policy.check(folder(LR"(C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Contoso)")), Reason::None);
    EXPECT_EQ(policy.check(folder(LR"(C:\Program Files\Common Files)")), Reason::BlacklistedName);
    EXPECT_EQ(policy.check(folder(LR"(D:\Apps\Microsoft)")), Reason::BlacklistedName);
    EXPECT_EQ(policy.check(key(Hive::Machine, LR"(SOFTWARE\Contoso\Microsoft)")), Reason::BlacklistedName);
    EXPECT_EQ(policy.check(key(Hive::Machine, LR"(SOFTWARE\Contoso)")), Reason::None);
}

TEST(ProtectedPolicyTest, NameLists) {
    EXPECT_TRUE(is_blacklisted_folder_name(L"common files"));
    EXPECT_TRUE(is_blacklisted_folder_name(L"Package Cache"));
    EXPECT_TRUE(is_blacklisted_folder_name(L"Microsoft.NET"));
    EXPECT_FALSE(is_blacklisted_folder_name(L"Microsoft VS Code"));
    EXPECT_TRUE(is_blacklisted_key_name(L"WOW6432NODE"));
    EXPECT_TRUE(is_questionable_folder_name(L"Data"));
    EXPECT_FALSE(is_questionable_folder_name(L"Contoso"));
}

}  // namespace
}  // namespace avel::leftovers
