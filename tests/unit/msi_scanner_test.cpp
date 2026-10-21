// The Windows Installer registration scanner, against a fake registration in scratch registry keys
// standing in for HKLM and a user's hive: a product's keys, the upgrade codes and components that
// list it (alone, or shared with another product), and the same product registered for a user.
#include <gtest/gtest.h>

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/inventory/msi_codes.h"
#include "core/leftovers/scan.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "scratch_registry.h"

namespace avel::leftovers {
namespace {

namespace fs = std::filesystem;
using platform::Hive;

constexpr wchar_t kSid[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kCode[] = L"{23170F69-40C1-2702-2600-000001000000}";
constexpr wchar_t kPacked[] = L"96F071321C0420726200000010000000";
constexpr wchar_t kOther[] = L"1C801506E3518D34A876C73E620BE0AC";  // Another product
constexpr wchar_t kClasses[] = LR"(SOFTWARE\Classes\Installer)";
constexpr wchar_t kInstaller[] = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Installer)";

class MsiScannerTest : public ::testing::Test {
protected:
    void SetUp() override {
        com_ = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // Program Files and the product's files, which its entry doesn't locate.
        root_ =
            fs::temp_directory_path() / (L"avel-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-msi");
        fs::remove_all(root_);
        fs::create_directories(root_ / LR"(PF\7-Zip\Lang)");
        std::ofstream(root_ / LR"(PF\7-Zip\7z.exe)") << "7z";
        std::ofstream(root_ / LR"(PF\7-Zip\Lang\de.txt)") << "de";
        // Machine-wide registration.
        machine_.set_string(path(kClasses, LR"(\Products\)", kPacked), L"ProductName", L"7-Zip");
        machine_.set_string(path(kClasses, LR"(\Features\)", kPacked), L"Complete", L"");
        machine_.set_string(path(kClasses, LR"(\UpgradeCodes\)", L"11111111111111111111111111111111"),
                            kPacked, L"");
        machine_.set_string(path(kClasses, LR"(\UpgradeCodes\)", L"22222222222222222222222222222222"),
                            kPacked, L"");
        machine_.set_string(path(kClasses, LR"(\UpgradeCodes\)", L"22222222222222222222222222222222"), kOther,
                            L"");
        machine_.set_string(path(kClasses, LR"(\Products\)", kOther), L"ProductName", L"Other");
        machine_.set_string(path(kClasses, LR"(\Dependencies\)", kCode), L"", L"7-Zip");
        const std::wstring system = kInstaller + std::wstring(LR"(\UserData\S-1-5-18)");
        machine_.set_string(path(system, LR"(\Products\)", kPacked) + LR"(\InstallProperties)",
                            L"LocalPackage", LR"(C:\Windows\Installer\1a2b3c.msi)");
        machine_.set_string(path(system, LR"(\Components\)", L"AAAA0000000000000000000000000001"), kPacked,
                            (root_ / LR"(PF\7-Zip\7z.exe)").wstring());
        machine_.set_string(path(system, LR"(\Components\)", L"AAAA0000000000000000000000000002"), kPacked,
                            LR"(C:\Windows\System32\shared.dll)");
        machine_.set_string(path(system, LR"(\Components\)", L"AAAA0000000000000000000000000002"), kOther,
                            LR"(C:\Windows\System32\shared.dll)");
        machine_.set_string(path(system, LR"(\Components\)", L"AAAA0000000000000000000000000003"), kOther,
                            LR"(C:\Program Files\Other\other.exe)");
        // The same product, registered for a user too.
        user_.set_string(path(LR"(Software\Microsoft\Installer)", LR"(\Products\)", kPacked), L"ProductName",
                         L"7-Zip");
        user_.set_string(path(LR"(Software\Microsoft\Installer)", LR"(\UpgradeCodes\)",
                              L"11111111111111111111111111111111"),
                         kPacked, L"");
        const std::wstring user_data = kInstaller + std::wstring(LR"(\UserData\)") + kSid;
        machine_.set_string(path(user_data, LR"(\Products\)", kPacked) + LR"(\InstallProperties)",
                            L"LocalPackage", LR"(C:\Windows\Installer\4d5e6f.msi)");
        machine_.set_string(path(user_data, LR"(\Components\)", L"BBBB0000000000000000000000000001"), kPacked,
                            LR"(C:\Users\fixture\AppData\Local\7-Zip\7z.exe)");
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        if (SUCCEEDED(com_)) {
            ::CoUninitialize();
        }
    }

    static std::wstring path(std::wstring_view base, std::wstring_view middle, std::wstring_view name) {
        return std::wstring(base) + std::wstring(middle) + std::wstring(name);
    }

