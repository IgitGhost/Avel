#include "core/tracker/noise_filter.h"

#include <algorithm>
#include <cstdint>
#include <optional>

#include "core/leftovers/environment.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"

namespace avel::tracker {

namespace {

constexpr NoiseReason kSystem = NoiseReason::SystemActivity;
constexpr NoiseReason kUser = NoiseReason::UserActivity;
constexpr NoiseReason kTemporary = NoiseReason::Temporary;
constexpr NoiseReason kAvel = NoiseReason::Avel;

struct Noise {
    std::wstring_view path;
    NoiseReason reason;
};

// Below every drive's root.
constexpr Noise kVolumeNoise[] = {
    {L"$Recycle.Bin", kUser},
    {L"System Volume Information", kSystem},
    {L"Config.Msi", kSystem},  // Windows Installer's rollback files
    {L"$Avel.Quarantine", kAvel},
};

// Below the Windows folder.
constexpr Noise kWindowsNoise[] = {
    {L"Temp", kTemporary},
    {L"SystemTemp", kTemporary},
    {L"CbsTemp", kTemporary},  // Servicing's working folders
    {L"Prefetch", kSystem},
    {L"Logs", kSystem},
    {L"SoftwareDistribution", kSystem},
    {L"ServiceState", kSystem},
    {L"bootstat.dat", kSystem},
    {L"AppReadiness", kSystem},  // Packaged apps being prepared for each user
    {LR"(appcompat\Programs)", kSystem},
    {LR"(appcompat\pca)", kSystem},  // The Program Compatibility Assistant's records
    // Windows' own compatibility fix database, which it downloads and installs by itself (seen
    // while Notepad++ installed in the Sandbox), and the compatibility appraiser's data and working
    // files, which it runs by itself (seen while Notepad++ and Inno Setup installed)
    {LR"(appcompat\cloudsdb*)", kSystem},
    {LR"(apppatch\drvpatch.sdb)", kSystem},
    {LR"(apppatch\CustomSDB\{22221111-1111-1111-1111-111111111111}.sdb)", kSystem},
    {LR"(System32\appraiser)", kSystem},
    {LR"(appcompat\appraiser)", kSystem},
    {LR"(appcompat\UA)", kSystem},
    {LR"(INF\setupapi*.log)", kSystem},
    {LR"(Microsoft.NET\Framework*\v*\ngen*.log)", kSystem},
    // The performance counters' names and help texts, which lodctr and WMI's performance adapter
    // rebuild, and the files the adapter generates
    {LR"(INF\WmiApRpl)", kSystem},
    {LR"(System32\wbem\Performance)", kSystem},
    {LR"(System32\perfc???.dat)", kSystem},
    {LR"(System32\perfd???.dat)", kSystem},
    {LR"(System32\perfh???.dat)", kSystem},
    {LR"(System32\perfi???.dat)", kSystem},
    {LR"(System32\PerfStringBackup.INI)", kSystem},
    {LR"(System32\perftmp.dat)", kSystem},
    {LR"(System32\AppLocker)", kSystem},  // AppLocker's compiled policy, which its service rewrites
    // Windows' own scheduled tasks, which it adds, changes and removes by itself
    {LR"(System32\Tasks\Microsoft\Windows)", kSystem},
    {LR"(System32\LogFiles)", kSystem},
    {LR"(System32\winevt\Logs)", kSystem},
    {LR"(System32\sru)", kSystem},
    {LR"(System32\wdi)", kSystem},
    {LR"(System32\catroot2)", kSystem},
    {LR"(System32\wbem\Repository)", kSystem},
    // The registry's hives with their logs and transaction files (not systemprofile, SYSTEM's profile)
    {LR"(System32\config\*.log*)", kSystem},
    {LR"(System32\config\*.regtrans-ms)", kSystem},
    {LR"(System32\config\*.blf)", kSystem},
    {LR"(System32\config\TxR)", kSystem},
    {LR"(System32\config\RegBack)", kSystem},
    {LR"(System32\config\SYSTEM)", kSystem},
    {LR"(System32\config\SOFTWARE)", kSystem},
    {LR"(System32\config\SAM)", kSystem},
    {LR"(System32\config\SECURITY)", kSystem},
    {LR"(System32\config\DEFAULT)", kSystem},
    {LR"(System32\config\COMPONENTS)", kSystem},
    {LR"(System32\config\DRIVERS)", kSystem},
    {LR"(System32\config\BBI)", kSystem},
    {LR"(System32\config\ELAM)", kSystem},
};

// Below the service accounts' profiles (relative to the Windows folder).
constexpr std::wstring_view kServiceProfiles[] = {LR"(ServiceProfiles\*)",
                                                  LR"(System32\config\systemprofile)"};

// Below each service account's profile: its caches, hive and temporary files.
constexpr Noise kServiceProfileNoise[] = {
    {LR"(AppData\Local\Temp)", kTemporary},
    {LR"(AppData\Local\Microsoft\Windows\DeliveryOptimization)", kSystem},
    {LR"(AppData\Local\Microsoft\Windows\INetCache)", kSystem},
    {LR"(AppData\Local\Microsoft\Windows\WebCache)", kSystem},
    {LR"(AppData\Local\Microsoft\Windows\Caches)", kSystem},
    {LR"(AppData\Local\Microsoft\Windows\UsrClass.dat*)", kSystem},
    {LR"(AppData\Local\Microsoft\Windows\PowerShell\StartupProfileData-*)", kSystem},  // A cache
    {LR"(AppData\Local\FontCache)", kSystem},
    {LR"(AppData\LocalLow)", kSystem},  // Only caches, such as crypt32's downloaded certificates
    {L"NTUSER.DAT*", kSystem},
    {L"ntuser.ini", kSystem},
};

// Below ProgramData.
constexpr Noise kProgramDataNoise[] = {
    {L"Avel", kAvel},
    {LR"(Microsoft\Windows Defender)", kSystem},
    {LR"(Microsoft\Windows\WER)", kSystem},
    {LR"(Microsoft\Search)", kSystem},
    {LR"(Microsoft\Diagnosis)", kSystem},
    {LR"(Microsoft\Network\Downloader)", kSystem},
    {LR"(Microsoft\Windows\AppRepository)", kSystem},
    {LR"(Microsoft\Windows\Caches)", kSystem},
    {LR"(Microsoft\Windows\DeviceMetadataCache)", kSystem},
    {LR"(USOShared\Logs)", kSystem},
    {L"USOPrivate", kSystem},
    // Windows' software identification tag of itself, which Windows rewrites by itself (seen while
    // Notepad++ installed in the Sandbox); other products' tags beside it are theirs
    {LR"(regid.1991-06.com.microsoft\regid.1991-06.com.microsoft_Windows-*.swidtag)", kSystem},
};

enum class UserFolder : std::uint8_t { Profile, Local, Roaming, LocalLow };

struct UserNoise {
    UserFolder folder;
    std::wstring_view path;
    NoiseReason reason;
};

// Below each user's folders.
constexpr UserNoise kUserNoise[] = {
    {UserFolder::Local, L"Temp", kTemporary},
    {UserFolder::Local, LR"(Microsoft\Windows\INetCache)", kUser},
    {UserFolder::Local, LR"(Microsoft\Windows\WebCache)", kUser},
    {UserFolder::Local, LR"(Microsoft\Windows\History)", kUser},
    {UserFolder::Local, LR"(Microsoft\Windows\Explorer)", kUser},  // Thumbnail and icon caches
    {UserFolder::Local, LR"(Microsoft\Windows\Notifications)", kSystem},
    {UserFolder::Local, LR"(Microsoft\Windows\Caches)", kSystem},
    {UserFolder::Local, LR"(Microsoft\Windows\ActionCenterCache)", kSystem},
    {UserFolder::Local, LR"(Microsoft\Windows\UsrClass.dat*)", kSystem},
    {UserFolder::Local, LR"(Microsoft\CLR_v*\UsageLogs)", kSystem},
    {UserFolder::Local, LR"(Microsoft\Windows\PowerShell\StartupProfileData-*)", kSystem},  // A cache
    {UserFolder::Local, L"ConnectedDevicesPlatform", kSystem},
    {UserFolder::Local, L"D3DSCache", kSystem},
    {UserFolder::Local, L"CrashDumps", kSystem},
    {UserFolder::Local, L"IconCache.db", kUser},
    // Packaged apps' caches and state; Windows removes them with the package
    {UserFolder::Local, LR"(Packages\*\AC)", kSystem},
    {UserFolder::Local, LR"(Packages\*\LocalCache)", kSystem},
    {UserFolder::Local, LR"(Packages\*\TempState)", kTemporary},
    {UserFolder::Local, LR"(Packages\*\Settings)", kSystem},
    // Their WebView2 data, the shell's among them, and the shell's index of apps
    {UserFolder::Local, LR"(Packages\*\LocalState\EBWebView)", kSystem},
    {UserFolder::Local, LR"(Packages\MicrosoftWindows.Client.CBS_*\LocalState\Search)", kSystem},
    // Browser profiles
    {UserFolder::Local, LR"(Microsoft\Edge*\User Data)", kUser},
    {UserFolder::Local, LR"(Google\Chrome*\User Data)", kUser},
    {UserFolder::Local, LR"(Mozilla\Firefox\Profiles\*\cache2)", kUser},
    {UserFolder::Roaming, LR"(Microsoft\Windows\Recent)", kUser},
    {UserFolder::LocalLow, LR"(Microsoft\CryptnetUrlCache)", kSystem},
    {UserFolder::LocalLow, LR"(Microsoft\CryptnetFlushCache)", kSystem},
    {UserFolder::Profile, L"NTUSER.DAT*", kSystem},
    {UserFolder::Profile, L"ntuser.ini", kSystem},
};

// Below HKLM: Windows' own state, telemetry, update and network services. Those below SOFTWARE
// also cover their WOW6432Node twins.
constexpr std::wstring_view kMachineKeyNoise[] = {
    LR"(SOFTWARE\Microsoft\Tracing)",
    LR"(SOFTWARE\Microsoft\Cryptography\RNG)",
    LR"(SOFTWARE\Microsoft\SystemCertificates\AuthRoot)",
    LR"(SOFTWARE\Microsoft\SQMClient)",
    LR"(SOFTWARE\Microsoft\PolicyManager)",
    LR"(SOFTWARE\Microsoft\WindowsUpdate)",
    LR"(SOFTWARE\Microsoft\Windows Search)",
    LR"(SOFTWARE\Microsoft\IdentityCRL)",  // The Microsoft account sign-in's caches and clock
    // Windows Security's own state (a security product's registration, in Provider, is real)
    LR"(SOFTWARE\Microsoft\Security Center\Monitoring)",
    LR"(SOFTWARE\Microsoft\Security Center\ProvidersMigration)",
    LR"(SOFTWARE\Microsoft\Security Center\Svc)",
    LR"(SOFTWARE\Microsoft\Security Center\Throttle)",
    LR"(SOFTWARE\Microsoft\Wbem\Tracing)",                // WMI providers' state, written as they load
    LR"(SOFTWARE\Microsoft\Wbem\PROVIDERS\Performance)",  // WMI's performance adapter
    LR"(SOFTWARE\Microsoft\Wbem\WDM\DREDGE)",             // WMI's list of the drivers' classes
    LR"(SOFTWARE\Microsoft\Windows\DRMitigationMarker)",
    LR"(SOFTWARE\Microsoft\Windows\Windows Error Reporting)",
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\BITS)",
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Diagnostics)",
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Reliability)",
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Group Policy)",
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate)",
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\VFUProvider)",
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\whesvc)",                // The health evaluation service
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Installer\InProgress)",  // Installer itself is real data
    // Packages' state, which Windows keeps for what is registered and removes with the package
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Appx)",
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\AppModel)",
    LR"(SOFTWARE\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel)",
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\OneSettings)",
    // The compatibility appraiser and its telemetry, which run by themselves (seen during the
    // Sandbox's installs)
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Appraiser)",
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\TelemetryController)",
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\TargetVersionUpgradeExperienceIndicators)",
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Shared\TargetVersionUpgradeExperienceIndicators)",
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\AvTracking)",
    // Windows' own compatibility fix database (a fixed id), which it downloads and installs by
    // itself, with an Uninstall entry Settings lists; a program's own database has its own id
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\InstalledSDB\{22221111-1111-1111-1111-111111111111})",
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{22221111-1111-1111-1111-111111111111}.sdb)",
    // Windows' feature configuration (fcon.dll): the usage of its staged features that processes
    // report, which its own Flighting\FeatureConfig tasks flush here, and their reporting state
    // (seen written between a removal and its undo in the Sandbox)
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Fcon)",
    LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\OneSettings)",  // Windows' settings from the cloud
    LR"(SOFTWARE\Microsoft\Wbem\Transports\Decoupled)",           // WMI's running decoupled providers
    LR"(SOFTWARE\Microsoft\RemovalTools\MRT)",  // The Malicious Software Removal Tool, from Windows Update
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Notifications)",
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Prefetcher)",
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\VolatileNotifications)",
    // The Task Scheduler's lists of its tasks by id: a task's key in Tree and its file in
    // System32\Tasks show what changed (a created task's entries belong to the task)
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tasks)",
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Plain)",
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Logon)",
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Boot)",
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Maintenance)",
    // Windows' own scheduled tasks, which it adds, changes and removes by itself
    LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tree\Microsoft\Windows)",
    LR"(SOFTWARE\Policies\Microsoft\Windows\BITS)",  // BITS makes its own when it first starts
    LR"(SYSTEM\CurrentControlSet\Services\DsmSvc\State)",
    LR"(SYSTEM\CurrentControlSet\Services\VSS\Diag)",              // Each writer's last activity
    LR"(SYSTEM\CurrentControlSet\Services\WmiApRpl\Performance)",  // WMI's performance adapter
    LR"(SYSTEM\CurrentControlSet\Services\Tcpip*)",
    LR"(SYSTEM\CurrentControlSet\Services\Dhcp)",
    LR"(SYSTEM\CurrentControlSet\Services\NlaSvc)",
    LR"(SYSTEM\CurrentControlSet\Services\iphlpsvc)",
    LR"(SYSTEM\CurrentControlSet\Services\NcbService\NCBKapiNlmCache)",  // The network broker's cache
    LR"(SYSTEM\CurrentControlSet\Services\SharedAccess\Epoch)",
    LR"(SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\RestrictedServices)",
    LR"(SYSTEM\CurrentControlSet\Services\mpssvc\Parameters\PortKeywords)",
    LR"(SYSTEM\CurrentControlSet\Services\W32Time)",
    LR"(SYSTEM\CurrentControlSet\Services\*\Enum)",  // Volatile: the devices a driver runs
    LR"(SYSTEM\CurrentControlSet\Services\bam)",
    LR"(SYSTEM\CurrentControlSet\Services\WinDefend)",
    LR"(SYSTEM\CurrentControlSet\Services\Sense)",
    LR"(SYSTEM\CurrentControlSet\Services\wuauserv)",
    LR"(SYSTEM\CurrentControlSet\Services\DoSvc)",
    LR"(SYSTEM\CurrentControlSet\Services\UsoSvc)",
};

