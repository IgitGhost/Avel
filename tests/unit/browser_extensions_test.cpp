// The browser extensions list against a fixture: Chromium User Data folders, an Opera profile and
// Firefox profiles in a temporary folder tree, and policies and registrations in scratch keys standing
// in for HKLM and a user's hive. Nothing outside them is read or written.
#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/leftovers/protection.h"
#include "core/platform/paths.h"
#include "core/platform/text.h"
#include "core/tools/browser_extensions.h"
#include "file_tree.h"
#include "scratch_registry.h"
#include "temp_root.h"

namespace avel::tools {
namespace {

namespace fs = std::filesystem;
using platform::Hive;
using platform::RegistryAddress;

constexpr wchar_t kSid[] = L"S-1-5-21-1-2-3-1001";
// Chromium ids: 32 letters a-p.
constexpr wchar_t kAlpha[] = L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr wchar_t kBeta[] = L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr wchar_t kComponent[] = L"cccccccccccccccccccccccccccccccc";
constexpr wchar_t kDeveloper[] = L"dddddddddddddddddddddddddddddddd";
constexpr wchar_t kForced[] = L"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
constexpr wchar_t kEdgeOwn[] = L"ffffffffffffffffffffffffffffffff";
constexpr wchar_t kOpera[] = L"gggggggggggggggggggggggggggggggg";
constexpr wchar_t kNotYet[] = L"hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh";
constexpr wchar_t kSubkey[] = L"iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii";
constexpr char kStoreUrl[] = "https://clients2.google.com/service/update2/crx";

class BrowserExtensionsTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = testing::long_temp_directory() /
                (L"avel-test-" + std::to_wstring(::GetCurrentProcessId()) + L"-extensions");
        fs::remove_all(root_);
        local_ = path(LR"(Users\fixture\AppData\Local)");
        roaming_ = path(LR"(Users\fixture\AppData\Roaming)");
        build_chrome();
        build_edge();
        build_opera();
        build_firefox();
        build_registry();
        file(LR"(PF\Google\Chrome\Application\chrome.exe)", "fixture");
        file(LR"(Firefox\firefox.exe)", "fixture");
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    std::wstring path(std::wstring_view relative) const {
        return platform::normalize_path((root_ / relative).wstring());
    }

    void file(std::wstring_view relative, std::string_view content) const {
        testing::write_file(path(relative), content);
    }

    // A path inside a JSON string.
    static std::string json_text(const std::wstring& text) {
        std::string escaped;
        for (const char c : platform::to_utf8(text)) {
            escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
        }
        return escaped;
    }

