// The Windows apps tool's list (tools/windows_apps) and data list (tools/windows_apps_data): the
// model built from injected package listings (categories, protection, frameworks dropped, families
// grouped, who has each, soft_undoable), and one read-only listing of the host's real packages.
// Nothing here changes a package.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/platform/packages.h"
#include "core/platform/text.h"
#include "core/tools/windows_apps.h"
#include "core/tools/windows_apps_data.h"

namespace avel::tools {
namespace {

using platform::PackageInfo;
using platform::PackageListing;
using platform::PackageSignatureKind;

constexpr wchar_t kMe[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kOther[] = L"S-1-5-21-1-2-3-1002";
constexpr wchar_t kThird[] = L"S-1-5-21-1-2-3-1003";

// A package as platform::list_packages describes it. `full_name` is Name_Version_Arch__PublisherId.
PackageInfo package(std::wstring name, std::wstring version, std::vector<std::wstring> users,
                    PackageSignatureKind signature = PackageSignatureKind::Store,
                    std::wstring publisher_id = L"8wekyb3d8bbwe") {
    PackageInfo info;
    info.name = name;
    info.version = version;
    info.family_name = name + L'_' + publisher_id;
    info.full_name = name + L'_' + version + L"_x64__" + publisher_id;
    info.display_name = name + L" app";
    info.publisher = L"Microsoft Corporation";
    info.install_location = LR"(C:\Program Files\WindowsApps\)" + info.full_name;
    info.signature_kind = signature;
    info.user_sids = std::move(users);
    return info;
}

WindowsAppsEnvironment environment(PackageListing listing) {
    WindowsAppsEnvironment environment;
    environment.listing = std::move(listing);
    environment.current_user_sid = kMe;
    environment.account_name = [](const std::wstring& sid) -> std::wstring {
        static const std::map<std::wstring, std::wstring> names{{kMe, L"PC\\me"}, {kOther, L"PC\\anna"}};
        const auto found = names.find(sid);
        return found != names.end() ? found->second : std::wstring();
    };
    return environment;
}

PackageListing complete(std::vector<PackageInfo> packages, std::vector<std::wstring> provisioned = {}) {
    PackageListing listing;
    listing.packages = std::move(packages);
    listing.provisioned = std::move(provisioned);
    listing.all_users = true;
    return listing;
}

const WindowsApp* find(const WindowsAppsList& list, std::wstring_view family) {
    const auto found = std::find_if(list.apps.begin(), list.apps.end(), [&](const WindowsApp& app) {
        return platform::iequals(app.family_name, family);
    });
    return found != list.apps.end() ? &*found : nullptr;
}

TEST(WindowsAppsDataTest, MatchesIdentityNamesIgnoringCase) {
    EXPECT_EQ(app_category(L"Microsoft.WindowsStore_8wekyb3d8bbwe"), AppCategory::Critical);
    EXPECT_EQ(app_category(L"microsoft.windowsstore"), AppCategory::Critical);
    EXPECT_EQ(app_category(L"MICROSOFT.DESKTOPAPPINSTALLER_8wekyb3d8bbwe"), AppCategory::Critical);
    EXPECT_EQ(app_category(L"Microsoft.BingNews_8wekyb3d8bbwe"), AppCategory::OftenRemoved);
    EXPECT_EQ(app_category(L"Clipchamp.Clipchamp_yxz26nhyzhsrt"), AppCategory::OftenRemoved);
    EXPECT_EQ(app_category(L"Microsoft.Windows.Photos_8wekyb3d8bbwe"), AppCategory::SemiCritical);
    EXPECT_EQ(app_warning(L"Microsoft.Windows.Photos_8wekyb3d8bbwe"), AppWarning::OpensFilesByDefault);
    EXPECT_EQ(app_warning(L"MicrosoftWindows.Client.WebExperience_cw5n1h2txyewy"), AppWarning::Widgets);
    // Only the whole identity name matches, not a longer or shorter one.
    EXPECT_EQ(app_category(L"Microsoft.BingNewsExtra_8wekyb3d8bbwe"), AppCategory::Other);
    EXPECT_EQ(app_category(L"Microsoft.Bing_8wekyb3d8bbwe"), AppCategory::Other);
    EXPECT_EQ(app_category(L"Contoso.Notes_abcdefghijklm"), AppCategory::Other);
    EXPECT_EQ(app_category(L""), AppCategory::Other);
    // No warning outside the semi-critical ones.
    EXPECT_EQ(app_warning(L"Microsoft.BingNews_8wekyb3d8bbwe"), AppWarning::None);
    EXPECT_EQ(app_warning(L"Microsoft.WindowsStore_8wekyb3d8bbwe"), AppWarning::None);
}

TEST(WindowsAppsDataTest, MediaPlayerAndTheXboxAppAreSemiCritical) {
    EXPECT_EQ(app_category(L"Microsoft.ZuneMusic_8wekyb3d8bbwe"), AppCategory::SemiCritical);
    EXPECT_EQ(app_warning(L"Microsoft.ZuneMusic_8wekyb3d8bbwe"), AppWarning::DefaultMediaPlayer);
    EXPECT_EQ(app_category(L"Microsoft.GamingApp_8wekyb3d8bbwe"), AppCategory::SemiCritical);
    EXPECT_EQ(app_warning(L"Microsoft.GamingApp_8wekyb3d8bbwe"), AppWarning::GamePass);
    EXPECT_EQ(to_string(AppWarning::DefaultMediaPlayer), "default-media-player");
    EXPECT_EQ(to_string(AppWarning::GamePass), "game-pass");
}

TEST(WindowsAppsDataTest, ThirdPartyPromotionsAreOftenRemoved) {
    for (const wchar_t* family :
         {L"king.com.CandyCrushSaga_kgqvnymyfvs32", L"king.com.BubbleWitch3Saga_kgqvnymyfvs32",
          L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0", L"Disney.37853FC22B2CE_6rarf9sa4v8jt",
          L"BytedancePte.Ltd.TikTok_6yccndn6064se", L"Facebook.InstagramBeta_8xx8rvfyw5nnt",
          L"FACEBOOK.317180B0BB486_8xx8rvfyw5nnt", L"4DF9E0F8.Netflix_mcm4njqhnhss8",
          L"A278AB0D.MarchofEmpires_h6adky7gbf63m"}) {
        SCOPED_TRACE(platform::to_utf8(family));
        EXPECT_EQ(app_category(family), AppCategory::OftenRemoved);
        EXPECT_EQ(app_warning(family), AppWarning::None);
    }
    // A prefix entry needs its dot: another publisher whose name merely starts the same isn't matched.
    EXPECT_EQ(app_category(L"king.comics.Reader_abcdefghijklm"), AppCategory::Other);
    EXPECT_EQ(app_category(L"FacebookResearch.Tool_abcdefghijklm"), AppCategory::Other);
}

TEST(WindowsAppsDataTest, MatchesPrefixEntries) {
    EXPECT_EQ(app_category(L"MicrosoftCorporationII.WinAppRuntime.Main.1.8_8wekyb3d8bbwe"),
              AppCategory::Critical);
    EXPECT_EQ(app_category(L"MicrosoftCorporationII.WinAppRuntime.Singleton_8wekyb3d8bbwe"),
              AppCategory::Critical);
    EXPECT_EQ(app_category(L"Microsoft.LanguageExperiencePackru-RU_8wekyb3d8bbwe"),
              AppCategory::SemiCritical);
    EXPECT_EQ(app_warning(L"Microsoft.LanguageExperiencePackhy-AM_8wekyb3d8bbwe"),
              AppWarning::DisplayLanguage);
}

TEST(WindowsAppsDataTest, TableIsConsistent) {
    std::set<std::wstring> names;
    for (const AppDataEntry& entry : app_data_entries()) {
        SCOPED_TRACE(platform::to_utf8(entry.name));
        EXPECT_FALSE(entry.name.empty());
        EXPECT_EQ(entry.name.find(L'_'), std::wstring_view::npos) << "an identity name, not a family name";
        EXPECT_TRUE(names.insert(platform::to_lower(entry.name)).second) << "listed twice";
        // Every semi-critical entry says what removing it breaks; no other entry has a warning.
        EXPECT_EQ(entry.category == AppCategory::SemiCritical, entry.warning != AppWarning::None);
        EXPECT_NE(entry.category, AppCategory::Other) << "Other is the default, not a table entry";
        // An entry finds itself (the first matching entry wins, so none is shadowed).
        if (!entry.name.ends_with(L'*')) {
            EXPECT_EQ(app_category(entry.name), entry.category);
        }
    }
}

TEST(WindowsAppsTest, CategoriesAndProtection) {
    const auto list = list_windows_apps(environment(complete({
        package(L"Microsoft.BingNews", L"4.55.0.0", {kMe}),
        package(L"Microsoft.Windows.Photos", L"2025.1.0.0", {kMe}),
        package(L"Microsoft.WindowsStore", L"22410.1.0.0", {kMe}),
        package(L"Microsoft.Windows.ShellExperienceHost", L"10.0.26100.1", {kMe},
                PackageSignatureKind::System, L"cw5n1h2txyewy"),
        package(L"Contoso.Notes", L"1.0.0.0", {kMe}, PackageSignatureKind::Developer, L"abcdefghijklm"),
    })));
    ASSERT_EQ(list.apps.size(), 5u);
    EXPECT_TRUE(list.all_users);

    const WindowsApp* news = find(list, L"Microsoft.BingNews_8wekyb3d8bbwe");
    ASSERT_NE(news, nullptr);
    EXPECT_EQ(news->category, AppCategory::OftenRemoved);
    EXPECT_EQ(news->protection, AppProtection::None);
    EXPECT_TRUE(news->can_remove_for_everyone);

    const WindowsApp* photos = find(list, L"Microsoft.Windows.Photos_8wekyb3d8bbwe");
    ASSERT_NE(photos, nullptr);
    EXPECT_EQ(photos->category, AppCategory::SemiCritical);
    EXPECT_EQ(photos->warning, AppWarning::OpensFilesByDefault);
    EXPECT_EQ(photos->protection, AppProtection::None);

    // The Store isn't System-signed; the critical list protects it.
    const WindowsApp* store = find(list, L"Microsoft.WindowsStore_8wekyb3d8bbwe");
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->category, AppCategory::Critical);
    EXPECT_EQ(store->protection, AppProtection::Critical);
    EXPECT_FALSE(store->can_remove_for_me);
    EXPECT_FALSE(store->can_deprovision);
    EXPECT_FALSE(store->can_remove_for_everyone);

    // Signed as part of Windows: protected whatever the list says.
    const WindowsApp* shell = find(list, L"Microsoft.Windows.ShellExperienceHost_cw5n1h2txyewy");
    ASSERT_NE(shell, nullptr);
    EXPECT_EQ(shell->protection, AppProtection::System);
    EXPECT_FALSE(shell->can_remove_for_everyone);

    const WindowsApp* notes = find(list, L"Contoso.Notes_abcdefghijklm");
    ASSERT_NE(notes, nullptr);
    EXPECT_EQ(notes->category, AppCategory::Other);
    EXPECT_EQ(notes->protection, AppProtection::None);
    EXPECT_EQ(notes->signature_kind, PackageSignatureKind::Developer);

    // Sorted by display name.
    EXPECT_TRUE(
        std::is_sorted(list.apps.begin(), list.apps.end(), [](const WindowsApp& a, const WindowsApp& b) {
            return platform::to_lower(a.display_name) < platform::to_lower(b.display_name);
        }));
}

// Packages with an external location give a desktop program its identity (PowerToys' context
// menus, Edge's or OneDrive's integration): protected, with the program's folder kept for the
// details. The data list and the signature come first.
TEST(WindowsAppsTest, PackagesOfDesktopProgramsAreProtected) {
    PackageInfo sparse = package(L"Microsoft.PowerToys.SparseApp", L"0.102.3101.0", {kMe, kOther},
                                 PackageSignatureKind::Developer);
    sparse.external_location = LR"(C:\Users\me\AppData\Local\PowerToys\WinUI3Apps\)";
    PackageInfo edge =
        package(L"Microsoft.MicrosoftEdge.Stable", L"152.0.4191.53", {kMe}, PackageSignatureKind::Developer);
    edge.external_location = LR"(C:\Program Files (x86)\Microsoft\Edge\Application)";
    // Only the older version has the external location: still the desktop program's.
    PackageInfo older =
        package(L"Contoso.Tool", L"1.0.0.0", {kOther}, PackageSignatureKind::Developer, L"abcdefghijklm");
    older.external_location = LR"(C:\Program Files\Contoso Tool)";
    PackageInfo newer =
        package(L"Contoso.Tool", L"2.0.0.0", {kMe}, PackageSignatureKind::Developer, L"abcdefghijklm");
    // Critical and with an external location: critical wins.
    PackageInfo store = package(L"Microsoft.WindowsStore", L"22410.1.0.0", {kMe});
    store.external_location = LR"(C:\Program Files\Store)";
    const auto list =
        list_windows_apps(environment(complete({sparse, edge, older, newer, store}, {sparse.full_name})));
    ASSERT_EQ(list.apps.size(), 4u);

    for (const wchar_t* name :
         {L"Microsoft.PowerToys.SparseApp_8wekyb3d8bbwe", L"Microsoft.MicrosoftEdge.Stable_8wekyb3d8bbwe",
          L"Contoso.Tool_abcdefghijklm"}) {
        SCOPED_TRACE(platform::to_utf8(name));
        const WindowsApp* app = find(list, name);
        ASSERT_NE(app, nullptr);
        EXPECT_EQ(app->protection, AppProtection::ExternalLocation);
        EXPECT_FALSE(app->can_remove_for_me);
        EXPECT_FALSE(app->can_deprovision);
        EXPECT_FALSE(app->can_remove_for_everyone);
    }
    EXPECT_EQ(find(list, L"Microsoft.PowerToys.SparseApp_8wekyb3d8bbwe")->external_location,
              LR"(C:\Users\me\AppData\Local\PowerToys\WinUI3Apps\)");
    EXPECT_TRUE(find(list, L"Microsoft.PowerToys.SparseApp_8wekyb3d8bbwe")->provisioned);
    // Edge's package keeps its category and warning; only its protection changes.
    const WindowsApp* edge_app = find(list, L"Microsoft.MicrosoftEdge.Stable_8wekyb3d8bbwe");
    EXPECT_EQ(edge_app->category, AppCategory::SemiCritical);
    EXPECT_EQ(edge_app->warning, AppWarning::BrowserIntegration);
    EXPECT_EQ(find(list, L"Contoso.Tool_abcdefghijklm")->external_location,
              LR"(C:\Program Files\Contoso Tool)");
    EXPECT_EQ(find(list, L"Microsoft.WindowsStore_8wekyb3d8bbwe")->protection, AppProtection::Critical);

    EXPECT_EQ(to_string(AppProtection::ExternalLocation), "external-location");
    EXPECT_EQ(to_string(AppProtection::System), "system");
    EXPECT_EQ(to_string(AppProtection::Critical), "critical");
}

TEST(WindowsAppsTest, FrameworksResourcesAndBundlesAreNotListed) {
    PackageInfo framework = package(L"Microsoft.VCLibs.140.00", L"14.0.33519.0", {kMe, kOther});
    framework.framework = true;
    PackageInfo resource = package(L"Microsoft.Contoso.Resources", L"1.0.0.0", {kMe});
    resource.resource = true;
    PackageInfo bundle = package(L"Contoso.Bundle", L"1.0.0.0", {kMe});
    bundle.bundle = true;
    PackageInfo optional = package(L"Contoso.Extension", L"1.0.0.0", {kMe});
    optional.optional = true;
    const auto list = list_windows_apps(environment(complete({framework, resource, bundle, optional})));
    ASSERT_EQ(list.apps.size(), 1u);
    EXPECT_EQ(list.apps[0].family_name, L"Contoso.Extension_8wekyb3d8bbwe");  // Optional packages are apps
}

TEST(WindowsAppsTest, GroupsVersionsByFamily) {
    // Another user still has an older version; the current user has the newer one; a third version is
    // staged for nobody.
    PackageInfo older = package(L"Contoso.Notes", L"1.9.0.0", {kOther}, PackageSignatureKind::Store);
    older.display_name = L"Notes (old)";
    PackageInfo mine = package(L"Contoso.Notes", L"1.10.0.0", {kMe, kThird}, PackageSignatureKind::Store);
    mine.display_name = L"Notes";
    PackageInfo newest = package(L"Contoso.Notes", L"2.0.0.0", {}, PackageSignatureKind::Store);
    newest.display_name = L"Notes 2";
    const auto list = list_windows_apps(environment(complete({older, newest, mine})));
    ASSERT_EQ(list.apps.size(), 1u);
    const WindowsApp& app = list.apps[0];
    EXPECT_EQ(app.id, windows_app_id(L"Contoso.Notes_8wekyb3d8bbwe"));
    EXPECT_EQ(app.name, L"Contoso.Notes");
    // Described by the current user's package.
    EXPECT_EQ(app.display_name, L"Notes");
    EXPECT_EQ(app.version, L"1.10.0.0");
    EXPECT_EQ(app.current_user_full_name, mine.full_name);
    // Every version, newest first (numerically: 1.10 is newer than 1.9).
    ASSERT_EQ(app.packages.size(), 3u);
    EXPECT_EQ(app.packages[0].full_name, newest.full_name);
    EXPECT_EQ(app.packages[1].full_name, mine.full_name);
    EXPECT_EQ(app.packages[2].full_name, older.full_name);
    EXPECT_EQ(app.packages[2].user_sids, std::vector<std::wstring>{kOther});
    // Every user, the current one first, then by account name; an unresolved SID stands for itself.
    ASSERT_EQ(app.users.size(), 3u);
    EXPECT_EQ(app.users[0].sid, kMe);
    EXPECT_TRUE(app.users[0].current);
    EXPECT_EQ(app.users[0].account_name, L"PC\\me");
    EXPECT_EQ(app.users[1].account_name, L"PC\\anna");
    EXPECT_FALSE(app.users[1].current);
    EXPECT_EQ(app.users[2].sid, kThird);
    EXPECT_EQ(app.users[2].account_name, kThird);
}

TEST(WindowsAppsTest, DescribedByTheNewestWhenTheCurrentUserHasNone) {
    const auto list = list_windows_apps(environment(complete({
        package(L"Contoso.Notes", L"1.0.0.0", {kOther}),
        package(L"Contoso.Notes", L"3.0.0.0", {kThird}),
    })));
    ASSERT_EQ(list.apps.size(), 1u);
    EXPECT_EQ(list.apps[0].version, L"3.0.0.0");
    EXPECT_TRUE(list.apps[0].current_user_full_name.empty());
    EXPECT_FALSE(list.apps[0].can_remove_for_me);
    EXPECT_TRUE(list.apps[0].can_remove_for_everyone);
}

TEST(WindowsAppsTest, SoftUndoableWhenAnotherUserHasItOrItIsProvisioned) {
    const PackageInfo shared = package(L"Contoso.Shared", L"1.0.0.0", {kMe, kOther});
    const PackageInfo provisioned = package(L"Contoso.Provisioned", L"1.0.0.0", {kMe});
    const PackageInfo alone = package(L"Contoso.Alone", L"1.0.0.0", {kMe});
    const PackageInfo staged = package(L"Contoso.Staged", L"1.0.0.0", {});
    // The provisioned list holds full names; a different version of the family still counts.
    const auto list = list_windows_apps(environment(
        complete({shared, provisioned, alone, staged}, {L"Contoso.Provisioned_0.9.0.0_neutral__8wekyb3d8bbwe",
                                                        L"Contoso.Staged_1.0.0.0_x64__8wekyb3d8bbwe"})));
    ASSERT_EQ(list.apps.size(), 4u);

    const WindowsApp* app = find(list, shared.family_name);
    ASSERT_NE(app, nullptr);
    EXPECT_TRUE(app->soft_undoable);
    EXPECT_TRUE(app->can_remove_for_me);
    EXPECT_FALSE(app->provisioned);
    EXPECT_FALSE(app->can_deprovision);

    app = find(list, provisioned.family_name);
    ASSERT_NE(app, nullptr);
    EXPECT_TRUE(app->provisioned);
    EXPECT_EQ(app->provisioned_full_name, L"Contoso.Provisioned_0.9.0.0_neutral__8wekyb3d8bbwe");
    EXPECT_TRUE(app->soft_undoable);
    EXPECT_TRUE(app->can_remove_for_me);
    EXPECT_TRUE(app->can_deprovision);

    // Removing the last user's registration of a package that isn't provisioned deletes it.
    app = find(list, alone.family_name);
    ASSERT_NE(app, nullptr);
    EXPECT_FALSE(app->soft_undoable);
    EXPECT_FALSE(app->can_remove_for_me);
    EXPECT_TRUE(app->can_remove_for_everyone);

    // Provisioned and staged for nobody: nothing to remove for the current user.
    app = find(list, staged.family_name);
    ASSERT_NE(app, nullptr);
    EXPECT_TRUE(app->users.empty());
    EXPECT_TRUE(app->soft_undoable);
    EXPECT_FALSE(app->can_remove_for_me);
    EXPECT_TRUE(app->can_deprovision);
}

TEST(WindowsAppsTest, NothingIsOfferedFromAnIncompleteListing) {
    // Without administrator rights: the current user's packages only, nothing known to be provisioned.
    // No mode is offered, as the planner refuses them all (incomplete-listing); all_users tells why.
    PackageListing listing;
    listing.packages = {package(L"Contoso.Notes", L"1.0.0.0", {kMe}),
                        package(L"Microsoft.BingNews", L"4.55.0.0", {kMe})};
    listing.all_users = false;
    const auto list = list_windows_apps(environment(listing));
    EXPECT_FALSE(list.all_users);
    ASSERT_EQ(list.apps.size(), 2u);
    for (const WindowsApp& app : list.apps) {
        SCOPED_TRACE(platform::to_utf8(app.family_name));
        EXPECT_EQ(app.protection, AppProtection::None);
        EXPECT_FALSE(app.soft_undoable);
        EXPECT_FALSE(app.can_remove_for_me);
        EXPECT_FALSE(app.can_deprovision);
        EXPECT_FALSE(app.can_remove_for_everyone);
    }

    // The same packages listed completely offer removal for everyone.
    listing.all_users = true;
    for (const WindowsApp& app : list_windows_apps(environment(listing)).apps) {
        EXPECT_TRUE(app.can_remove_for_everyone);
    }
}

TEST(WindowsAppsTest, KeepsTheListingsError) {
    PackageListing listing = complete({package(L"Contoso.Notes", L"1.0.0.0", {kMe})});
    listing.error = static_cast<std::int32_t>(0x80070005);
    const auto list = list_windows_apps(environment(listing));
    EXPECT_EQ(list.error, static_cast<std::int32_t>(0x80070005));
    EXPECT_EQ(list.apps.size(), 1u);
}

TEST(WindowsAppsTest, IdsAreStableAndIgnoreCase) {
    const std::wstring id = windows_app_id(L"Microsoft.BingNews_8wekyb3d8bbwe");
    EXPECT_EQ(id.size(), 10u);
    EXPECT_EQ(id.find_first_not_of(L"0123456789abcdef"), std::wstring::npos);
    EXPECT_EQ(windows_app_id(L"microsoft.bingnews_8WEKYB3D8BBWE"), id);
    EXPECT_NE(windows_app_id(L"Microsoft.BingWeather_8wekyb3d8bbwe"), id);
}

// Read-only: lists the host's real packages (every user's when elevated) and prints how they are
// categorized. Changes nothing.
TEST(WindowsAppsTest, ListsTheHostsPackagesReadOnly) {
    const auto list = list_windows_apps(live_windows_apps_environment());
    EXPECT_EQ(list.error, 0);
    std::map<std::string, int> counts;
    for (const WindowsApp& app : list.apps) {
        const std::string category(to_string(app.category));
        const std::string protection(to_string(app.protection));
        ++counts[category + "/" + protection];
        EXPECT_FALSE(app.id.empty());
        EXPECT_FALSE(app.family_name.empty());
        // An app with an external location is always protected (System or Critical may come first).
        if (!app.external_location.empty()) {
            EXPECT_NE(app.protection, AppProtection::None);
        }
        if (app.protection != AppProtection::None || !list.all_users) {
            EXPECT_FALSE(app.can_remove_for_me || app.can_deprovision || app.can_remove_for_everyone);
        }
        if (app.category != AppCategory::Other || app.protection != AppProtection::None) {
            std::printf(
                "  %-13s %-8s %-15s %s%s%s%s\n", category.c_str(), protection.c_str(),
                app.soft_undoable ? "soft-undoable" : "", platform::to_utf8(app.family_name).c_str(),
                app.provisioned ? " (provisioned)" : "",
                app.warning != AppWarning::None ? (" [" + std::string(to_string(app.warning)) + "]").c_str()
                                                : "",
                app.external_location.empty() ? ""
                                              : (" in " + platform::to_utf8(app.external_location)).c_str());
        }
    }
    std::printf("%zu apps, all users: %s\n", list.apps.size(), list.all_users ? "yes" : "no");
    for (const auto& [key, count] : counts) {
        std::printf("  %s: %d\n", key.c_str(), count);
    }
}

}  // namespace
}  // namespace avel::tools