// Below each user's hive.
constexpr Noise kUserKeyNoise[] = {
    {LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\UserAssist)", kUser},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\RecentDocs)", kUser},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\FeatureUsage)", kUser},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\ComDlg32)", kUser},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\RunMRU)", kUser},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\TypedPaths)", kUser},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\SessionInfo)", kUser},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\Streams)", kUser},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Search)", kSystem},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Notifications)", kSystem},
    {LR"(Software\Microsoft\Windows\CurrentVersion\PushNotifications)", kSystem},
    {LR"(Software\Microsoft\Windows\CurrentVersion\ContentDeliveryManager)", kSystem},
    {LR"(Software\Microsoft\Windows\CurrentVersion\IrisService)", kSystem},  // Windows' content delivery
    // Security and Maintenance's (once Action Center's) state of its checks and notifications
    {LR"(Software\Microsoft\Windows\CurrentVersion\Security and Maintenance)", kSystem},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Shell Extensions\Cached)", kSystem},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Internet Settings\Connections)", kSystem},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Internet Settings\ZoneMap)", kSystem},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Internet Settings\5.0\Cache)", kSystem},
    {LR"(Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Compatibility Assistant)", kSystem},
    {LR"(Software\Microsoft\IdentityCRL)", kSystem},  // The Microsoft account sign-in's caches
    {LR"(Software\Microsoft\AuthCookies)", kSystem},
    {LR"(Software\Microsoft\Windows\Winlogon\PasswordExpiryNotification)", kSystem},
    // The Start menu's and Settings' synced state, and packages' state (see the machine's)
    {LR"(Software\Microsoft\Windows\CurrentVersion\CloudStore)", kSystem},
    {LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel)", kSystem},
    // Resource strings Windows caches (Explorer's MuiCache is Local Settings\...\Shell\MuiCache),
    // and packages' names and logos it resolves (MrtCache; Avel's own inventory, reading them for
    // the packaged apps, fills it too: seen during `force` in the Sandbox)
    {LR"(Software\Classes\Local Settings\MuiCache)", kSystem},
    {LR"(Software\Classes\Local Settings\MrtCache)", kSystem},
    {LR"(Software\Microsoft\RestartManager)", kSystem},  // Restart Manager's sessions, for any installer
    // The shell's own bookkeeping, which it writes by itself (seen between a removal and its undo
    // in the Sandbox): its notifications of apps newly added to startup, and its watch over apps it
    // launches ahead of time
    {LR"(Software\Microsoft\Windows\CurrentVersion\RunNotification)", kSystem},
    {LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\PrelaunchErrorWatcher)", kSystem},
    // Storage settings' cache of what folders take up (the user's Storage Sense policy beside it is real)
    {LR"(Software\Microsoft\Windows\CurrentVersion\StorageSense\Parameters\CachedSizes)", kSystem},
    // Explorer's folder views
    {LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\Bags)", kUser},
    {LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\BagMRU)", kUser},
    {LR"(Software\Microsoft\Windows\Shell\Bags)", kUser},
    {LR"(Software\Microsoft\Windows\Shell\BagMRU)", kUser},
};