    void build_chrome() const {
        const std::wstring data = LR"(Users\fixture\AppData\Local\Google\Chrome\User Data\)";
        file(data + L"Local State",
             R"({"profile":{"info_cache":{"Default":{"name":"Person A"},"Profile 1":{"name":"Work"}}}})");
        // Preferences and Secure Preferences each hold part of extensions.settings.
        file(data + LR"(Default\Preferences)",
             std::string(R"({"extensions":{"settings":{)") +
                 R"("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{"location":1,"from_webstore":true,"state":1,)"
                 R"("path":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\1.0_0"},)"
                 R"("cccccccccccccccccccccccccccccccc":{"location":5,"path":"x"},)"
                 R"("dddddddddddddddddddddddddddddddd":{"location":4,"path":")" +
                 json_text(path(L"Dev")) + R"(","state":1}}}})");
        file(data + LR"(Default\Secure Preferences)",
             R"({"extensions":{"settings":{)"
             R"("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{"install_time":"13300000000000000"},)"
             R"("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb":{"location":3,"disable_reasons":[1],)"
             R"("path":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\\2.0_0"}}}})");
        file(data + LR"(Default\Extensions\aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\1.0_0\manifest.json)",
             std::string(R"({"manifest_version":2,"name":"__MSG_AppName__","description":"__MSG_missing__",)"
                         R"("default_locale":"en","version":"1.0","update_url":")") +
                 kStoreUrl + R"(","permissions":["tabs","https://*/*",{"socket":["x"]}]})");
        file(data + LR"(Default\Extensions\aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\1.0_0\_locales\en\messages.json)",
             R"({"appname":{"message":"Alpha"}})");
        file(data + LR"(Default\Extensions\bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\2.0_0\manifest.json)",
             R"({"manifest_version":3,"name":"Beta","version":"2.0","permissions":["storage"],)"
             R"("host_permissions":["<all_urls>"],"optional_permissions":["history"]})");
        file(LR"(Dev\manifest.json)", R"({"manifest_version":3,"name":"Developer","version":"0.1"})");
        file(data + LR"(Profile 1\Preferences)",
             R"({"extensions":{"settings":{"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee":{"location":9,)"
             R"("path":"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\\3.0_0"}}}})");
        file(data + LR"(Profile 1\Extensions\eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\3.0_0\manifest.json)",
             R"({"manifest_version":3,"name":"Forced","version":"3.0"})");
        // A theme isn't an extension.
        file(
            data + LR"(Profile 1\Secure Preferences)",
            R"({"extensions":{"settings":{"jjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjj":{"location":1,"path":"j\\1"}}}})");
        file(data + LR"(Profile 1\Extensions\j\1\manifest.json)", R"({"name":"Theme","theme":{}})");
    }

    // No Local State: the profiles are the folders with Preferences.
    void build_edge() const {
        const std::wstring data = LR"(Users\fixture\AppData\Local\Microsoft\Edge\User Data\)";
        file(
            data + LR"(Default\Preferences)",
            R"({"extensions":{"settings":{"ffffffffffffffffffffffffffffffff":{"location":1,"from_webstore":true,)"
            R"("manifest":{"name":"Edge Own","version":"5"},"path":"ffffffffffffffffffffffffffffffff\\5_0"}}}})");
        file(data + LR"(System Profile\Preferences)", "{}");
    }

    void build_opera() const {
        file(
            LR"(Users\fixture\AppData\Roaming\Opera Software\Opera Stable\Preferences)",
            R"({"extensions":{"settings":{"gggggggggggggggggggggggggggggggg":{"location":1,"state":0,"from_webstore":true,)"
            R"("manifest":{"name":"Opera Thing","version":"1"},"path":"gggggggggggggggggggggggggggggggg\\1_0"}}}})");
    }

    void build_firefox() const {
        const std::wstring base = LR"(Users\fixture\AppData\Roaming\Mozilla\Firefox\)";
        file(base + L"profiles.ini",
             "[General]\r\nStartWithLastProfile=1\r\n\r\n[Profile0]\r\nName=default-release\r\n"
             "IsRelative=1\r\nPath=Profiles/abc.default-release\r\nDefault=1\r\n\r\n"
             "[Profile1]\r\nName=Elsewhere\r\nIsRelative=0\r\nPath=" +
                 platform::to_utf8(path(L"FirefoxElsewhere")) + "\r\n");
        file(base + LR"(Profiles\abc.default-release\extensions.json)", R"({"schemaVersion":36,"addons":[
            {"id":"store@example.com","type":"extension","location":"app-profile","active":true,"signedState":2,
             "version":"1.2","defaultLocale":{"name":"Store One","description":"From AMO"},
             "installTelemetryInfo":{"source":"amo"},"installDate":1700000000000,
             "sourceURI":"https://addons.mozilla.org/firefox/downloads/file/1/store.xpi",
             "userPermissions":{"permissions":["tabs"],"origins":["<all_urls>"]},
             "optionalPermissions":{"permissions":["history"],"origins":[]}},
            {"id":"builtin@mozilla.org","type":"extension","location":"app-builtin","active":true},
            {"id":"theme@example.com","type":"theme","location":"app-profile","active":true},
            {"id":"registry@example.com","type":"extension","location":"winreg-app-global","active":true,
             "defaultLocale":{"name":"Registered"},"path":"C:\\Contoso\\registry.xpi"},
            {"id":"policy@example.com","type":"extension","location":"app-profile","active":false,"userDisabled":true,
             "installTelemetryInfo":{"source":"enterprise-policy"},"defaultLocale":{"name":"By Policy"},
             "sourceURI":"https://example.com/addons/policy%20one.xpi"},
            {"id":"temp@example.com","type":"extension","location":"app-temporary","active":true,
             "defaultLocale":{"name":"Temporary"}},
            {"id":"json@example.com","type":"extension","location":"app-profile","active":true,
             "installTelemetryInfo":{"source":"enterprise-policy"},"defaultLocale":{"name":"From JSON"}}]})");
        file(LR"(FirefoxElsewhere\prefs.js)", "");
        file(LR"(Firefox\distribution\policies.json)",
             R"({"policies":{"ExtensionSettings":{"json@example.com":{"installation_mode":"force_installed",)"
             R"("install_url":"https://example.com/json.xpi"},"*":{"installation_mode":"blocked"}},)"
             R"("Extensions":{"Install":["https://example.com/other.xpi"],"Locked":["json@example.com"]}}})");
    }

    void build_registry() const {
        const std::wstring chrome = LR"(SOFTWARE\Policies\Google\Chrome)";
        machine_.set_string(chrome + L"\\ExtensionInstallForcelist", L"1",
                            std::wstring(kForced) + L";https://example.com/update");
        machine_.set_string(chrome + L"\\ExtensionInstallForcelist", L"2", kNotYet);
        machine_.set_string(chrome + L"\\ExtensionInstallForcelist", L"x", L"not a list entry");
        machine_.set_string(
            chrome, L"ExtensionSettings",
            std::wstring(LR"({"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb, zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz":)"
                         LR"({"installation_mode":"force_installed","update_url":"u"},)"
                         LR"("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":{"installation_mode":"allowed"}})"));
        machine_.set_string(std::wstring(LR"(SOFTWARE\WOW6432Node\Google\Chrome\Extensions\)") + kBeta,
                            L"update_url", platform::to_utf16(kStoreUrl));
        machine_.set_string(LR"(SOFTWARE\Mozilla\Firefox\Extensions)", L"registry@example.com",
                            LR"(C:\Contoso\registry.xpi)");
        machine_.set_string(LR"(SOFTWARE\Policies\Mozilla\Firefox\Extensions\Install)", L"1",
                            L"https://example.com/addons/policy one.xpi");
        machine_.set_string(LR"(SOFTWARE\Policies\Mozilla\Firefox\Extensions\Install)", L"2",
                            L"https://example.com/unknown.xpi");
        // The user's own Edge policy, in the subkey form.
        user_.set_string(std::wstring(LR"(Software\Policies\Microsoft\Edge\ExtensionSettings\)") + kSubkey,
                         L"installation_mode", L"normal_installed");
        user_.set_string(std::wstring(LR"(Software\Policies\Microsoft\Edge\ExtensionSettings\)") + kSubkey,
                         L"update_url", L"https://edge.microsoft.com/extensionwebstorebase/v1/crx");
    }

    BrowserExtensionsEnvironment environment() const {
        BrowserExtensionsEnvironment environment;
        auto& scan = environment.scan;
        scan.machine = machine_.get();
        scan.machine_folders.program_files = path(L"PF");
        scan.machine_folders.program_files_x86 = path(L"PF86");
        leftovers::UserScope user;
        user.profile = {kSid, L"PC\\fixture", path(LR"(Users\fixture)"), true};
        user.hive = user_.get();
        user.folders.profile = user.profile.profile_path;
        user.folders.local_app_data = local_;
        user.folders.roaming_app_data = roaming_;
        scan.users.push_back(std::move(user));
        scan.current_user_sid = kSid;
        environment.firefox_folders = std::vector<std::wstring>{path(L"Firefox")};
        environment.managed = false;
        return environment;
    }

    static const BrowserExtension* find(const BrowserExtensionsList& list, BrowserKind browser,
                                        std::wstring_view extension_id) {
        for (const auto& entry : list.extensions) {
            if (entry.browser == browser && entry.extension_id == extension_id) {
                return &entry;
            }
        }
        return nullptr;
    }

    static std::vector<ExtensionSourceKind> kinds(const BrowserExtensionsList& list,
                                                  const BrowserExtension& entry) {
        std::vector<ExtensionSourceKind> result;
        for (const std::size_t s : entry.sources) {
            result.push_back(list.sources[s].kind);
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    fs::path root_;
    std::wstring local_;
    std::wstring roaming_;
    testing::ScratchKey machine_;
    testing::ScratchKey user_;
};

TEST_F(BrowserExtensionsTest, ListsTheProfilesOfEveryBrowser) {
    const auto list = list_browser_extensions(environment());
    std::vector<std::wstring> profiles;
    for (const auto& profile : list.profiles) {
        profiles.push_back(platform::to_utf16(to_string(profile.browser)) + L":" + profile.directory + L":" +
                           profile.name);
    }
    std::sort(profiles.begin(), profiles.end());
    EXPECT_EQ(profiles,
              (std::vector<std::wstring>{L"chrome:Default:Person A", L"chrome:Profile 1:Work",
                                         L"edge:Default:Default", L"firefox:FirefoxElsewhere:Elsewhere",
                                         L"firefox:abc.default-release:default-release", L"opera::"}));
    for (const auto& profile : list.profiles) {
        if (profile.browser == BrowserKind::Chrome && profile.directory == L"Profile 1") {
            EXPECT_EQ(profile.page.executable, path(LR"(PF\Google\Chrome\Application\chrome.exe)"));
            EXPECT_EQ(profile.page.arguments,
                      (std::vector<std::wstring>{L"--profile-directory=Profile 1", L"chrome://extensions"}));
        }
        if (profile.browser == BrowserKind::Firefox && profile.name == L"Elsewhere") {
            EXPECT_EQ(profile.path, path(L"FirefoxElsewhere"));
            EXPECT_EQ(profile.page.executable, path(LR"(Firefox\firefox.exe)"));
            EXPECT_EQ(profile.page.arguments,
                      (std::vector<std::wstring>{L"-profile", path(L"FirefoxElsewhere"), L"about:addons"}));
        }
        if (profile.browser == BrowserKind::Opera) {
            EXPECT_EQ(profile.path, path(LR"(Users\fixture\AppData\Roaming\Opera Software\Opera Stable)"));
            EXPECT_TRUE(profile.page.executable.empty());
            EXPECT_EQ(profile.page.arguments, std::vector<std::wstring>{L"opera://extensions"});
        }
    }
    EXPECT_TRUE(list.diagnostics.empty());
    EXPECT_FALSE(list.policies_managed);
}

TEST_F(BrowserExtensionsTest, ReadsChromiumExtensionsFromBothPreferencesFiles) {
    const auto list = list_browser_extensions(environment());
    const auto* alpha = find(list, BrowserKind::Chrome, kAlpha);
    ASSERT_NE(alpha, nullptr);
    EXPECT_EQ(alpha->name, L"Alpha");                   // __MSG_AppName__, keys ignore case
    EXPECT_EQ(alpha->description, L"__MSG_missing__");  // No such message: kept as written
    EXPECT_EQ(alpha->version, L"1.0");
    EXPECT_EQ(alpha->profile_name, L"Person A");
    EXPECT_TRUE(alpha->enabled);
    EXPECT_EQ(alpha->install, ExtensionInstall::Store);
    EXPECT_EQ(alpha->store, ExtensionStore::ChromeWebStore);
    EXPECT_TRUE(alpha->from_store);
    EXPECT_EQ(alpha->install_time, 13300000000000LL - 11644473600000LL);  // From Secure Preferences
    EXPECT_EQ(alpha->permissions, std::vector<std::wstring>{L"tabs"});
    EXPECT_EQ(alpha->host_permissions, std::vector<std::wstring>{L"https://*/*"});  // MV2: a match pattern
    EXPECT_EQ(alpha->folder,
              path(LR"(Users\fixture\AppData\Local\Google\Chrome\User Data\Default\Extensions\)"
                   LR"(aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\1.0_0)"));
    EXPECT_TRUE(alpha->sources.empty());  // "allowed" isn't an install
    EXPECT_EQ(alpha->removal, ExtensionRemoval::NotRemovable);
    EXPECT_EQ(alpha->id, browser_extension_id(BrowserKind::Chrome, kSid, alpha->profile_path, kAlpha));
    EXPECT_EQ(alpha->id.size(), 10u);

    const auto* beta = find(list, BrowserKind::Chrome, kBeta);
    ASSERT_NE(beta, nullptr);
    EXPECT_EQ(beta->install, ExtensionInstall::ExternalRegistry);
    EXPECT_FALSE(beta->enabled);
    EXPECT_EQ(beta->disable_reasons, 1u);
    EXPECT_EQ(beta->permissions, std::vector<std::wstring>{L"storage"});
    EXPECT_EQ(beta->host_permissions, std::vector<std::wstring>{L"<all_urls>"});
    EXPECT_EQ(beta->optional_permissions, std::vector<std::wstring>{L"history"});
    EXPECT_EQ(kinds(list, *beta), (std::vector<ExtensionSourceKind>{ExtensionSourceKind::Settings,
                                                                    ExtensionSourceKind::ExternalRegistry}));
    EXPECT_EQ(beta->removal, ExtensionRemoval::Removable);
    for (const std::size_t s : beta->sources) {
        const auto& source = list.sources[s];
        if (source.kind == ExtensionSourceKind::Settings) {
            EXPECT_EQ(source.settings_key,
                      L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb, zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz");
            EXPECT_EQ(source.installation_mode, L"force_installed");
            EXPECT_FALSE(source.settings_subkey);
            EXPECT_EQ(source.value_name, L"ExtensionSettings");
            EXPECT_TRUE(source.value);
        } else {
            EXPECT_TRUE(source.wow64);
            EXPECT_EQ(platform::display_path(source.key),
                      std::wstring(LR"(HKLM\SOFTWARE\WOW6432Node\Google\Chrome\Extensions\)") + kBeta);
            EXPECT_EQ(source.update_url, platform::to_utf16(kStoreUrl));
        }
    }

    EXPECT_EQ(find(list, BrowserKind::Chrome, kComponent), nullptr);  // A component: Chrome's own
    EXPECT_EQ(find(list, BrowserKind::Chrome, L"jjjjjjjjjjjjjjjjjjjjjjjjjjjjjjjj"), nullptr);  // A theme

    const auto* developer = find(list, BrowserKind::Chrome, kDeveloper);
    ASSERT_NE(developer, nullptr);
    EXPECT_EQ(developer->install, ExtensionInstall::Developer);
    EXPECT_EQ(developer->folder, path(L"Dev"));
    EXPECT_EQ(developer->name, L"Developer");
    EXPECT_FALSE(developer->from_store);

    const auto* forced = find(list, BrowserKind::Chrome, kForced);
    ASSERT_NE(forced, nullptr);
    EXPECT_EQ(forced->profile_name, L"Work");
    EXPECT_EQ(forced->install, ExtensionInstall::Policy);
    ASSERT_EQ(kinds(list, *forced), std::vector<ExtensionSourceKind>{ExtensionSourceKind::ForceList});
    const auto& entry = list.sources[forced->sources[0]];
    EXPECT_EQ(entry.value_name, L"1");
    EXPECT_EQ(entry.update_url, L"https://example.com/update");
    EXPECT_EQ(entry.list.size(), 2u);  // "1" and "2"; "x" isn't a list entry
    EXPECT_TRUE(entry.user_sid.empty());

    // No Local State: Edge's profile comes from its folder; the manifest from the preferences.
    const auto* edge = find(list, BrowserKind::Edge, kEdgeOwn);
    ASSERT_NE(edge, nullptr);
    EXPECT_EQ(edge->name, L"Edge Own");
    EXPECT_EQ(edge->store, ExtensionStore::EdgeAddons);  // from_webstore, in Edge

    const auto* opera = find(list, BrowserKind::Opera, kOpera);
    ASSERT_NE(opera, nullptr);
    EXPECT_FALSE(opera->enabled);  // state 0
    EXPECT_EQ(opera->store, ExtensionStore::OperaAddons);
}

TEST_F(BrowserExtensionsTest, ReadsFirefoxAddonsAndTheirSources) {
    const auto list = list_browser_extensions(environment());
    EXPECT_EQ(find(list, BrowserKind::Firefox, L"builtin@mozilla.org"), nullptr);
    EXPECT_EQ(find(list, BrowserKind::Firefox, L"theme@example.com"), nullptr);

    const auto* store = find(list, BrowserKind::Firefox, L"store@example.com");
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->name, L"Store One");
    EXPECT_EQ(store->description, L"From AMO");
    EXPECT_EQ(store->install, ExtensionInstall::Store);
    EXPECT_EQ(store->store, ExtensionStore::Mozilla);
    EXPECT_EQ(store->install_time, 1700000000000LL);
    EXPECT_EQ(store->permissions, std::vector<std::wstring>{L"tabs"});
    EXPECT_EQ(store->host_permissions, std::vector<std::wstring>{L"<all_urls>"});
    EXPECT_EQ(store->optional_permissions, std::vector<std::wstring>{L"history"});
    EXPECT_EQ(store->removal, ExtensionRemoval::NotRemovable);

    const auto* registered = find(list, BrowserKind::Firefox, L"registry@example.com");
    ASSERT_NE(registered, nullptr);
    EXPECT_EQ(registered->install, ExtensionInstall::ExternalRegistry);
    ASSERT_EQ(kinds(list, *registered),
              std::vector<ExtensionSourceKind>{ExtensionSourceKind::FirefoxRegistry});
    EXPECT_EQ(list.sources[registered->sources[0]].path, LR"(C:\Contoso\registry.xpi)");
    EXPECT_EQ(registered->removal, ExtensionRemoval::Removable);

    // The Install policy names a URL; the add-on installed from it is found by its sourceURI.
    const auto* policy = find(list, BrowserKind::Firefox, L"policy@example.com");
    ASSERT_NE(policy, nullptr);
    EXPECT_EQ(policy->install, ExtensionInstall::Policy);
    EXPECT_FALSE(policy->enabled);
    ASSERT_EQ(kinds(list, *policy), std::vector<ExtensionSourceKind>{ExtensionSourceKind::FirefoxInstall});
    EXPECT_EQ(policy->removal, ExtensionRemoval::Removable);

    EXPECT_EQ(find(list, BrowserKind::Firefox, L"temp@example.com")->install, ExtensionInstall::Developer);

    const auto* json = find(list, BrowserKind::Firefox, L"json@example.com");
    ASSERT_NE(json, nullptr);
    EXPECT_EQ(kinds(list, *json), (std::vector<ExtensionSourceKind>{ExtensionSourceKind::PoliciesJson,
                                                                    ExtensionSourceKind::PoliciesJson}));
    EXPECT_EQ(json->removal, ExtensionRemoval::ManagedByPoliciesJson);
    EXPECT_EQ(list.sources[json->sources[0]].file, path(LR"(Firefox\distribution\policies.json)"));
}

TEST_F(BrowserExtensionsTest, SourcesWithoutAnInstalledExtensionAreEntriesOfTheirOwn) {
    const auto list = list_browser_extensions(environment());
    // In the machine's force list, but in no profile yet.
    const auto* not_yet = find(list, BrowserKind::Chrome, kNotYet);
    ASSERT_NE(not_yet, nullptr);
    EXPECT_FALSE(not_yet->in_profile);
    EXPECT_TRUE(not_yet->user_sid.empty());
    EXPECT_EQ(not_yet->install, ExtensionInstall::Policy);
    EXPECT_EQ(not_yet->removal, ExtensionRemoval::Removable);
    EXPECT_EQ(not_yet->page.arguments, std::vector<std::wstring>{L"chrome://extensions"});
    EXPECT_EQ(not_yet->page.executable, path(LR"(PF\Google\Chrome\Application\chrome.exe)"));

    // The other id of the "id1, id2" ExtensionSettings key.
    const auto* other = find(list, BrowserKind::Chrome, L"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz");
    ASSERT_NE(other, nullptr);
    EXPECT_FALSE(other->in_profile);

    // The user's Edge policy in the subkey form.
    const auto* subkey = find(list, BrowserKind::Edge, kSubkey);
    ASSERT_NE(subkey, nullptr);
    EXPECT_EQ(subkey->user_sid, kSid);
    ASSERT_EQ(subkey->sources.size(), 1u);
    const auto& source = list.sources[subkey->sources[0]];
    EXPECT_TRUE(source.settings_subkey);
    EXPECT_EQ(source.installation_mode, L"normal_installed");
    EXPECT_EQ(
        platform::display_path(source.key),
        std::wstring(L"HKU\\") + kSid + LR"(\Software\Policies\Microsoft\Edge\ExtensionSettings\)" + kSubkey);
    EXPECT_EQ(subkey->store, ExtensionStore::EdgeAddons);

    // A Firefox Install URL no add-on came from, and policies.json's.
    std::size_t unmatched = 0;
    for (const auto& entry : list.extensions) {
        if (entry.browser == BrowserKind::Firefox && !entry.in_profile && entry.extension_id.empty()) {
            ++unmatched;
            EXPECT_TRUE(entry.name == L"https://example.com/unknown.xpi" ||
                        entry.name == L"https://example.com/other.xpi")
                << platform::to_utf8(entry.name);
        }
    }
    EXPECT_EQ(unmatched, 2u);

    // Ids are stable and distinct.
    std::vector<std::wstring> ids;
    for (const auto& entry : list.extensions) {
        ids.push_back(entry.id);
    }
    const auto again = list_browser_extensions(environment());
    std::vector<std::wstring> ids_again;
    for (const auto& entry : again.extensions) {
        ids_again.push_back(entry.id);
    }
    EXPECT_EQ(ids, ids_again);
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end());
}

