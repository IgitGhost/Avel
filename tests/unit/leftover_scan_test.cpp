// Runs the leftover scan against a fixture: a fake program "Contoso Fixture" laid out in a
// temporary folder tree (standing in for Program Files, ProgramData and a user profile) and in
// scratch registry keys (standing in for HKLM and the user's hive), with decoys that must never
// be reported. Expected scores follow BCU's rules (see product_name_test.cpp and scanners.h).
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "core/leftovers/scan.h"
#include "core/platform/text.h"
#include "scratch_registry.h"

namespace avel::leftovers {
namespace {

namespace fs = std::filesystem;
using platform::Hive;

constexpr wchar_t kSid[] = L"S-1-5-21-1-2-3-1001";

class LeftoverScanTest : public ::testing::Test {
protected:
    void SetUp() override {
        com_ = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        root_ = fs::temp_directory_path() / (L"avel-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-leftovers");
        fs::remove_all(root_);
        install_ = path(LR"(PF\Contoso\Fixture)");

        // Files
        file(LR"(PF\Contoso\Fixture\app.exe)");
        file(LR"(PF\Contoso\Other\other.exe)");
        file(LR"(PF\Common Files\Contoso Fixture\shared.dll)");  // Decoy: shared folder
        folder(LR"(PF\Contoso Fixture Tools)");
        folder(LR"(PF86)");
        file(LR"(PD\Contoso Fixture\settings.ini)");
        file(LR"(Users\fixture\AppData\Roaming\Contoso Fixture\config.json)");
        file(LR"(Users\fixture\AppData\Roaming\Microsoft\Contoso Fixture\cache.bin)");  // Decoy
        file(LR"(Users\fixture\AppData\Local\Contoso\Fixture\log.txt)");
        folder(LR"(Users\fixture\AppData\Roaming\Microsoft\Windows\Start Menu\Programs)");
        shortcut(LR"(PD\Microsoft\Windows\Start Menu\Programs\Contoso Fixture\Fixture.lnk)", install_ + L"\\app.exe");

        // Machine registry
        machine_.set_string(LR"(SOFTWARE\Contoso\Fixture)", L"InstallDir", install_);
        machine_.set_string(LR"(SOFTWARE\Contoso\Other)", L"InstallDir", path(LR"(PF\Contoso\Other)"));
        machine_.create(LR"(SOFTWARE\WOW6432Node\Contoso\Fixture)");
        machine_.create(LR"(SOFTWARE\Microsoft\Contoso Fixture)");  // Decoy: never searched
        machine_.set_string(kRun, L"ContosoUpdater", L"\"" + install_ + LR"(\app.exe" /background)");
        machine_.set_string(kRun, L"Unrelated", LR"("C:\Avel.Tests\Elsewhere\tool.exe")");
        machine_.set_string(kAppPaths, L"", install_ + L"\\app.exe");
        machine_.set_dword(kSharedDlls, install_ + L"\\shared.dll", 1);
        machine_.set_string(kUninstall, L"DisplayName", L"Contoso Fixture 1.0");
        // Services, COM classes and firewall rules live in the machine registry too
        machine_.set_string(kServices + std::wstring(LR"(\ContosoService)"), L"ImagePath",
                            L"\"" + install_ + LR"(\app.exe" -service)");
        machine_.set_dword(kServices + std::wstring(LR"(\ContosoService)"), L"Type", SERVICE_WIN32_OWN_PROCESS);
        machine_.set_string(kServices + std::wstring(LR"(\ContosoDriver)"), L"ImagePath", install_ + L"\\driver.sys");
        machine_.set_dword(kServices + std::wstring(LR"(\ContosoDriver)"), L"Type", SERVICE_KERNEL_DRIVER);  // Decoy
        machine_.set_string(LR"(SOFTWARE\Classes\CLSID\)" + std::wstring(kClsid) + LR"(\InprocServer32)", L"",
                            install_ + L"\\shell.dll");
        machine_.set_string(LR"(SOFTWARE\Classes\CLSID\{22222222-2222-2222-2222-222222222222}\InprocServer32)", L"",
                            LR"(C:\Avel.Tests\Elsewhere\shell.dll)");  // Decoy
        machine_.set_string(kContextMenu, L"", kClsid);
        machine_.set_string(kApproved, kClsid, L"Contoso Fixture shell extension");
        machine_.set_string(kFirewallRules, L"{33333333-3333-3333-3333-333333333333}",
                            L"v2.31|Action=Allow|Active=TRUE|Dir=In|App=" + install_ + LR"(\app.exe|Name=Contoso|)");
        machine_.set_string(kFirewallRules, L"{44444444-4444-4444-4444-444444444444}",
                            LR"(v2.31|Action=Allow|Active=TRUE|Dir=In|App=C:\Avel.Tests\Elsewhere\tool.exe|Name=X|)");
        // User registry
        user_.set_string(LR"(Software\Contoso Fixture)", L"Theme", L"dark");
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        if (SUCCEEDED(com_)) {
            ::CoUninitialize();
        }
    }

