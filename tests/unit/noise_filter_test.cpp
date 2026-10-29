// The noise filter's default rules on a made-up machine: its Windows folder, ProgramData, drives
// and two users (one with a redirected AppData, one whose folders default to their profile), and
// on registry keys and values of both hives.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/leftovers/environment.h"
#include "core/platform/text.h"
#include "core/tracker/noise_filter.h"

namespace avel::tracker {
namespace {

constexpr wchar_t kMe[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kOther[] = L"S-1-5-21-1-2-3-1002";

class NoiseFilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        leftovers::ScanEnvironment environment;
        environment.machine_folders.windows = LR"(C:\Windows)";
        environment.machine_folders.program_data = LR"(C:\ProgramData)";
        leftovers::UserScope me;
        me.profile = {kMe, L"PC\\me", LR"(C:\Users\me)", true};
        me.folders.profile = LR"(C:\Users\me)";
        me.folders.local_app_data = LR"(C:\Users\me\AppData\Local)";
        me.folders.roaming_app_data = LR"(D:\Redirected\me\Roaming)";
        me.folders.local_low_app_data = LR"(C:\Users\me\AppData\LocalLow)";
        leftovers::UserScope other;  // Folders not read: they default to the profile's
        other.profile = {kOther, L"PC\\other", LR"(E:\Profiles\other)", false};
        environment.users = {me, other};
        rules_ = default_noise_rules(environment, {LR"(C:\Program Files\Avel)", LR"(C:\ProgramData\Avel)"});
    }

    NoiseReason file(std::wstring_view path) const { return classify_file(rules_, path); }

    NoiseReason machine_key(std::wstring path) const {
        return classify_key(rules_, {platform::Hive::Machine, L"", std::move(path)});
    }

    NoiseReason user_key(std::wstring sid, std::wstring path) const {
        return classify_key(rules_, {platform::Hive::User, std::move(sid), std::move(path)});
    }

    NoiseReason machine_value(std::wstring key, std::wstring_view name) const {
        return classify_value(rules_, {platform::Hive::Machine, L"", std::move(key)}, name);
    }

    NoiseReason user_value(std::wstring sid, std::wstring key, std::wstring_view name) const {
        return classify_value(rules_, {platform::Hive::User, std::move(sid), std::move(key)}, name);
    }