    ScanResult scan(inventory::Scope scope) {
        ScanEnvironment environment;
        environment.machine = machine_.get();
        wchar_t windows[MAX_PATH] = {};
        ::GetWindowsDirectoryW(windows, MAX_PATH);
        environment.machine_folders.windows = windows;
        environment.machine_folders.program_files = (root_ / L"PF").wstring();
        environment.machine_folders.all = {environment.machine_folders.program_files};
        UserScope user;
        user.profile = {kSid, L"PC\\fixture", LR"(C:\Avel.Tests\Users\fixture)", false};
        user.hive = user_.get();
        environment.users.push_back(std::move(user));
        environment.current_user_sid = kSid;
        environment.tasks = std::vector<ScheduledTask>{};

        ScanTarget target;
        target.program_id = L"msi:" + std::wstring(kCode);
        target.display_name = L"7-Zip 26.00 (x64 edition)";
        target.names = make_product_names(target.display_name, L"Igor Pavlov");
        target.scope = scope;
        target.user_sid = scope == inventory::Scope::User ? kSid : L"";
        target.msi_product_code = kCode;
        return scan_leftovers(environment, std::vector<ScanTarget>{target}, {});
    }

    static std::vector<std::wstring> found(const ScanResult& result) {
        std::vector<std::wstring> items;
        for (const auto& item : result.leftovers) {
            if (item.found_by != Scanner::MsiRegistration) {
                continue;
            }
            EXPECT_EQ(level(item.records), Confidence::VeryGood);
            items.push_back(identity(item.location));
        }
        std::sort(items.begin(), items.end());
        return items;
    }

    static std::wstring key(Hive hive, std::wstring_view path) {
        return identity(RegistryKeyLocation{{hive, hive == Hive::User ? kSid : L"", std::wstring(path)}});
    }

    static std::wstring value(std::wstring_view path, std::wstring_view name) {
        return identity(RegistryValueLocation{{Hive::Machine, L"", std::wstring(path)}, std::wstring(name)});
    }

    HRESULT com_ = E_FAIL;
    fs::path root_;
    testing::ScratchKey machine_;
    testing::ScratchKey user_;
};

TEST_F(MsiScannerTest, FindsAMachineWideProductsRegistration) {
    const auto result = scan(inventory::Scope::Machine);
    const std::wstring system = kInstaller + std::wstring(LR"(\UserData\S-1-5-18)");
    std::vector<std::wstring> expected = {
        key(Hive::Machine, path(kClasses, LR"(\Products\)", kPacked)),
        key(Hive::Machine, path(kClasses, LR"(\Features\)", kPacked)),
        key(Hive::Machine, path(kClasses, LR"(\Dependencies\)", kCode)),
        key(Hive::Machine, path(system, LR"(\Products\)", kPacked)),
        // An upgrade code and a component only this product used go whole; shared ones lose its value.
        key(Hive::Machine, path(kClasses, LR"(\UpgradeCodes\)", L"11111111111111111111111111111111")),
        value(path(kClasses, LR"(\UpgradeCodes\)", L"22222222222222222222222222222222"), kPacked),
        key(Hive::Machine, path(system, LR"(\Components\)", L"AAAA0000000000000000000000000001")),
        value(path(system, LR"(\Components\)", L"AAAA0000000000000000000000000002"), kPacked),
    };
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(found(result), expected);
    EXPECT_EQ(result.protected_count, 0u);
}

TEST_F(MsiScannerTest, FindsTheFolderOfAProductThatRecordsNone) {
    const auto result = scan(inventory::Scope::Machine);
    const std::wstring folder = platform::normalize_path((root_ / LR"(PF\7-Zip)").wstring());
    ASSERT_EQ(result.targets.size(), 1u);
    EXPECT_EQ(result.targets.front().install_location, folder);
    const auto item =
        std::find_if(result.leftovers.begin(), result.leftovers.end(), [&](const Leftover& leftover) {
            return identity(leftover.location) == identity(FileLocation{folder, true});
        });
    ASSERT_NE(item, result.leftovers.end());
    EXPECT_EQ(item->found_by, Scanner::InstallLocation);
}

TEST_F(MsiScannerTest, FindsAPerUserProductsRegistrationOnlyForThatUser) {
    const auto result = scan(inventory::Scope::User);
    const std::wstring user_data = kInstaller + std::wstring(LR"(\UserData\)") + kSid;
    std::vector<std::wstring> expected = {
        key(Hive::User, path(LR"(Software\Microsoft\Installer)", LR"(\Products\)", kPacked)),
        key(Hive::User, path(LR"(Software\Microsoft\Installer)", LR"(\UpgradeCodes\)",
                             L"11111111111111111111111111111111")),
        key(Hive::Machine, path(user_data, LR"(\Products\)", kPacked)),
        key(Hive::Machine, path(user_data, LR"(\Components\)", L"BBBB0000000000000000000000000001")),
    };
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(found(result), expected);
}

TEST_F(MsiScannerTest, PackedCodeOfTheProduct) { EXPECT_EQ(inventory::pack_guid(kCode), kPacked); }

}  // namespace
}  // namespace avel::leftovers