struct ValueNoise {
    platform::Hive hive;
    std::wstring_view key;
    std::wstring_view name;
    NoiseReason reason;
};

// Values that change by themselves in keys that otherwise hold real data.
constexpr ValueNoise kValueNoise[] = {
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\*)",
     L"LocalProfileLoadTime*", kSystem},
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\*)",
     L"LocalProfileUnloadTime*", kSystem},
    // A scheduled task's last and next run
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tasks\*)",
     L"DynamicInfo", kSystem},
    // The last performance counter numbers handed out
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib)", L"Last Counter",
     kSystem},
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib)", L"Last Help",
     kSystem},
    // Windows Security noting that it moved a product's registration
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Security Center)", L"cval", kSystem},
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Security Center\Provider\*)", L"DataMigrated", kSystem},
    // BITS making itself start delayed when it first runs
    {platform::Hive::Machine, LR"(SYSTEM\CurrentControlSet\Services\BITS)", L"Start", kSystem},
    {platform::Hive::Machine, LR"(SYSTEM\CurrentControlSet\Services\BITS)", L"DelayedAutostart", kSystem},
    // The Game Bar's list of known games, refreshed from the network
    {platform::Hive::User, LR"(Software\Microsoft\Windows\CurrentVersion\GameDVR)",
     L"KGLToGCSUpdatedRevision", kSystem},
    // The shell's count of association changes, bumped for whoever announces one (SHChangeNotify
    // with SHCNE_ASSOCCHANGED), so that caches of them are read again
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer)",
     L"GlobalAssocChangedCounter", kSystem},
    // Explorer's answer to an environment change being announced (WM_SETTINGCHANGE "Environment")
    {platform::Hive::User, LR"(Software\Microsoft\Windows NT\CurrentVersion\Winlogon)", L"ParseAutoexec",
     kSystem},
    // Storage Sense's last reading of the system volume's free space (seen in the Sandbox's idle round)
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\StorageSense)", L"BootVolFreeCap",
     kSystem},
    // Windows' own compatibility fix database being installed (see kMachineKeyNoise)
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags)",
     L"{22221111-1111-1111-1111-111111111111}.sdb", kSystem},
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags)",
     L"drvpatch.sdb", kSystem},
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags)",
     L"PatchDbUpdateTickCount", kSystem},
    {platform::Hive::Machine, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Custom\*)",
     L"{22221111-1111-1111-1111-111111111111}.sdb", kSystem},
};