// A registration of an extension the user installed from the store, in HKLM's 64-bit view (which
// Chromium doesn't read), and a force-list value past the list's gap: listed, removable, flagged.
TEST_F(BrowserExtensionsTest, FlagsUserCopiesAndSourcesTheBrowserIgnores) {
    machine_.set_string(std::wstring(LR"(SOFTWARE\Google\Chrome\Extensions\)") + kAlpha, L"update_url",
                        platform::to_utf16(kStoreUrl));
    machine_.set_string(LR"(SOFTWARE\Policies\Google\Chrome\ExtensionInstallForcelist)", L"4", kDeveloper);
    const auto list = list_browser_extensions(environment());

    const auto* alpha = find(list, BrowserKind::Chrome, kAlpha);
    ASSERT_NE(alpha, nullptr);
    EXPECT_EQ(alpha->install, ExtensionInstall::Store);
    EXPECT_EQ(alpha->removal, ExtensionRemoval::Removable);
    EXPECT_TRUE(alpha->keeps_user_copy);
    ASSERT_EQ(alpha->sources.size(), 1u);
    EXPECT_TRUE(list.sources[alpha->sources[0]].ignored_by_browser);  // SOFTWARE\Google, not WOW6432Node
    EXPECT_FALSE(list.sources[alpha->sources[0]].wow64);

    // Unpacked by the user, and named past the force list's gap ("1", "2", then "4").
    const auto* developer = find(list, BrowserKind::Chrome, kDeveloper);
    ASSERT_NE(developer, nullptr);
    EXPECT_TRUE(developer->keeps_user_copy);
    ASSERT_EQ(developer->sources.size(), 1u);
    const auto& late = list.sources[developer->sources[0]];
    EXPECT_EQ(late.value_name, L"4");
    EXPECT_TRUE(late.ignored_by_browser);

    // Installed by the sources themselves: no copy of the user's, and sources the browser reads.
    const auto* beta = find(list, BrowserKind::Chrome, kBeta);
    ASSERT_NE(beta, nullptr);
    EXPECT_FALSE(beta->keeps_user_copy);
    for (const std::size_t s : beta->sources) {
        EXPECT_FALSE(list.sources[s].ignored_by_browser);  // WOW6432Node registration, a JSON policy
    }
    const auto* forced = find(list, BrowserKind::Chrome, kForced);
    ASSERT_NE(forced, nullptr);
    EXPECT_FALSE(forced->keeps_user_copy);
    EXPECT_FALSE(list.sources[forced->sources[0]].ignored_by_browser);        // "1"
    EXPECT_FALSE(find(list, BrowserKind::Chrome, kNotYet)->keeps_user_copy);  // In no profile
}

