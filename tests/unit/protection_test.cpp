#include <gtest/gtest.h>

#include <windows.h>

#include <string>
#include <string_view>

#include "core/leftovers/protection.h"
#include "core/platform/known_folders.h"
#include "core/platform/text.h"

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

TEST(HardFloorTest, GuardsWindowsTasks) {
    const HardFloor floor = fixture_floor();
    EXPECT_EQ(floor.check(TaskLocation{LR"(\Microsoft\Windows\Defrag\ScheduledDefrag)"}), Reason::WindowsTask);
    EXPECT_EQ(floor.check(TaskLocation{LR"(\Contoso\Updater)"}), Reason::None);
}

TEST(HardFloorTest, LiveFloorGuardsThisMachine) {
    const HardFloor& floor = HardFloor::live();
    const auto machine = platform::machine_folders();
    EXPECT_EQ(floor.check(folder(machine.windows)), Reason::WindowsFolder);
    EXPECT_EQ(floor.check(folder(machine.program_files)), Reason::ProgramFiles);
    EXPECT_EQ(floor.check(folder(machine.users)), Reason::UserProfiles);
    EXPECT_EQ(floor.check(folder(machine.program_files + LR"(\Avel.Tests.Nonexistent)")), Reason::None);
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