// Never noise, in either hive, whatever else matches: a program's removal relies on them.
constexpr std::wstring_view kNeverNoise[] = {
    LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\MuiCache)",
    LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved)",
    LR"(Software\Microsoft\Windows\CurrentVersion\App Paths)",
};

constexpr std::wstring_view kAvelKey = LR"(Software\Avel)";

// HKEY_USERS\<sid>_Classes: the user's classes, which their hive also shows as Software\Classes.
constexpr std::wstring_view kClassesSuffix = L"_Classes";

// `text` matches `pattern`, where '*' stands for any run of characters and '?' for any one.
bool wildcard_match(std::wstring_view pattern, std::wstring_view text) {
    size_t p = 0;
    size_t t = 0;
    size_t star = std::wstring_view::npos;
    size_t resume = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == L'?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == L'*') {
            star = p++;
            resume = t;
        } else if (star != std::wstring_view::npos) {
            p = star + 1;  // Let the last '*' take one more character
            t = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == L'*') {
        ++p;
    }
    return p == pattern.size();
}

// `text` is what `pattern` names, or, with `subtree`, also below it; both are lowercase paths and
// each component is matched on its own.
bool pattern_matches(std::wstring_view pattern, std::wstring_view text, bool subtree) {
    for (;;) {
        const size_t pattern_end = pattern.find(L'\\');
        const size_t text_end = text.find(L'\\');
        if (!wildcard_match(pattern.substr(0, pattern_end), text.substr(0, text_end))) {
            return false;
        }
        if (pattern_end == std::wstring_view::npos) {
            return subtree || text_end == std::wstring_view::npos;
        }
        if (text_end == std::wstring_view::npos) {
            return false;
        }
        pattern.remove_prefix(pattern_end + 1);
        text.remove_prefix(text_end + 1);
    }
}

