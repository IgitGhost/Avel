// Expected scores were produced by running Bulk Crap Uninstaller's original AreEntriesRelated (C#,
// without the orphan/rating branches Avel leaves out) on the same scenarios.
#include <gtest/gtest.h>

#include <chrono>

#include "core/inventory/related_entries.h"

namespace avel::inventory {
namespace {

using namespace std::chrono;

Program make(std::wstring name, std::wstring version = {}, std::wstring publisher = {},
             std::wstring location = {}, Architecture architecture = Architecture::Unknown) {
    Program program;
    program.display_name = std::move(name);
    program.display_version = std::move(version);
    program.publisher = std::move(publisher);
    program.install_location = std::move(location);
    program.architecture = architecture;
    return program;
}

constexpr sys_days kMay1 = sys_days{year{2024} / May / 1};
constexpr sys_days kMay3 = sys_days{year{2024} / May / 3};

TEST(RelatedEntriesTest, SameInstallLocationIsCertain) {
    EXPECT_EQ(relatedness_score(make(L"Alpha", {}, {}, LR"(C:\Program Files\Alpha)"),
                                make(L"Beta", {}, {}, LR"(c:\program files\alpha\)")),
              kCertainlyRelated);
}

TEST(RelatedEntriesTest, SameUninstallCommandIsCertain) {
    Program base = make(L"Alpha");
    base.uninstall_string = LR"("C:\Alpha\unins000.exe")";
    Program other = make(L"Beta");
    other.uninstall_string = LR"(C:\Alpha\unins000.exe)";
    EXPECT_EQ(relatedness_score(base, other), kCertainlyRelated);
}

TEST(RelatedEntriesTest, MsiProductOfTheSameApp) {
    Program base =
        make(L"Contoso App", L"1.2.3", L"Contoso Ltd", LR"(C:\Program Files\Contoso\App)", Architecture::X64);
    base.install_date = kMay1;
    Program other = make(L"Contoso App", L"1.2.3", L"Contoso Ltd");
    other.install_date = kMay1;
    EXPECT_EQ(relatedness_score(base, other), 21);
}

TEST(RelatedEntriesTest, RedistributableBundleVersusItsRuntime) {
    Program base = make(L"Microsoft Visual C++ 2015-2022 Redistributable (x64) - 14.38.33130",
                        L"14.38.33130.0", L"Microsoft Corporation", {}, Architecture::X64);
    base.install_date = kMay1;
    Program other = make(L"Microsoft Visual C++ 2022 X64 Minimum Runtime - 14.38.33130", L"14.38.33130",
                         L"Microsoft Corporation", {}, Architecture::X64);
    other.install_date = kMay3;
    EXPECT_EQ(relatedness_score(base, other), 0);
}

TEST(RelatedEntriesTest, UnrelatedPrograms) {
    EXPECT_EQ(relatedness_score(make(L"Google Chrome", {}, L"Google LLC",
                                     LR"(C:\Program Files\Google\Chrome\Application)", Architecture::X64),
                                make(L"Mozilla Firefox", {}, L"Mozilla",
                                     LR"(C:\Program Files\Mozilla Firefox)", Architecture::X64)),
              -23);
}

TEST(RelatedEntriesTest, SameNameDifferentBitness) {
    EXPECT_EQ(relatedness_score(make(L"7-Zip 23.01", L"23.01", L"Igor Pavlov",
                                     LR"(C:\Program Files (x86)\7-Zip)", Architecture::X86),
                                make(L"7-Zip 23.01 (x64)", L"23.01", L"Igor Pavlov",
                                     LR"(C:\Program Files\7-Zip)", Architecture::X64)),
              4);
}

TEST(RelatedEntriesTest, InstallFolderNamedAfterTheProgram) {
    EXPECT_EQ(
        relatedness_score(make(L"Notepad++ (64-bit x64)", L"8.6.2", L"Notepad++ Team"),
                          make(L"Notepad++", L"8.6.2", L"Notepad++ Team", LR"(C:\Program Files\Notepad++)")),
        17);
}

TEST(RelatedEntriesTest, InstalledFromInsideAnotherProgram) {
    Program base = make(L"Plugin Pack");
    base.install_source = LR"(C:\Program Files\HostApp\plugins\setup)";
    EXPECT_EQ(relatedness_score(base, make(L"Host Application", {}, {}, LR"(C:\Program Files\HostApp)")), -5);
}

TEST(RelatedEntriesTest, NestedInstallLocations) {
    EXPECT_EQ(relatedness_score(
                  make(L"Suite Component", {}, L"Fabrikam", LR"(C:\Program Files\Fabrikam\Suite\Component)"),
                  make(L"Suite", {}, L"Fabrikam", LR"(C:\Program Files\Fabrikam\Suite)")),
              -11);
}

}  // namespace
}  // namespace avel::inventory