TEST_F(BrowserExtensionsTest, UnreadableFilesAreReported) {
    file(LR"(Users\fixture\AppData\Local\Google\Chrome\User Data\Default\Preferences)", "{not json");
    file(LR"(Users\fixture\AppData\Roaming\Mozilla\Firefox\Profiles\abc.default-release\extensions.json)",
         "[]");
    machine_.set_string(LR"(SOFTWARE\Policies\Microsoft\Edge)", L"ExtensionSettings", L"{broken");
    const auto list = list_browser_extensions(environment());
    std::vector<ExtensionDiagnosticCode> codes;
    for (const auto& diagnostic : list.diagnostics) {
        codes.push_back(diagnostic.code);
    }
    std::sort(codes.begin(), codes.end());
    EXPECT_EQ(codes, (std::vector<ExtensionDiagnosticCode>{ExtensionDiagnosticCode::ProfileUnreadable,
                                                           ExtensionDiagnosticCode::AddonsUnreadable,
                                                           ExtensionDiagnosticCode::PolicyUnreadable}));
    // Secure Preferences is still read.
    EXPECT_NE(find(list, BrowserKind::Chrome, kBeta), nullptr);
}

TEST(BrowserExtensionPolicyTest, OnlyExtensionPolicyValuesAreEditable) {
    const auto machine = [](std::wstring path) {
        return RegistryAddress{Hive::Machine, L"", std::move(path)};
    };
    const RegistryAddress user{Hive::User, kSid,
                               LR"(Software\Policies\Microsoft\Edge\ExtensionInstallForcelist)"};
    EXPECT_TRUE(is_extension_policy_value(
        machine(LR"(SOFTWARE\Policies\Google\Chrome\ExtensionInstallForcelist)"), L"3"));
    EXPECT_TRUE(is_extension_policy_value(
        machine(LR"(SOFTWARE\WOW6432Node\Policies\Google\Chrome\ExtensionInstallForcelist)"), L"1"));
    EXPECT_TRUE(is_extension_policy_value(user, L"12"));
    EXPECT_TRUE(is_extension_policy_value(machine(LR"(SOFTWARE\Policies\BraveSoftware\Brave)"),
                                          L"ExtensionSettings"));
    EXPECT_TRUE(
        is_extension_policy_value(machine(LR"(SOFTWARE\Policies\Mozilla\Firefox)"), L"ExtensionSettings"));
    EXPECT_TRUE(
        is_extension_policy_value(machine(LR"(SOFTWARE\Policies\Mozilla\Firefox\Extensions\Locked)"), L"2"));

    EXPECT_FALSE(is_extension_policy_value(user, L"0"));
    EXPECT_FALSE(is_extension_policy_value(user, L"01"));
    EXPECT_FALSE(is_extension_policy_value(user, L"name"));
    EXPECT_FALSE(
        is_extension_policy_value(machine(LR"(SOFTWARE\Policies\Google\Chrome)"), L"HomepageLocation"));
    EXPECT_FALSE(
        is_extension_policy_value(machine(LR"(SOFTWARE\Policies\Google\Chrome\URLBlocklist)"), L"1"));
    EXPECT_FALSE(
        is_extension_policy_value(machine(LR"(SOFTWARE\Policies\Contoso\ExtensionInstallForcelist)"), L"1"));
    EXPECT_FALSE(
        is_extension_policy_value(machine(LR"(SOFTWARE\Google\Chrome\ExtensionInstallForcelist)"), L"1"));
    EXPECT_FALSE(is_extension_policy_value(
        {Hive::User, L"S-1-5-21-1_Classes", LR"(Software\Policies\Google\Chrome\ExtensionInstallForcelist)"},
        L"1"));
}