std::wstring file_pattern(std::wstring_view folder, std::wstring_view relative) {
    return platform::to_lower(
        platform::normalize_path(std::wstring(folder) + L"\\" + std::wstring(relative)));
}

// "SOFTWARE\X" also exists as "SOFTWARE\WOW6432Node\X" for 32-bit programs (but not Classes).
std::wstring wow64_twin(std::wstring_view path) {
    constexpr std::wstring_view kSoftware = LR"(software\)";
    if (!platform::istarts_with(path, kSoftware) || platform::istarts_with(path, LR"(software\classes\)")) {
        return {};
    }
    return LR"(software\wow6432node\)" + std::wstring(path.substr(kSoftware.size()));
}

void add_key(std::vector<KeyRule>& rules, platform::Hive hive, std::wstring_view path, NoiseReason reason) {
    rules.push_back({hive, L"", platform::to_lower(path), reason});
    if (hive == platform::Hive::Machine) {
        if (std::wstring twin = wow64_twin(rules.back().pattern); !twin.empty()) {
            rules.push_back({hive, L"", std::move(twin), reason});
        }
    }
}

std::wstring user_folder(const leftovers::UserScope& user, UserFolder which) {
    const auto& folders = user.folders;
    const std::wstring profile =
        folders.profile.empty() ? platform::normalize_path(user.profile.profile_path) : folders.profile;
    // Tests and partly read profiles may leave the AppData folders out; they default to the profile's.
    const auto or_default = [&](const std::wstring& folder, std::wstring_view fallback) {
        if (!folder.empty() || profile.empty()) {
            return folder;
        }
        return profile + L"\\" + std::wstring(fallback);
    };
    switch (which) {
        case UserFolder::Profile:
            return profile;
        case UserFolder::Local:
            return or_default(folders.local_app_data, LR"(AppData\Local)");
        case UserFolder::Roaming:
            return or_default(folders.roaming_app_data, LR"(AppData\Roaming)");
        case UserFolder::LocalLow:
            return or_default(folders.local_low_app_data, LR"(AppData\LocalLow)");
    }
    return {};
}