    std::wstring path(std::wstring_view relative) const { return (root_ / relative).wstring(); }

    void folder(std::wstring_view relative) const { fs::create_directories(root_ / relative); }

    void file(std::wstring_view relative) const {
        fs::create_directories((root_ / relative).parent_path());
        std::ofstream(root_ / relative) << "fixture";
    }

    void shortcut(std::wstring_view relative, const std::wstring& target) const {
        fs::create_directories((root_ / relative).parent_path());
        wil::com_ptr_nothrow<IShellLinkW> link;
        ASSERT_HRESULT_SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                                    IID_PPV_ARGS(link.put())));
        ASSERT_HRESULT_SUCCEEDED(link->SetPath(target.c_str()));
        const auto file = link.try_query<IPersistFile>();
        ASSERT_TRUE(file);
        ASSERT_HRESULT_SUCCEEDED(file->Save((root_ / relative).c_str(), TRUE));
    }

    ScanResult scan() {
        ScanEnvironment environment;
        environment.machine = machine_.get();
        auto& machine = environment.machine_folders;
        wchar_t windows[MAX_PATH] = {};
        ::GetWindowsDirectoryW(windows, MAX_PATH);
        machine.windows = windows;
        machine.program_files = path(L"PF");
        machine.program_files_x86 = path(L"PF86");
        machine.program_data = path(L"PD");
        machine.common_programs = path(LR"(PD\Microsoft\Windows\Start Menu\Programs)");
        machine.common_desktop = path(LR"(Public\Desktop)");
        machine.users = path(L"Users");
        machine.all = {machine.program_files, machine.program_files_x86, machine.program_data, machine.common_programs,
                       machine.users};

        UserScope user;
        user.profile = {kSid, L"PC\\fixture", path(LR"(Users\fixture)"), false};
        user.hive = user_.get();
        user.folders = platform::user_folders(user.hive, user.profile);
        environment.users.push_back(std::move(user));
        environment.current_user_sid = kSid;
        environment.tasks = std::vector<ScheduledTask>{
            {LR"(\Contoso\Fixture Updater)", {L"\"" + install_ + LR"(\app.exe" /update)"}},
            {LR"(\Unrelated)", {LR"(C:\Avel.Tests\Elsewhere\tool.exe)"}}};

        ScanTarget target;
        target.program_id = L"reg:HKLM64:ContosoFixture";
        target.display_name = L"Contoso Fixture 1.0";
        target.names = make_product_names(target.display_name, L"Contoso Ltd");
        target.install_location = install_;
        target.uninstall_key = platform::RegistryAddress{Hive::Machine, L"", kUninstall};

        inventory::Program other;
        other.id = L"reg:HKLM64:ContosoOther";
        other.display_name = L"Contoso Other";
        other.install_location = path(LR"(PF\Contoso\Other)");
        const std::vector<inventory::Program> installed{other};
        return scan_leftovers(environment, std::vector<ScanTarget>{target}, installed);
    }

    static const Leftover* find(const ScanResult& result, const Location& location) {
        const std::wstring key = identity(location);
        for (const auto& item : result.leftovers) {
            if (identity(item.location) == key) {
                return &item;
            }
        }
        return nullptr;
    }

    void expect_found(const ScanResult& result, const Location& location, Confidence expected) const {
        const Leftover* item = find(result, location);
        ASSERT_NE(item, nullptr) << platform::to_utf8(identity(location));
        EXPECT_EQ(level(item->records), expected)
            << platform::to_utf8(identity(location)) << " scored " << score(item->records);
        EXPECT_EQ(item->owner_ids, std::vector<std::wstring>{L"reg:HKLM64:ContosoFixture"});
    }

    Location dir(std::wstring_view relative) const { return FileLocation{path(relative), true}; }
    static Location machine_key(std::wstring_view key) { return RegistryKeyLocation{{Hive::Machine, L"", std::wstring(key)}}; }

    static constexpr wchar_t kRun[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Run)";
    static constexpr wchar_t kAppPaths[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\fixture.exe)";
    static constexpr wchar_t kSharedDlls[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)";
    static constexpr wchar_t kUninstall[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ContosoFixture)";
    static constexpr wchar_t kServices[] = LR"(SYSTEM\CurrentControlSet\Services)";
    static constexpr wchar_t kFirewallRules[] =
        LR"(SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\FirewallRules)";
    static constexpr wchar_t kClsid[] = L"{11111111-1111-1111-1111-111111111111}";
    static constexpr wchar_t kContextMenu[] = LR"(SOFTWARE\Classes\*\shellex\ContextMenuHandlers\ContosoMenu)";
    static constexpr wchar_t kApproved[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved)";

    HRESULT com_ = E_FAIL;
    fs::path root_;
    std::wstring install_;
    testing::ScratchKey machine_;
    testing::ScratchKey user_;
};

TEST_F(LeftoverScanTest, FindsTheProgramsFoldersWithBcuScores) {
    const auto result = scan();
    // The install folder: explicit (+4), and its name one level below the publisher's folder
    // (dodgy -2, depth 0, company +4).
    expect_found(result, dir(LR"(PF\Contoso\Fixture)"), Confidence::VeryGood);
    // The publisher's folder: another program still lives in it.
    expect_found(result, dir(LR"(PF\Contoso)"), Confidence::Bad);
    // An empty folder whose name contains the program's.
    expect_found(result, dir(LR"(PF\Contoso Fixture Tools)"), Confidence::Good);
    expect_found(result, dir(LR"(PD\Contoso Fixture)"), Confidence::Good);
    expect_found(result, dir(LR"(PD\Microsoft\Windows\Start Menu\Programs\Contoso Fixture)"), Confidence::Good);
    expect_found(result, dir(LR"(Users\fixture\AppData\Roaming\Contoso Fixture)"), Confidence::Good);
    // The publisher's folder in AppData holds nothing but the program's folder (+4), but "Contoso
    // Other" is named like it too (-2): -2 + 2 - 2 - 1 + 4 - 2.
    expect_found(result, dir(LR"(Users\fixture\AppData\Local\Contoso)"), Confidence::Bad);
    expect_found(result, dir(LR"(Users\fixture\AppData\Local\Contoso\Fixture)"), Confidence::Good);
}

TEST_F(LeftoverScanTest, FindsShortcutsAndReferencesIntoTheInstallFolder) {
    const auto result = scan();
    const Location link =
        FileLocation{path(LR"(PD\Microsoft\Windows\Start Menu\Programs\Contoso Fixture\Fixture.lnk)"), false};
    expect_found(result, link, Confidence::Good);
    const Leftover* item = find(result, link);
    ASSERT_NE(item, nullptr);
    ASSERT_TRUE(item->inside.has_value());
    EXPECT_EQ(identity(result.leftovers[*item->inside].location),
              identity(dir(LR"(PD\Microsoft\Windows\Start Menu\Programs\Contoso Fixture)")));

    expect_found(result, RegistryValueLocation{{Hive::Machine, L"", kRun}, L"ContosoUpdater"}, Confidence::VeryGood);
    expect_found(result, machine_key(kAppPaths), Confidence::Good);
    expect_found(result, RegistryValueLocation{{Hive::Machine, L"", kSharedDlls}, install_ + L"\\shared.dll"},
                 Confidence::Good);
    EXPECT_EQ(find(result, RegistryValueLocation{{Hive::Machine, L"", kRun}, L"Unrelated"}), nullptr);
}

TEST_F(LeftoverScanTest, FindsKeysInEveryRootAndTheUninstallKey) {
    const auto result = scan();
    expect_found(result, machine_key(kUninstall), Confidence::VeryGood);
    expect_found(result, machine_key(LR"(SOFTWARE\Contoso\Fixture)"), Confidence::VeryGood);
    // The 32-bit view's copy shares the evidence of the 64-bit one.
    expect_found(result, machine_key(LR"(SOFTWARE\WOW6432Node\Contoso\Fixture)"), Confidence::VeryGood);
    expect_found(result, machine_key(LR"(SOFTWARE\Contoso)"), Confidence::Bad);
    expect_found(result, RegistryKeyLocation{{Hive::User, kSid, LR"(Software\Contoso Fixture)"}}, Confidence::Good);
    EXPECT_EQ(find(result, machine_key(LR"(SOFTWARE\Contoso\Other)")), nullptr);
}

TEST_F(LeftoverScanTest, FindsServicesTasksShellExtensionsAndFirewallRules) {
    const auto result = scan();
    expect_found(result, ServiceLocation{L"ContosoService"}, Confidence::VeryGood);
    expect_found(result, TaskLocation{LR"(\Contoso\Fixture Updater)"}, Confidence::VeryGood);
    expect_found(result, machine_key(LR"(SOFTWARE\Classes\CLSID\)" + std::wstring(kClsid)), Confidence::Good);
    expect_found(result, machine_key(kContextMenu), Confidence::Good);
    expect_found(result, RegistryValueLocation{{Hive::Machine, L"", kApproved}, kClsid}, Confidence::Good);
    expect_found(result, FirewallRuleLocation{L"{33333333-3333-3333-3333-333333333333}"}, Confidence::Good);

    EXPECT_EQ(find(result, ServiceLocation{L"ContosoDriver"}), nullptr);  // Drivers are never leftovers
    EXPECT_EQ(find(result, TaskLocation{LR"(\Unrelated)"}), nullptr);
    EXPECT_EQ(find(result, machine_key(LR"(SOFTWARE\Classes\CLSID\{22222222-2222-2222-2222-222222222222})")), nullptr);
    EXPECT_EQ(find(result, FirewallRuleLocation{L"{44444444-4444-4444-4444-444444444444}"}), nullptr);
}

TEST_F(LeftoverScanTest, IgnoresARecordedFolderThatHoldsProtectedOnes) {
    install_ = path(L"PF");  // A broken entry recording Program Files itself
    const auto result = scan();
    EXPECT_EQ(find(result, ServiceLocation{L"ContosoService"}), nullptr);
    EXPECT_EQ(find(result, TaskLocation{LR"(\Contoso\Fixture Updater)"}), nullptr);
    EXPECT_EQ(find(result, dir(L"PF")), nullptr);
    // Name matches still work.
    expect_found(result, dir(LR"(PD\Contoso Fixture)"), Confidence::Good);
}

TEST_F(LeftoverScanTest, NeverReportsSharedOrProtectedPlaces) {
    const auto result = scan();
    for (const auto& item : result.leftovers) {
        const std::wstring key = identity(item.location);
        EXPECT_EQ(key.find(L"common files"), std::wstring::npos) << platform::to_utf8(key);
        EXPECT_EQ(key.find(L"roaming\\microsoft\\contoso"), std::wstring::npos) << platform::to_utf8(key);
        EXPECT_EQ(key.find(L"software\\microsoft\\contoso"), std::wstring::npos) << platform::to_utf8(key);
    }
    // The fixture's Program Files and ProgramData stand-ins are known folders.
    EXPECT_EQ(find(result, dir(L"PF")), nullptr);
    EXPECT_EQ(find(result, dir(L"PD")), nullptr);
}

}  // namespace
}  // namespace avel::leftovers
