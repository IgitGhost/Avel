#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "core/inventory/uninstall_entry.h"
#include "registry_values.h"

namespace avel::inventory {
namespace {

using namespace std::chrono;
using testing::dword_value;
using testing::string_value;

UninstallKeyContext context_for(std::wstring key_name) {
    UninstallKeyContext context;
    context.location.key_name = std::move(key_name);
    context.location.view = RegistryView::Machine64;
    context.id_prefix = L"reg:HKLM64:";
    context.architecture = Architecture::X64;
    context.expand = [](std::wstring_view text) {
        std::wstring result(text);
        if (const auto at = result.find(L"%ProgramFiles%"); at != std::wstring::npos) {
            result.replace(at, 14, LR"(C:\Program Files)");
        }
        return result;
    };
    return context;
}

TEST(UninstallEntryTest, ParsesATypicalEntry) {
    const std::vector values = {
        string_value(L"DisplayName", L"  7-Zip 23.01 (x64)  "),
        string_value(L"DisplayVersion", L"23.01"),
        string_value(L"Publisher", L"Igor Pavlov"),
        string_value(L"InstallLocation", LR"("C:\Program Files\7-Zip\")"),
        string_value(L"UninstallString", LR"("C:\Program Files\7-Zip\Uninstall.exe")"),
        string_value(L"DisplayIcon", LR"(%ProgramFiles%\7-Zip\7zFM.exe)", REG_EXPAND_SZ),
        string_value(L"InstallDate", L"20240501"),
        string_value(L"URLInfoAbout", L"https://www.7-zip.org/"),
        dword_value(L"EstimatedSize", 5000),
        dword_value(L"NoModify", 1),
    };
    const auto program = parse_uninstall_entry(context_for(L"7-Zip"), values);
    ASSERT_TRUE(program.has_value());
    EXPECT_EQ(program->id, L"reg:HKLM64:7-Zip");
    EXPECT_EQ(program->display_name, L"7-Zip 23.01 (x64)");
    EXPECT_EQ(program->display_version, L"23.01");
    EXPECT_EQ(program->publisher, L"Igor Pavlov");
    EXPECT_EQ(program->install_location, LR"(C:\Program Files\7-Zip)");
    EXPECT_EQ(program->display_icon, LR"(C:\Program Files\7-Zip\7zFM.exe)");
    EXPECT_EQ(program->install_date, sys_days{year{2024} / May / 1});
    EXPECT_EQ(program->estimated_size_bytes, 5000u * 1024u);
    EXPECT_EQ(program->about_url, L"https://www.7-zip.org/");
    EXPECT_EQ(program->architecture, Architecture::X64);
    EXPECT_TRUE(program->no_modify);
    EXPECT_FALSE(program->no_remove);
    EXPECT_TRUE(is_listed_in_settings(*program));
    ASSERT_TRUE(program->registry.has_value());
    EXPECT_EQ(program->registry->key_name, L"7-Zip");
}

TEST(UninstallEntryTest, SkipsKeysWithoutUsefulInformation) {
    EXPECT_FALSE(parse_uninstall_entry(context_for(L"Empty"), {}).has_value());
    EXPECT_FALSE(
        parse_uninstall_entry(context_for(L"Junk"), {dword_value(L"EstimatedSize", 10)}).has_value());
}

TEST(UninstallEntryTest, KeepsNamelessEntriesButHidesThem) {
    const auto program = parse_uninstall_entry(context_for(L"Orphan"),
                                               {string_value(L"UninstallString", LR"(C:\x\unins000.exe)")});
    ASSERT_TRUE(program.has_value());
    EXPECT_TRUE(program->display_name.empty());
    EXPECT_FALSE(is_listed_in_settings(*program));
}

TEST(UninstallEntryTest, PrefersLocalizedDisplayName) {
    auto context = context_for(L"mstsc");
    context.resolve_indirect = [](std::wstring_view reference) {
        return reference == LR"(@C:\Windows\System32\mstsc.exe,-4000)"
                   ? std::wstring(L"Remote Desktop Connection")
                   : std::wstring();
    };
    const auto program = parse_uninstall_entry(
        context, {string_value(L"DisplayName_Localized", LR"(@C:\Windows\System32\mstsc.exe,-4000)"),
                  string_value(L"UninstallString", LR"("C:\Windows\System32\mstsc.exe" /uninstall)")});
    ASSERT_TRUE(program.has_value());
    EXPECT_EQ(program->display_name, L"Remote Desktop Connection");
}

TEST(UninstallEntryTest, FallsBackToDisplayNameWhenTheLocalizedNameDoesNotResolve) {
    auto context = context_for(L"Intel");
    context.resolve_indirect = [](std::wstring_view) { return std::wstring(); };
    const auto program =
        parse_uninstall_entry(context, {string_value(L"DisplayName", L"Intel(R) Something"),
                                        string_value(L"DisplayName_Localized", LR"(@C:\Missing.dll,-1)")});
    ASSERT_TRUE(program.has_value());
    EXPECT_EQ(program->display_name, L"Intel(R) Something");
}

TEST(UninstallEntryTest, ReadsHiddenUninstallStrings) {
    const auto program = parse_uninstall_entry(
        context_for(L"Sneaky"), {string_value(L"DisplayName", L"Sneaky"),
                                 string_value(L"UninstallString_Hidden", LR"("C:\Sneaky\uninstall.exe")")});
    ASSERT_TRUE(program.has_value());
    EXPECT_EQ(program->uninstall_string, LR"("C:\Sneaky\uninstall.exe")");
}

TEST(UninstallEntryTest, QuietUninstallStringStandsInForAMissingUninstallString) {
    const auto program = parse_uninstall_entry(
        context_for(L"Quiet"), {string_value(L"DisplayName", L"Quiet"),
                                string_value(L"QuietUninstallString", LR"("C:\Quiet\uninst.exe" /S)")});
    ASSERT_TRUE(program.has_value());
    EXPECT_EQ(program->uninstall_string, LR"("C:\Quiet\uninst.exe" /S)");
    EXPECT_EQ(program->quiet_uninstall_string, LR"("C:\Quiet\uninst.exe" /S)");
}

TEST(UninstallEntryTest, WindowsInstallerProductTakesTheCodeFromTheKeyName) {
    const auto program = parse_uninstall_entry(
        context_for(L"{605108c1-153e-43d8-8a67-7ce326b00eca}"),
        {string_value(L"DisplayName", L"Product"),
         string_value(L"UninstallString", L"MsiExec.exe /I{605108C1-153E-43D8-8A67-7CE326B00ECA}"),
         dword_value(L"WindowsInstaller", 1)});
    ASSERT_TRUE(program.has_value());
    EXPECT_TRUE(program->windows_installer);
    EXPECT_EQ(program->installer_type, InstallerType::Msi);
    EXPECT_EQ(program->msi_product_code, L"{605108C1-153E-43D8-8A67-7CE326B00ECA}");
}

TEST(UninstallEntryTest, DetectsUpdates) {
    auto update = [](std::wstring key, std::vector<platform::RegistryValue> extra) {
        extra.push_back(string_value(L"DisplayName", L"Some update"));
        return parse_uninstall_entry(context_for(std::move(key)), extra)->is_update;
    };
    EXPECT_TRUE(update(L"A", {string_value(L"ParentKeyName", L"Office16.PROPLUS")}));
    EXPECT_TRUE(update(L"B", {string_value(L"ReleaseType", L"Security Update")}));
    EXPECT_TRUE(update(L"C", {string_value(L"ReleaseType", L"Hotfix")}));
    EXPECT_TRUE(update(L"KB5034441", {}));
    EXPECT_TRUE(update(L"D", {string_value(L"", L"KB2504637")}));
    EXPECT_FALSE(update(L"E", {string_value(L"ReleaseType", L"Product")}));
    EXPECT_FALSE(update(L"KBTools", {}));
}

TEST(UninstallEntryTest, SystemComponentsAreNotListed) {
    const auto program =
        parse_uninstall_entry(context_for(L"Hidden"),
                              {string_value(L"DisplayName", L"Runtime"), dword_value(L"SystemComponent", 1)});
    ASSERT_TRUE(program.has_value());
    EXPECT_TRUE(program->system_component);
    EXPECT_FALSE(is_listed_in_settings(*program));
}

TEST(UninstallEntryTest, EstimatedSizeWrittenAsText) {
    const auto program = parse_uninstall_entry(
        context_for(L"Text"),
        {string_value(L"DisplayName", L"Text"), string_value(L"EstimatedSize", L" 2048 ")});
    ASSERT_TRUE(program.has_value());
    EXPECT_EQ(program->estimated_size_bytes, 2048u * 1024u);
}

TEST(UninstallEntryTest, CleansUpVersionSeparators) {
    const auto program = parse_uninstall_entry(
        context_for(L"Version"),
        {string_value(L"DisplayName", L"V"), string_value(L"DisplayVersion", L"1, 2, 3,4")});
    ASSERT_TRUE(program.has_value());
    EXPECT_EQ(program->display_version, L"1.2.3.4");
}

TEST(UninstallEntryTest, InstallerTypeDetection) {
    EXPECT_EQ(installer_type_from_command(L"MsiExec.exe /X{12345678-1234-1234-1234-123456789ABC}"),
              InstallerType::Msi);
    EXPECT_EQ(
        installer_type_from_command(
            LR"("C:\ProgramData\Package Cache\{3a1b2c3d-1111-2222-3333-444455556666}\vc_redist.x64.exe" /uninstall)"),
        InstallerType::WixBurn);
    EXPECT_EQ(installer_type_from_command(LR"("C:\Program Files\App\unins000.exe")"),
              InstallerType::InnoSetup);
    EXPECT_EQ(installer_type_from_command(LR"("C:\Users\me\AppData\Local\Discord\Update.exe" --uninstall)"),
              InstallerType::Squirrel);
    EXPECT_EQ(
        installer_type_from_command(
            LR"(C:\Program Files (x86)\InstallShield Installation Information\{ABC}\setup.exe -runfromtemp)"),
        InstallerType::InstallShield);
    EXPECT_EQ(installer_type_from_command(LR"(powershell.exe -File C:\x\uninstall.ps1)"),
              InstallerType::PowerShell);
    EXPECT_EQ(
        installer_type_from_command(
            LR"("C:\Program Files (x86)\NSIS Uninstall Information\{3584}\Setup.exe" _?=C:\Program Files (x86)\NSIS Uninstall Information\{3584})"),
        InstallerType::Nsis);
    EXPECT_EQ(installer_type_from_command(LR"(C:\Program Files\App\uninst.exe)"), InstallerType::Unknown);
    EXPECT_EQ(installer_type_from_command(L""), InstallerType::Unknown);
}

TEST(UninstallEntryTest, InstallDateFormats) {
    EXPECT_EQ(parse_install_date(L"20240501"), sys_days{year{2024} / May / 1});
    EXPECT_EQ(parse_install_date(L"20243112"), sys_days{year{2024} / December / 31});  // YYYYDDMM
    EXPECT_EQ(parse_install_date(L"2024-05-01"), sys_days{year{2024} / May / 1});
    EXPECT_EQ(parse_install_date(L"Sat Oct 17 22:38:01 2026"), sys_days{year{2026} / October / 17});
    EXPECT_EQ(parse_install_date(L"Wed Oct  7 09:00:00 2026"), sys_days{year{2026} / October / 7});
    EXPECT_FALSE(parse_install_date(L"Sat Foo 17 22:38:01 2026").has_value());
    EXPECT_FALSE(parse_install_date(L"20245511").has_value());
    EXPECT_FALSE(parse_install_date(L"20241399").has_value());
    EXPECT_FALSE(parse_install_date(L"5/1/2024").has_value());
    EXPECT_FALSE(parse_install_date(L"").has_value());
}

TEST(UninstallEntryTest, ExtractGuid) {
    EXPECT_EQ(extract_guid(L"MsiExec.exe /X{605108c1-153e-43d8-8a67-7ce326b00eca} /qn"),
              L"{605108C1-153E-43D8-8A67-7CE326B00ECA}");
    EXPECT_FALSE(extract_guid(L"{not-a-guid}").has_value());
    EXPECT_FALSE(extract_guid(L"").has_value());
}

}  // namespace
}  // namespace avel::inventory