// A key as the rules name it: lowercase, CurrentControlSet for any ControlSet00n, and a user's
// classes below their Software\Classes.
struct KeyName {
    platform::Hive hive = platform::Hive::Machine;
    std::wstring_view sid;
    std::wstring path;
};

KeyName key_name(const platform::RegistryAddress& key) {
    KeyName name{key.hive, key.sid, platform::to_lower(key.path)};
    if (key.hive == platform::Hive::Machine) {
        constexpr std::wstring_view kControlSet = LR"(system\controlset)";
        const size_t end = kControlSet.size() + 3;
        if (name.path.starts_with(kControlSet) && name.path.size() >= end &&
            std::all_of(name.path.begin() + kControlSet.size(), name.path.begin() + end,
                        [](wchar_t c) { return c >= L'0' && c <= L'9'; }) &&
            (name.path.size() == end || name.path[end] == L'\\')) {
            name.path = LR"(system\currentcontrolset)" + name.path.substr(end);
        }
    } else if (platform::iends_with(key.sid, kClassesSuffix)) {
        name.sid = std::wstring_view(key.sid).substr(0, key.sid.size() - kClassesSuffix.size());
        name.path =
            name.path.empty() ? std::wstring(LR"(software\classes)") : LR"(software\classes\)" + name.path;
    }
    return name;
}