// HardFloor's rule for them: a string written in their place, or the value deleted, as RestoreValue.
TEST(BrowserExtensionPolicyTest, HardFloorLetsOnlyEntryEditsThrough) {
    const RegistryAddress list{Hive::Machine, L"",
                               LR"(SOFTWARE\Policies\Google\Chrome\ExtensionInstallForcelist)"};
    const platform::RawValue text{L"1", REG_SZ, std::vector<std::byte>(4)};
    const platform::RawValue binary{L"1", REG_BINARY, std::vector<std::byte>(4)};
    const auto check = [&](leftovers::ValueEditLocation edit) {
        return leftovers::HardFloor::live().check(edit);
    };
    using leftovers::ProtectionReason;
    using leftovers::ValueEditKind;
    EXPECT_EQ(check({list, L"1", ValueEditKind::RestoreValue, text, text, {}, 0}), ProtectionReason::None);
    EXPECT_EQ(check({list, L"1", ValueEditKind::RestoreValue, text, std::nullopt, {}, 0}),
              ProtectionReason::None);
    EXPECT_EQ(check({list, L"1", ValueEditKind::RestoreValue, text, binary, {}, 0}),
              ProtectionReason::NotEditable);
    EXPECT_EQ(check({list, L"1", ValueEditKind::DeleteValue, text, std::nullopt, {}, 0}),
              ProtectionReason::NotEditable);
    EXPECT_EQ(check({list, L"Name", ValueEditKind::RestoreValue, text, std::nullopt, {}, 0}),
              ProtectionReason::NotEditable);
    // The whole policy key is never removed; an id's own subkey or registration may be.
    EXPECT_NE(leftovers::HardFloor::live().check(
                  leftovers::RegistryKeyLocation{{Hive::Machine, L"", LR"(SOFTWARE\Policies)"}}),
              ProtectionReason::None);
    EXPECT_EQ(leftovers::HardFloor::live().check(leftovers::RegistryKeyLocation{
                  {Hive::Machine, L"", LR"(SOFTWARE\Policies\Google\Chrome\ExtensionSettings\abc)"}}),
              ProtectionReason::None);
}

TEST(BrowserExtensionPolicyTest, StoresAreToldByTheirUpdateUrls) {
    EXPECT_EQ(store_of_update_url(L"https://clients2.google.com/service/update2/crx"),
              ExtensionStore::ChromeWebStore);
    EXPECT_EQ(store_of_update_url(L"HTTPS://clients2.google.com/service/update2/crx/?x=1"),
              ExtensionStore::ChromeWebStore);
    EXPECT_EQ(store_of_update_url(L"https://edge.microsoft.com/extensionwebstorebase/v1/crx"),
              ExtensionStore::EdgeAddons);
    EXPECT_EQ(store_of_update_url(L"https://extension-updates.opera.com/api/omaha/update/"),
              ExtensionStore::OperaAddons);
    EXPECT_EQ(store_of_update_url(L"https://example.com/updates.xml"), ExtensionStore::None);
    EXPECT_EQ(store_of_update_url(L""), ExtensionStore::None);
}

}  // namespace
}  // namespace avel::tools