    NoiseRules rules_;
};

TEST_F(NoiseFilterTest, FollowsEachUsersOwnFolders) {
    EXPECT_EQ(file(LR"(C:\Users\me\AppData\Local\Temp\nsx1.tmp\System.dll)"), NoiseReason::Temporary);
    EXPECT_EQ(file(LR"(E:\Profiles\other\AppData\Local\Temp\setup.log)"), NoiseReason::Temporary);
    EXPECT_EQ(file(LR"(C:\Users\third\AppData\Local\Temp\setup.log)"), NoiseReason::None);  // Not a user here

    // My Roaming folder is redirected; the other user's is where Windows puts it.
    EXPECT_EQ(file(LR"(D:\Redirected\me\Roaming\Microsoft\Windows\Recent\setup.lnk)"),
              NoiseReason::UserActivity);
    EXPECT_EQ(file(LR"(C:\Users\me\AppData\Roaming\Microsoft\Windows\Recent\setup.lnk)"), NoiseReason::None);
    EXPECT_EQ(file(LR"(E:\Profiles\other\AppData\Roaming\Microsoft\Windows\Recent\setup.lnk)"),
              NoiseReason::UserActivity);

    EXPECT_EQ(file(LR"(C:\Users\me\AppData\Local\Microsoft\Windows\INetCache\IE\X1\setup[1].exe)"),
              NoiseReason::UserActivity);
    EXPECT_EQ(file(LR"(C:\Users\me\AppData\Local\Microsoft\Windows\Explorer\thumbcache_256.db)"),
              NoiseReason::UserActivity);
    EXPECT_EQ(file(LR"(C:\Users\me\AppData\Local\Microsoft\CLR_v4.0_32\UsageLogs\setup.exe.log)"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Users\me\AppData\Local\IconCache.db)"), NoiseReason::UserActivity);
    EXPECT_EQ(file(LR"(C:\Users\me\AppData\LocalLow\Microsoft\CryptnetUrlCache\Content\AB12)"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Users\me\NTUSER.DAT)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Users\me\ntuser.dat)"), NoiseReason::SystemActivity);  // As the journal spells it
    EXPECT_EQ(file(LR"(C:\Users\me\ntuser.dat.LOG1)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(E:\Profiles\other\AppData\Local\Microsoft\Windows\UsrClass.dat)"),
              NoiseReason::SystemActivity);

    // Packaged apps: their caches and temporary state, not their data.
    const std::wstring package =
        LR"(C:\Users\me\AppData\Local\Packages\Microsoft.Windows.Search_cw5n1h2txyewy)";
    EXPECT_EQ(file(package + LR"(\AC\INetCache\x)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(package + LR"(\TempState\x)"), NoiseReason::Temporary);
    EXPECT_EQ(file(package + LR"(\Settings\settings.dat)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(package + LR"(\LocalState\x)"), NoiseReason::None);

    // Browser profiles, Firefox only for its cache.
    EXPECT_EQ(file(LR"(C:\Users\me\AppData\Local\Google\Chrome\User Data\Default\Cache\f_000001)"),
              NoiseReason::UserActivity);
    EXPECT_EQ(file(LR"(C:\Users\me\AppData\Local\Microsoft\Edge Beta\User Data\Default\History)"),
              NoiseReason::UserActivity);
    EXPECT_EQ(
        file(LR"(E:\Profiles\other\AppData\Local\Mozilla\Firefox\Profiles\k2.default\cache2\entries\A1)"),
        NoiseReason::UserActivity);
    EXPECT_EQ(file(LR"(E:\Profiles\other\AppData\Local\Mozilla\Firefox\Profiles\k2.default\startupCache)"),
              NoiseReason::None);

    // What programs install for a user is real.
    EXPECT_EQ(file(LR"(C:\Users\me\AppData\Local\Programs\Contoso\contoso.exe)"), NoiseReason::None);
    EXPECT_EQ(file(LR"(D:\Redirected\me\Roaming\Contoso\settings.json)"), NoiseReason::None);
    EXPECT_EQ(file(LR"(C:\Users\me\AppData\Local\Temperature\x)"), NoiseReason::None);
}

TEST_F(NoiseFilterTest, KnowsWindowsOwnFolders) {
    EXPECT_EQ(file(LR"(C:\Windows\Temp\MSI1a2b.LOG)"), NoiseReason::Temporary);
    EXPECT_EQ(file(LR"(c:\WINDOWS\temp)"), NoiseReason::Temporary);  // Any case
    EXPECT_EQ(file(LR"(C:\Windows\Prefetch\SETUP.EXE-1A2B3C4D.pf)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Windows\INF\setupapi.dev.log)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Windows\INF\oem12.inf)"), NoiseReason::None);
    EXPECT_EQ(file(LR"(C:\Windows\bootstat.dat)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Windows\System32\winevt\Logs\Application.evtx)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Windows\System32\config\SOFTWARE)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Windows\System32\config\SOFTWARE.LOG1)"), NoiseReason::SystemActivity);
    EXPECT_EQ(
        file(LR"(C:\Windows\System32\config\TxR\{016888cc-6c6f-11de-8d1d-001e0bcde3ec}.TxR.0.regtrans-ms)"),
        NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Windows\System32\config\systemprofile\AppData\Local\Temp\x)"),
              NoiseReason::Temporary);
    EXPECT_EQ(file(LR"(C:\Windows\System32\config\systemprofile\AppData\Local\Contoso\x)"),
              NoiseReason::None);
    EXPECT_EQ(
        file(
            LR"(C:\Windows\ServiceProfiles\NetworkService\AppData\Local\Microsoft\Windows\DeliveryOptimization\Logs\x)"),
        NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Windows\ServiceProfiles\LocalService\NTUSER.DAT)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Windows\System32\drivers\contoso.sys)"), NoiseReason::None);
    EXPECT_EQ(file(LR"(C:\Windows\Installer\1a2b3c.msi)"),
              NoiseReason::None);  // Windows Installer's cache is real

    EXPECT_EQ(file(LR"(D:\$Recycle.Bin\S-1-5-21-1-2-3-1001\$R1A2B.txt)"), NoiseReason::UserActivity);
    EXPECT_EQ(file(LR"(E:\System Volume Information\tracking.log)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Config.Msi\1a2b.rbf)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\Tools\Config.Msi\x)"), NoiseReason::None);  // Only at a drive's root

    EXPECT_EQ(file(LR"(C:\ProgramData\Microsoft\Windows Defender\Scans\History\x)"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\ProgramData\USOShared\Logs\System\x.etl)"), NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Contoso.lnk)"),
              NoiseReason::None);
    EXPECT_EQ(file(LR"(C:\ProgramData\Package Cache\{0E4D5C66}\setup.exe)"), NoiseReason::None);
    EXPECT_EQ(file(LR"(C:\Program Files\Contoso\contoso.exe)"), NoiseReason::None);
}

TEST_F(NoiseFilterTest, SetsAsideAvelsOwnFilesAndKeys) {
    EXPECT_EQ(file(LR"(C:\Program Files\Avel\avel.exe)"), NoiseReason::Avel);
    EXPECT_EQ(file(LR"(C:\ProgramData\Avel\Traces\20261023-140509-1a2b3c4d\trace.json)"), NoiseReason::Avel);
    EXPECT_EQ(file(LR"(D:\$Avel.Quarantine\20261023-140509-1a2b3c4d\0001)"), NoiseReason::Avel);
    EXPECT_EQ(file(LR"(C:\Program Files\Avel Tools\x.exe)"), NoiseReason::None);
    EXPECT_EQ(machine_key(LR"(SOFTWARE\Avel)"), NoiseReason::Avel);
    EXPECT_EQ(user_key(kMe, LR"(Software\Avel\Avel)"), NoiseReason::Avel);
}

TEST_F(NoiseFilterTest, KnowsWindowsOwnKeys) {
    EXPECT_EQ(machine_key(LR"(SOFTWARE\Microsoft\Tracing\setup_RASAPI32)"), NoiseReason::SystemActivity);
    EXPECT_EQ(machine_key(LR"(SOFTWARE\WOW6432Node\Microsoft\Tracing\setup_RASMANCS)"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(machine_key(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\InProgress)"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(
        machine_key(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\UserData\S-1-5-18\Products\X)"),
        NoiseReason::None);
    EXPECT_EQ(machine_key(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso)"),
              NoiseReason::None);
    EXPECT_EQ(machine_key(LR"(SYSTEM\CurrentControlSet\Services\Tcpip6\Parameters\Interfaces\{X})"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(machine_key(LR"(SYSTEM\ControlSet001\Services\bam\State\UserSettings\S-1-5-21-1-2-3-1001)"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(machine_key(LR"(SYSTEM\CurrentControlSet\Services\Contoso\Enum)"), NoiseReason::SystemActivity);
    EXPECT_EQ(machine_key(LR"(SYSTEM\CurrentControlSet\Services\Contoso)"), NoiseReason::None);
    EXPECT_EQ(
        machine_key(
            LR"(SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\FirewallRules)"),
        NoiseReason::None);
    EXPECT_EQ(machine_key(LR"(SYSTEM\ControlSet1\Services\bam)"), NoiseReason::None);  // Not a control set

    for (const auto* sid : {kMe, kOther}) {
        EXPECT_EQ(
            user_key(sid, LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\UserAssist\{GUID}\Count)"),
            NoiseReason::UserActivity);
        EXPECT_EQ(user_key(sid, LR"(Software\Microsoft\Windows\CurrentVersion\Internet Settings\ZoneMap)"),
                  NoiseReason::SystemActivity);
    }
    EXPECT_EQ(user_key(kMe, LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\BagMRU\1)"),
              NoiseReason::UserActivity);
    EXPECT_EQ(user_key(std::wstring(kMe) + L"_Classes",
                       LR"(Local Settings\Software\Microsoft\Windows\Shell\Bags\1)"),
              NoiseReason::UserActivity);
    EXPECT_EQ(user_key(kMe, LR"(Software\Contoso)"), NoiseReason::None);
    EXPECT_EQ(user_key(kMe, LR"(Software\Microsoft\Tracing)"), NoiseReason::None);  // A machine rule
    EXPECT_EQ(user_key(kMe, LR"(Software\Microsoft\Windows\CurrentVersion\Run)"), NoiseReason::None);
}

TEST_F(NoiseFilterTest, KnowsValuesThatChangeByThemselves) {
    const std::wstring profile =
        LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\S-1-5-21-1-2-3-1001)";
    EXPECT_EQ(machine_value(profile, L"LocalProfileLoadTimeLow"), NoiseReason::SystemActivity);
    EXPECT_EQ(machine_value(profile, L"LocalProfileUnloadTimeHigh"), NoiseReason::SystemActivity);
    EXPECT_EQ(machine_value(profile, L"ProfileImagePath"), NoiseReason::None);
    EXPECT_EQ(machine_key(profile), NoiseReason::None);

    const std::wstring task =
        LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tasks\{0E4D5C66})";
    EXPECT_EQ(machine_value(task, L"DynamicInfo"), NoiseReason::SystemActivity);
    const std::wstring perflib = LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib)";
    EXPECT_EQ(machine_value(perflib, L"Last Counter"), NoiseReason::SystemActivity);
    EXPECT_EQ(machine_value(perflib, L"Last Help"), NoiseReason::SystemActivity);
    EXPECT_EQ(machine_value(perflib, L"Disable Performance Counters"), NoiseReason::None);
    EXPECT_EQ(machine_key(perflib + LR"(\_V2Providers\{0E4D5C66})"), NoiseReason::None);  // A provider

    // A value in a noise key is noise too.
    EXPECT_EQ(machine_value(LR"(SOFTWARE\Microsoft\Tracing\setup_RASAPI32)", L"EnableFileTracing"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(
        user_value(kOther, LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\RecentDocs)", L"MRUListEx"),
        NoiseReason::UserActivity);
}

// What the idle round in Windows Sandbox found Windows changing by itself.
TEST_F(NoiseFilterTest, KnowsWhatWindowsKeepsUpToDate) {
    // Performance counters, AppLocker's policy, Windows' own scheduled tasks.
    for (const auto* path :
         {LR"(C:\Windows\System32\perfc009.dat)", LR"(C:\Windows\System32\perfh019.dat)",
          LR"(C:\Windows\System32\PerfStringBackup.INI)", LR"(C:\Windows\System32\perftmp.dat)",
          LR"(C:\Windows\INF\WmiApRpl\0009\WmiApRpl.ini)",
          LR"(C:\Windows\System32\wbem\Performance\WmiApRpl.h)",
          LR"(C:\Windows\System32\AppLocker\EXE.AppLocker)",
          LR"(C:\Windows\System32\Tasks\Microsoft\Windows\Registry\rgnupdt-run)",
          LR"(C:\Windows\appcompat\pca\PcaAppLaunchDic.txt)",
          LR"(C:\Windows\AppReadiness\S-1-5-21-1-2-3-1001)",
          LR"(C:\Windows\Microsoft.NET\Framework64\v4.0.30319\ngen.log)",
          LR"(C:\Windows\ServiceProfiles\LocalService\AppData\LocalLow\Microsoft)"}) {
        EXPECT_EQ(file(path), NoiseReason::SystemActivity) << platform::to_utf8(path);
    }
    for (const auto* path :
         {LR"(C:\Windows\System32\perfcontoso.dat)", LR"(C:\Windows\System32\Tasks\Contoso\Updater)",
          LR"(C:\Windows\System32\Tasks\Microsoft\Office\Contoso)", LR"(C:\Windows\INF\oem12.inf)",
          LR"(C:\Windows\Microsoft.NET\Framework64\v4.0.30319\contoso.dll)"}) {
        EXPECT_EQ(file(path), NoiseReason::None) << platform::to_utf8(path);
    }

    // Each user's: packaged apps' WebView2 data and the shell's app index, PowerShell's cache,
    // crypt32's caches.
    const std::wstring packages = LR"(C:\Users\me\AppData\Local\Packages)";
    EXPECT_EQ(
        file(packages + LR"(\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\LocalState\EBWebView\Local State)"),
        NoiseReason::SystemActivity);
    EXPECT_EQ(
        file(packages + LR"(\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\LocalState\Search\AppsIndex.db)"),
        NoiseReason::SystemActivity);
    EXPECT_EQ(file(packages + LR"(\Contoso.App_1a2b3c4d5e6f\LocalState\EBWebView\Default\Cookies)"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(file(packages + LR"(\Contoso.App_1a2b3c4d5e6f\LocalState\Search\index.db)"), NoiseReason::None);
    EXPECT_EQ(file(packages + LR"(\Contoso.App_1a2b3c4d5e6f)"), NoiseReason::None);
    EXPECT_EQ(
        file(LR"(C:\Users\me\AppData\Local\Microsoft\Windows\PowerShell\StartupProfileData-NonInteractive)"),
        NoiseReason::SystemActivity);
    EXPECT_EQ(file(LR"(E:\Profiles\other\AppData\LocalLow\Microsoft\CryptnetFlushCache\x)"),
              NoiseReason::SystemActivity);

    // Keys: the machine's.
    for (
        const auto* key : {
            LR"(SOFTWARE\Microsoft\IdentityCRL\NegativeCache\1)",
            LR"(SOFTWARE\Microsoft\Security Center\Monitoring)",
            LR"(SOFTWARE\Microsoft\Wbem\Tracing\Providers\ScheduledTaskProv @ Root/Microsoft/Windows/TaskScheduler)",
            LR"(SOFTWARE\Microsoft\Wbem\WDM\DREDGE)",
            LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Appx\PackageState\S-1-5-21-1-2-3-1001)",
            LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\AppModel\StateRepository\Cache\Package\Data\95)",
            LR"(SOFTWARE\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel\PackageRepository)",
            LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tasks\{0E4D5C66})",
            LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Logon\{0E4D5C66})",
            LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tree\Microsoft\Windows\Registry\rgnupdt-run)",
            LR"(SOFTWARE\Policies\Microsoft\Windows\BITS)",
            LR"(SYSTEM\ControlSet001\Services\VSS\Diag\BITS Writer)",
            LR"(SYSTEM\CurrentControlSet\Services\WmiApRpl\Performance)",
        }) {
        EXPECT_EQ(machine_key(key), NoiseReason::SystemActivity) << platform::to_utf8(key);
    }
    for (const auto* key : {
             LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tree\Contoso\Updater)",
             LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tree\Microsoft\Office)",
             LR"(SOFTWARE\Policies\Microsoft\Windows\Contoso)",
             LR"(SOFTWARE\Classes\PackagedCom\Package\Contoso.App_1.0.0.0_neutral__1a2b3c4d5e6f)",
             LR"(SYSTEM\CurrentControlSet\Services\VSS\Settings)",
             LR"(SYSTEM\CurrentControlSet\Services\WmiApRpl)",
             LR"(SYSTEM\CurrentControlSet\Services\BITS)",
             LR"(SOFTWARE\Microsoft\Security Center\Provider\Av\{0E4D5C66})",  // A security product
             LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib\009)",
         }) {
        EXPECT_EQ(machine_key(key), NoiseReason::None) << platform::to_utf8(key);
    }
    EXPECT_EQ(machine_value(LR"(SOFTWARE\Microsoft\Security Center)", L"cval"), NoiseReason::SystemActivity);
    EXPECT_EQ(machine_value(LR"(SOFTWARE\Microsoft\Security Center\Provider\Fw)", L"DataMigrated"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(machine_value(LR"(SOFTWARE\Microsoft\Security Center\Provider\Av\{0E4D5C66})", L"DataMigrated"),
              NoiseReason::None);
    EXPECT_EQ(machine_value(LR"(SYSTEM\ControlSet001\Services\BITS)", L"Start"), NoiseReason::SystemActivity);
    EXPECT_EQ(machine_value(LR"(SYSTEM\CurrentControlSet\Services\BITS)", L"ImagePath"), NoiseReason::None);
    for (const auto* explorer : {LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer)",
                                 LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Explorer)"}) {
        EXPECT_EQ(machine_value(explorer, L"GlobalAssocChangedCounter"), NoiseReason::SystemActivity);
        EXPECT_EQ(machine_value(explorer, L"SmartScreenEnabled"), NoiseReason::None);
    }

    // Each user's, through their Software and their classes.
    for (
        const auto* key : {
            LR"(Software\Microsoft\IdentityCRL\Immersive\production\Token)",
            LR"(Software\Microsoft\AuthCookies\Live\Default\DIDC)",
            LR"(Software\Microsoft\Windows\CurrentVersion\CloudStore\Store\Cache\DefaultAccount)",
            LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel\Repository)",
            LR"(Software\Classes\Local Settings\MuiCache\1657\B1A07F78)",
            LR"(Software\Microsoft\Windows\CurrentVersion\IrisService\Providers\SessionContextHash\177622)",
            LR"(Software\Microsoft\Windows\CurrentVersion\Security and Maintenance\Checks)",
        }) {
        EXPECT_EQ(user_key(kOther, key), NoiseReason::SystemActivity) << platform::to_utf8(key);
    }
    EXPECT_EQ(user_value(kMe, LR"(Software\Microsoft\Windows\CurrentVersion\IrisService)", L"LastContext"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(user_key(std::wstring(kMe) + L"_Classes", LR"(Local Settings\MuiCache\1657\B1A07F78)"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(user_value(kMe, LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache)",
                         LR"(C:\Program Files\Contoso\contoso.exe.FriendlyAppName)"),
              NoiseReason::None);  // Explorer's MuiCache is never noise
    EXPECT_EQ(user_key(kMe, LR"(Software\Microsoft\Windows\CurrentVersion\Start\TileProperties)"),
              NoiseReason::None);
    const std::wstring game_bar = LR"(Software\Microsoft\Windows\CurrentVersion\GameDVR)";
    EXPECT_EQ(user_value(kMe, game_bar, L"KGLToGCSUpdatedRevision"), NoiseReason::SystemActivity);
    EXPECT_EQ(user_value(kMe, game_bar, L"AppCaptureEnabled"), NoiseReason::None);
}

// What the 5b Sandbox run saw Windows do during the installs, and during Avel's own removals.
TEST_F(NoiseFilterTest, KnowsWhatWindowsDidDuringTheSandboxsInstalls) {
    const std::wstring flags = LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags)";
    const std::wstring uninstall = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\)";
    const std::wstring fix_database = L"{22221111-1111-1111-1111-111111111111}";
    const std::wstring own_database = L"{C0C0A000-0000-0000-0000-000000000001}";
    const std::vector<std::wstring> keys = {
        flags + LR"(\Appraiser\RunResults\LastFullSyncRun)",
        flags + LR"(\Appraiser\SEC)",
        flags + LR"(\TelemetryController\AppInv)",
        flags + LR"(\TargetVersionUpgradeExperienceIndicators\UNV)",
        flags + LR"(\Shared\TargetVersionUpgradeExperienceIndicators\UNV)",
        flags + LR"(\AvTracking\History)",
        flags + LR"(\InstalledSDB\)" + fix_database,
        uninstall + fix_database + L".sdb",
        LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\OneSettings\appcompat\runtimesdbincloud)",
        LR"(SOFTWARE\Microsoft\Wbem\Transports\Decoupled\Client)",
        LR"(SOFTWARE\Microsoft\RemovalTools\MRT)",
        LR"(SYSTEM\CurrentControlSet\Services\NcbService\NCBKapiNlmCache\2)",
    };
    for (const auto& key : keys) {
        EXPECT_EQ(machine_key(key), NoiseReason::SystemActivity) << platform::to_utf8(key);
    }
    // A program's own compatibility database, and its Uninstall entry, are its own.
    EXPECT_EQ(machine_key(flags + LR"(\InstalledSDB\)" + own_database), NoiseReason::None);
    EXPECT_EQ(machine_key(uninstall + own_database + L".sdb"), NoiseReason::None);
    for (const auto& name :
         {fix_database + L".sdb", std::wstring(L"drvpatch.sdb"), std::wstring(L"PatchDbUpdateTickCount")}) {
        EXPECT_EQ(machine_value(flags, name), NoiseReason::SystemActivity) << platform::to_utf8(name);
    }
    EXPECT_EQ(machine_value(flags + LR"(\Custom\patchsdbtest.exe)", fix_database + L".sdb"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(machine_value(flags + LR"(\Custom\contoso.exe)", own_database + L".sdb"), NoiseReason::None);

    const std::vector<std::wstring> files = {
        LR"(C:\Windows\appcompat\cloudsdb\apppatch.sdb)",
        LR"(C:\Windows\appcompat\cloudsdb.cab)",
        LR"(C:\Windows\apppatch\drvpatch.sdb)",
        LR"(C:\Windows\apppatch\CustomSDB\)" + fix_database + L".sdb",
        LR"(C:\Windows\System32\appraiser\Appraiser_Data.ini)",
        LR"(C:\Windows\appcompat\appraiser\AltData\appraiser.sdb)",
        LR"(C:\Windows\appcompat\UA\GenericError.bmp)",
        LR"(C:\Windows\System32\config\systemprofile\AppData\Local\Microsoft\Windows\PowerShell\)"
        L"StartupProfileData-NonInteractive",
        LR"(C:\ProgramData\regid.1991-06.com.microsoft\regid.1991-06.com.microsoft_Windows-10-Enterprise.swidtag)",
    };
    for (const auto& path : files) {
        EXPECT_EQ(file(path), NoiseReason::SystemActivity) << platform::to_utf8(path);
    }
    EXPECT_EQ(file(LR"(C:\Windows\CbsTemp\31286386_1157142330\FoDEnum)"), NoiseReason::Temporary);
    EXPECT_EQ(file(LR"(C:\Windows\apppatch\CustomSDB\)" + own_database + L".sdb"), NoiseReason::None);
    EXPECT_EQ(file(LR"(C:\Windows\appcompat\contoso.sdb)"), NoiseReason::None);
    EXPECT_EQ(
        file(LR"(C:\ProgramData\regid.1991-06.com.microsoft\regid.1991-06.com.microsoft_Office-16.swidtag)"),
        NoiseReason::None);

    // Each user's: packages' resolved names and logos, Restart Manager's sessions, and Explorer's
    // answer to an environment change Avel announces.
    EXPECT_EQ(user_key(kMe, LR"(Software\Classes\Local Settings\MrtCache\C:%5CProgram Files%5CWindowsApps)"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(user_key(kMe, LR"(Software\Microsoft\RestartManager\Session0000)"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(user_value(kMe, LR"(Software\Microsoft\Windows NT\CurrentVersion\Winlogon)", L"ParseAutoexec"),
              NoiseReason::SystemActivity);
    EXPECT_EQ(user_value(kMe, LR"(Software\Microsoft\Windows NT\CurrentVersion\Winlogon)", L"Shell"),
              NoiseReason::None);

    // Sandbox run 2: the shell's own bookkeeping between a removal and its undo, Storage Sense in
    // the idle round. The user's Storage Sense policy, and the machine's other values, are real.
    const std::wstring current_version = LR"(Software\Microsoft\Windows\CurrentVersion)";
    for (const auto& key :
         {current_version + LR"(\RunNotification)", current_version + LR"(\Explorer\PrelaunchErrorWatcher)",
          current_version +
              LR"(\StorageSense\Parameters\CachedSizes\{BE4D5A12-0000-0000-73DE-0CBE81D53E6D})"}) {
        EXPECT_EQ(user_key(kMe, key), NoiseReason::SystemActivity) << platform::to_utf8(key);
    }
    EXPECT_EQ(user_value(kMe, current_version + LR"(\StorageSense\Parameters\StoragePolicy)", L"01"),
              NoiseReason::None);
    EXPECT_EQ(user_key(kMe, current_version + LR"(\Run)"), NoiseReason::None);
    const std::wstring storage_sense = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\StorageSense)";
    EXPECT_EQ(machine_value(storage_sense, L"BootVolFreeCap"), NoiseReason::SystemActivity);
    EXPECT_EQ(machine_value(storage_sense, L"Contoso"), NoiseReason::None);

    // Sandbox run 3: Windows' feature configuration flushing its features' usage, between a removal
    // and its undo; a key merely named like it is real.
    const std::wstring fcon = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Fcon)";
    for (const auto* name : {L"DU", L"PDU", L"PUU", L"TimeSinceLastLog"}) {
        EXPECT_EQ(machine_value(fcon, name), NoiseReason::SystemActivity) << platform::to_utf8(name);
    }
    EXPECT_EQ(machine_key(fcon + LR"(\HealthStatus)"), NoiseReason::SystemActivity);
    EXPECT_EQ(machine_key(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\FconContoso)"), NoiseReason::None);
}

// A created folder or key holding nothing but noise was made for it; one holding anything else, or
// nothing, wasn't.
TEST_F(NoiseFilterTest, ACreatedContainerHoldingOnlyNoiseIsNoise) {
    const auto created_key = [](platform::Hive hive, const wchar_t* sid, std::wstring path,
                                std::vector<leftovers::ContainedItem> inside) {
        TraceChange change;
        change.location = leftovers::RegistryKeyLocation{{hive, sid, std::move(path)}};
        change.inside = std::move(inside);
        return change;
    };
    const std::wstring flags = LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags)";
    std::vector<TraceChange> changes = {
        // A user's AppCompatFlags, created for the Compatibility Assistant's records
        created_key(platform::Hive::User, kMe,
                    LR"(Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags)",
                    {{L"Compatibility Assistant", std::nullopt, true},
                     {LR"(Compatibility Assistant\Store)", std::nullopt, true},
                     {LR"(Compatibility Assistant\Store)", LR"(C:\Setup\setup.exe)", false}}),
        // Windows' fix database naming a program to fix
        created_key(platform::Hive::Machine, L"", flags + LR"(\Custom\patchsdbtest.exe)",
                    {{L"", L"{22221111-1111-1111-1111-111111111111}.sdb", false}}),
        created_key(platform::Hive::Machine, L"", flags + LR"(\InstalledSDB)",
                    {{L"{22221111-1111-1111-1111-111111111111}", std::nullopt, true},
                     {L"{22221111-1111-1111-1111-111111111111}", L"DatabasePath", false}}),
        // A program's own database there, beside Windows'
        created_key(platform::Hive::Machine, L"", flags + LR"(\Custom\contoso.exe)",
                    {{L"", L"{22221111-1111-1111-1111-111111111111}.sdb", false},
                     {L"", L"{C0C0A000-0000-0000-0000-000000000001}.sdb", false}}),
        created_key(platform::Hive::Machine, L"", LR"(SOFTWARE\Contoso)", {}),  // Empty
        created_key(platform::Hive::Machine, L"", LR"(SOFTWARE\Fabrikam)", {}),
    };
    changes.back().inside.reset();  // Only its count kept
    TraceChange folder;
    folder.location = leftovers::FileLocation{LR"(C:\Users\me\AppData\Local\Contoso)", true};
    folder.inside = std::vector<leftovers::ContainedItem>{{L"Temp", std::nullopt, true}};
    changes.push_back(folder);
    // A packaged app's folder for nothing but its WebView2 data
    folder.location = leftovers::FileLocation{
        LR"(C:\Users\me\AppData\Local\Packages\Contoso.App_1a2b3c4d5e6f\LocalState)", true};
    folder.inside = std::vector<leftovers::ContainedItem>{{L"EBWebView", std::nullopt, true},
                                                          {LR"(EBWebView\Local State)", std::nullopt, false}};
    changes.push_back(folder);
    // Created by the Malicious Software Removal Tool, from Windows Update (seen during 7-Zip's install)
    changes.push_back(created_key(platform::Hive::Machine, L"", LR"(SOFTWARE\Microsoft\RemovalTools)",
                                  {{L"MRT", std::nullopt, true}, {L"MRT", L"GUID", false}}));

    apply_noise(rules_, changes);
    EXPECT_EQ(changes[0].noise, NoiseReason::SystemActivity);
    EXPECT_EQ(changes[1].noise, NoiseReason::SystemActivity);
    EXPECT_EQ(changes[2].noise, NoiseReason::SystemActivity);
    EXPECT_EQ(changes[3].noise, NoiseReason::None);
    EXPECT_EQ(changes[4].noise, NoiseReason::None);
    EXPECT_EQ(changes[5].noise, NoiseReason::None);
    EXPECT_EQ(changes[6].noise, NoiseReason::None);  // Contoso\Temp isn't a temp folder
    EXPECT_EQ(changes[7].noise, NoiseReason::SystemActivity);
    EXPECT_EQ(changes[8].noise, NoiseReason::SystemActivity);
}

TEST_F(NoiseFilterTest, NeverSetsAsideWhatRemovalReliesOn) {
    // A broad rule that would otherwise cover them.
    rules_.keys.push_back({platform::Hive::User, L"", LR"(software\microsoft\windows\currentversion)",
                           NoiseReason::SystemActivity});
    rules_.keys.push_back(
        {platform::Hive::User, L"", LR"(software\classes\local settings)", NoiseReason::UserActivity});

    EXPECT_EQ(user_key(kMe, LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)"),
              NoiseReason::None);
    EXPECT_EQ(user_value(kMe, LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)",
                         L"Contoso"),
              NoiseReason::None);
    EXPECT_EQ(user_key(kMe, LR"(Software\Microsoft\Windows\CurrentVersion\App Paths\contoso.exe)"),
              NoiseReason::None);
    EXPECT_EQ(user_key(kMe, LR"(Software\Microsoft\Windows\CurrentVersion\Run)"),
              NoiseReason::SystemActivity);

    const std::wstring mui_cache =
        LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache)";
    EXPECT_EQ(user_value(kMe, mui_cache, LR"(C:\Program Files\Contoso\contoso.exe.FriendlyAppName)"),
              NoiseReason::None);
    EXPECT_EQ(user_value(std::wstring(kMe) + L"_Classes",
                         LR"(Local Settings\Software\Microsoft\Windows\Shell\MuiCache)",
                         LR"(C:\Program Files\Contoso\contoso.exe.FriendlyAppName)"),
              NoiseReason::None);

    EXPECT_EQ(machine_key(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\contoso.exe)"),
              NoiseReason::None);
    EXPECT_EQ(machine_key(LR"(SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths\contoso.exe)"),
              NoiseReason::None);
}

TEST_F(NoiseFilterTest, RulesCanNameOneUser) {
    rules_.keys.push_back(
        {platform::Hive::User, kMe, LR"(software\contoso\cache)", NoiseReason::UserActivity});
    EXPECT_EQ(user_key(kMe, LR"(Software\Contoso\Cache\1)"), NoiseReason::UserActivity);
    EXPECT_EQ(user_key(kOther, LR"(Software\Contoso\Cache\1)"), NoiseReason::None);
}

TEST_F(NoiseFilterTest, AppliesToAWholeTraceAndKeepsWhatIsSetAlready) {
    std::vector<TraceChange> changes(7);
    changes[0].location = leftovers::FileLocation{LR"(C:\Users\me\AppData\Local\Temp\setup.log)", false};
    changes[1].location = leftovers::FileLocation{LR"(C:\Program Files\Contoso\contoso.exe)", false};
    changes[2].location = leftovers::RegistryKeyLocation{
        {platform::Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Tracing\setup_RASAPI32)"}};
    changes[3].location = leftovers::RegistryValueLocation{
        {platform::Hive::Machine, L"",
         LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\S-1-5-18)"},
        L"LocalProfileLoadTimeLow"};
    changes[4].location = leftovers::ServiceLocation{L"Tcpip", false};  // Created services are always real
    changes[5].location = leftovers::FileLocation{L"", false};
    changes[5].noise = NoiseReason::Unresolved;
    changes[6].location = leftovers::FileLocation{LR"(C:\Program Files\Contoso\readme.txt)", false};
    changes[6].noise = NoiseReason::UserActivity;  // Set by someone else

    apply_noise(rules_, changes);
    EXPECT_EQ(changes[0].noise, NoiseReason::Temporary);
    EXPECT_EQ(changes[1].noise, NoiseReason::None);
    EXPECT_EQ(changes[2].noise, NoiseReason::SystemActivity);
    EXPECT_EQ(changes[3].noise, NoiseReason::SystemActivity);
    EXPECT_EQ(changes[4].noise, NoiseReason::None);
    EXPECT_EQ(changes[5].noise, NoiseReason::Unresolved);
    EXPECT_EQ(changes[6].noise, NoiseReason::UserActivity);
}

}  // namespace
}  // namespace avel::tracker