bool key_rule_matches(const KeyRule& rule, const KeyName& key, bool subtree) {
    return rule.hive == key.hive && (rule.sid.empty() || platform::iequals(rule.sid, key.sid)) &&
           pattern_matches(rule.pattern, key.path, subtree);
}

bool never_noise(const NoiseRules& rules, const KeyName& key) {
    return std::any_of(rules.never.begin(), rules.never.end(),
                       [&](const auto& rule) { return key_rule_matches(rule, key, true); });
}

NoiseReason classify_key_name(const NoiseRules& rules, const KeyName& key) {
    for (const auto& rule : rules.keys) {
        if (key_rule_matches(rule, key, true)) {
            return rule.reason;
        }
    }
    return NoiseReason::None;
}

}  // namespace

NoiseRules default_noise_rules(const leftovers::ScanEnvironment& environment,
                               const std::vector<std::wstring>& avel_paths) {
    NoiseRules rules;
    for (const auto& path : avel_paths) {
        if (const std::wstring normalized = platform::normalize_path(path);
            platform::is_absolute_path(normalized)) {
            rules.files.push_back({platform::to_lower(normalized), kAvel});
        }
    }
    for (const auto& [path, reason] : kVolumeNoise) {
        rules.files.push_back({platform::to_lower(L"?:\\" + std::wstring(path)), reason});  // On every drive
    }
    if (const auto& windows = environment.machine_folders.windows; !windows.empty()) {
        for (const auto& [path, reason] : kWindowsNoise) {
            rules.files.push_back({file_pattern(windows, path), reason});
        }
        for (const auto profile : kServiceProfiles) {
            for (const auto& [path, reason] : kServiceProfileNoise) {
                rules.files.push_back(
                    {file_pattern(windows, std::wstring(profile) + L"\\" + std::wstring(path)), reason});
            }
        }
    }
    if (const auto& program_data = environment.machine_folders.program_data; !program_data.empty()) {
        for (const auto& [path, reason] : kProgramDataNoise) {
            rules.files.push_back({file_pattern(program_data, path), reason});
        }
    }
    for (const auto& user : environment.users) {
        for (const auto& [which, path, reason] : kUserNoise) {
            if (const std::wstring folder = user_folder(user, which); !folder.empty()) {
                rules.files.push_back({file_pattern(folder, path), reason});
            }
        }
    }

    // The same user rules serve every user's hive.
    for (const auto hive : {platform::Hive::Machine, platform::Hive::User}) {
        add_key(rules.keys, hive, kAvelKey, kAvel);
        for (const auto path : kNeverNoise) {
            add_key(rules.never, hive, path, NoiseReason::None);
        }
    }
    for (const auto path : kMachineKeyNoise) {
        add_key(rules.keys, platform::Hive::Machine, path, kSystem);
    }
    for (const auto& [path, reason] : kUserKeyNoise) {
        add_key(rules.keys, platform::Hive::User, path, reason);
    }
    for (const auto& [hive, key, name, reason] : kValueNoise) {
        std::vector<KeyRule> keys;
        add_key(keys, hive, key, reason);
        for (auto& rule : keys) {
            rules.values.push_back({std::move(rule), platform::to_lower(name)});
        }
    }
    return rules;
}

NoiseReason classify_file(const NoiseRules& rules, std::wstring_view path) {
    if (path.empty()) {
        return NoiseReason::None;
    }
    const std::wstring lower = platform::to_lower(path);
    for (const auto& rule : rules.files) {
        if (pattern_matches(rule.pattern, lower, true)) {
            return rule.reason;
        }
    }
    return NoiseReason::None;
}

NoiseReason classify_key(const NoiseRules& rules, const platform::RegistryAddress& key) {
    const KeyName name = key_name(key);
    return never_noise(rules, name) ? NoiseReason::None : classify_key_name(rules, name);
}

NoiseReason classify_value(const NoiseRules& rules, const platform::RegistryAddress& key,
                           std::wstring_view value_name) {
    const KeyName name = key_name(key);
    if (never_noise(rules, name)) {
        return NoiseReason::None;
    }
    // Value names are matched whole: they may hold backslashes (MuiCache names files).
    const std::wstring lower = platform::to_lower(value_name);
    for (const auto& rule : rules.values) {
        if (key_rule_matches(rule.key, name, false) && wildcard_match(rule.name, lower)) {
            return rule.key.reason;
        }
    }
    return classify_key_name(rules, name);
}

namespace {

// A created folder or key that holds nothing but noise was made to hold it (Windows creating a
// user's AppCompatFlags for the Compatibility Assistant's records): the first item's reason.
NoiseReason classify_contents(const NoiseRules& rules, const TraceChange& change) {
    if (change.change != ChangeKind::Created || !change.inside || change.inside->empty()) {
        return NoiseReason::None;
    }
    std::optional<NoiseReason> first;
    for (const auto& item : *change.inside) {
        NoiseReason reason = NoiseReason::None;
        if (const auto* file = std::get_if<leftovers::FileLocation>(&change.location)) {
            reason = classify_file(rules, file->path + L"\\" + item.path);
        } else if (const auto* key = std::get_if<leftovers::RegistryKeyLocation>(&change.location)) {
            platform::RegistryAddress address = key->key;
            if (!item.path.empty()) {
                address.path += L"\\" + item.path;
            }
            reason = item.value ? classify_value(rules, address, *item.value) : classify_key(rules, address);
        }
        if (reason == NoiseReason::None) {
            return NoiseReason::None;
        }
        first = first.value_or(reason);
    }
    return first.value_or(NoiseReason::None);
}

}  // namespace

void apply_noise(const NoiseRules& rules, std::vector<TraceChange>& changes) {
    for (auto& change : changes) {
        if (change.noise != NoiseReason::None) {
            continue;
        }
        if (const auto* file = std::get_if<leftovers::FileLocation>(&change.location)) {
            change.noise = classify_file(rules, file->path);
        } else if (const auto* key = std::get_if<leftovers::RegistryKeyLocation>(&change.location)) {
            change.noise = classify_key(rules, key->key);
        } else if (const auto* value = std::get_if<leftovers::RegistryValueLocation>(&change.location)) {
            change.noise = classify_value(rules, value->key, value->value_name);
        }
        if (change.noise == NoiseReason::None) {
            change.noise = classify_contents(rules, change);
        }
    }
}

}  // namespace avel::tracker
