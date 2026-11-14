// The Windows apps tool's planner (tools/windows_apps_changes): the plans for removing apps for the
// current user, deprovisioning them and removing them for everyone, and every issue that keeps an app
// out of a plan, on lists built from injected package listings. Nothing here changes a package.
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "core/leftovers/leftover.h"
#include "core/platform/packages.h"
#include "core/tools/windows_apps.h"
#include "core/tools/windows_apps_changes.h"

namespace avel::tools {
namespace {

using leftovers::Category;
using leftovers::PackageLocation;
using leftovers::PackageProvisioningLocation;
using leftovers::PackageUserLocation;
using platform::PackageInfo;
using platform::PackageListing;
using platform::PackageSignatureKind;
using Code = WindowsAppsPlanIssueCode;

constexpr wchar_t kMe[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kOther[] = L"S-1-5-21-1-2-3-1002";

PackageInfo package(std::wstring name, std::wstring version, std::vector<std::wstring> users,
                    PackageSignatureKind signature = PackageSignatureKind::Store) {
    PackageInfo info;
    info.name = name;
    info.version = version;
    info.family_name = name + L"_8wekyb3d8bbwe";
    info.full_name = name + L'_' + version + L"_x64__8wekyb3d8bbwe";
    info.display_name = name.substr(name.find(L'.') + 1);
    info.signature_kind = signature;
    info.user_sids = std::move(users);
    return info;
}

std::wstring family(std::wstring_view name) { return std::wstring(name) + L"_8wekyb3d8bbwe"; }
std::wstring full(std::wstring_view name, std::wstring_view version) {
    return std::wstring(name) + L'_' + std::wstring(version) + L"_x64__8wekyb3d8bbwe";
}

// One app of each situation the planners tell apart.
WindowsAppsList sample_list(bool all_users = true) {
    PackageListing listing;
    listing.all_users = all_users;
    listing.packages = {
        package(L"Contoso.Shared", L"1.0.0.0", {kMe, kOther}),  // Another user has it
        package(L"Contoso.Provisioned", L"2.0.0.0", {kMe}),     // Provisioned
        package(L"Contoso.Alone", L"1.0.0.0", {kMe}),           // Only the current user, not provisioned
        package(L"Contoso.Theirs", L"1.0.0.0", {kOther}),       // Not the current user's
        package(L"Contoso.Versions", L"1.0.0.0", {kOther}),     // Two versions, provisioned
        package(L"Contoso.Versions", L"1.1.0.0", {kMe}),
        package(L"Microsoft.WindowsStore", L"22410.1.0.0", {kMe}),  // Critical
        package(L"Microsoft.Windows.ShellExperienceHost", L"10.0.0.0", {kMe, kOther},
                PackageSignatureKind::System),
    };
    if (all_users) {
        listing.provisioned = {full(L"Contoso.Provisioned", L"2.0.0.0"),
                               full(L"Contoso.Versions", L"1.1.0.0")};
    }
    WindowsAppsEnvironment environment;
    environment.listing = std::move(listing);
    environment.current_user_sid = kMe;
    return list_windows_apps(environment);
}

const WindowsApp& app_of(const WindowsAppsList& list, std::wstring_view name) {
    for (const auto& app : list.apps) {
        if (app.name == name) {
            return app;
        }
    }
    throw std::runtime_error("no such app");
}

std::vector<Code> codes(const WindowsAppsPlan& plan) {
    std::vector<Code> result;
    for (const auto& issue : plan.issues) {
        result.push_back(issue.code);
    }
    return result;
}

TEST(WindowsAppsChangesTest, RemoveForMePlansTheCurrentUsersRegistration) {
    const auto list = sample_list();
    const WindowsApp& shared = app_of(list, L"Contoso.Shared");
    const WindowsApp& provisioned = app_of(list, L"Contoso.Provisioned");
    const std::vector<std::wstring> ids{shared.id, family(L"contoso.provisioned")};
    const auto result = plan_remove_for_me(list, ids);
    EXPECT_TRUE(result.issues.empty());
    EXPECT_EQ(result.planned, (std::vector<std::wstring>{shared.id, provisioned.id}));
    ASSERT_EQ(result.plan.items.size(), 2u);

    const auto& item = result.plan.items[0];
    const auto* location = std::get_if<PackageUserLocation>(&item.location);
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->family_name, family(L"Contoso.Shared"));
    EXPECT_EQ(location->full_name, full(L"Contoso.Shared", L"1.0.0.0"));
    EXPECT_EQ(location->user_sid, kMe);
    EXPECT_TRUE(location->before);
    EXPECT_FALSE(location->after);
    EXPECT_EQ(item.category, Category::Packages);
    EXPECT_EQ(item.programs, std::vector<std::wstring>{L"Shared"});
    EXPECT_EQ(item.confidence, leftovers::Confidence::Unknown);
    EXPECT_TRUE(result.plan.description.empty());  // The caller's to set

    const auto* second = std::get_if<PackageUserLocation>(&result.plan.items[1].location);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->family_name, family(L"Contoso.Provisioned"));
}

TEST(WindowsAppsChangesTest, RemoveForMeActsOnTheCurrentUsersVersion) {
    const auto list = sample_list();
    const auto result = plan_remove_for_me(list, std::vector<std::wstring>{family(L"Contoso.Versions")});
    ASSERT_EQ(result.plan.items.size(), 1u);
    const auto* location = std::get_if<PackageUserLocation>(&result.plan.items[0].location);
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->full_name, full(L"Contoso.Versions", L"1.1.0.0"));
}

TEST(WindowsAppsChangesTest, RemoveForMeIssues) {
    const auto list = sample_list();
    const std::vector<std::wstring> ids{
        L"0123456789",                                     // No such app
        app_of(list, L"Microsoft.WindowsStore").id,        // Critical
        family(L"Microsoft.Windows.ShellExperienceHost"),  // System
        app_of(list, L"Contoso.Theirs").id,                // Not the current user's
        app_of(list, L"Contoso.Alone").id,                 // Would delete the package
    };
    const auto result = plan_remove_for_me(list, ids);
    EXPECT_TRUE(result.plan.items.empty());
    EXPECT_TRUE(result.planned.empty());
    EXPECT_EQ(codes(result), (std::vector<Code>{Code::UnknownId, Code::Protected, Code::Protected,
                                                Code::NotInstalledForCurrentUser, Code::WouldDeletePackage}));
    ASSERT_EQ(result.issues.size(), ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(result.issues[i].id, ids[i]);  // As asked
    }
}

TEST(WindowsAppsChangesTest, NothingIsPlannedFromAnIncompleteListing) {
    const auto list = sample_list(false);
    const std::vector<std::wstring> ids{app_of(list, L"Contoso.Shared").id,
                                        app_of(list, L"Microsoft.WindowsStore").id};
    for (const auto& result : {plan_remove_for_me(list, ids), plan_deprovision(list, ids),
                               plan_remove_for_everyone(list, ids, true)}) {
        EXPECT_TRUE(result.plan.items.empty());
        // Protection is told first: it doesn't depend on the listing.
        EXPECT_EQ(codes(result), (std::vector<Code>{Code::IncompleteListing, Code::Protected}));
    }
}

TEST(WindowsAppsChangesTest, EachAppIsPlannedOnce) {
    const auto list = sample_list();
    const WindowsApp& shared = app_of(list, L"Contoso.Shared");
    const auto result =
        plan_remove_for_me(list, std::vector<std::wstring>{shared.id, family(L"CONTOSO.SHARED"), shared.id});
    EXPECT_TRUE(result.issues.empty());
    EXPECT_EQ(result.planned, std::vector<std::wstring>{shared.id});
    EXPECT_EQ(result.plan.items.size(), 1u);
}

TEST(WindowsAppsChangesTest, DeprovisionPlansTheProvisioning) {
    const auto list = sample_list();
    const WindowsApp& versions = app_of(list, L"Contoso.Versions");
    const std::vector<std::wstring> ids{versions.id, app_of(list, L"Contoso.Shared").id,
                                        app_of(list, L"Microsoft.WindowsStore").id};
    const auto result = plan_deprovision(list, ids);
    EXPECT_EQ(result.planned, std::vector<std::wstring>{versions.id});
    EXPECT_EQ(codes(result), (std::vector<Code>{Code::NotProvisioned, Code::Protected}));
    ASSERT_EQ(result.plan.items.size(), 1u);
    const auto* location = std::get_if<PackageProvisioningLocation>(&result.plan.items[0].location);
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->family_name, family(L"Contoso.Versions"));
    EXPECT_EQ(location->provisioned_full_name, full(L"Contoso.Versions", L"1.1.0.0"));
    EXPECT_TRUE(location->before);
    EXPECT_FALSE(location->after);
    EXPECT_EQ(result.plan.items[0].category, Category::Packages);
}

TEST(WindowsAppsChangesTest, DeprovisioningAFamilyNoUserHasIsRefused) {
    PackageListing listing;
    listing.all_users = true;
    listing.packages = {package(L"Contoso.Nobody", L"1.0.0.0", {})};
    listing.provisioned = {full(L"Contoso.Nobody", L"1.0.0.0")};
    WindowsAppsEnvironment environment;
    environment.listing = std::move(listing);
    environment.current_user_sid = kMe;
    const auto list = list_windows_apps(environment);
    const auto result = plan_deprovision(list, std::vector<std::wstring>{app_of(list, L"Contoso.Nobody").id});
    EXPECT_TRUE(result.plan.items.empty());
    EXPECT_EQ(codes(result), std::vector<Code>{Code::WouldDeletePackage});
}

TEST(WindowsAppsChangesTest, RemoveForEveryoneNeedsTheIrreversibleConfirmation) {
    const auto list = sample_list();
    const std::vector<std::wstring> ids{app_of(list, L"Contoso.Alone").id,
                                        app_of(list, L"Microsoft.WindowsStore").id};
    const auto result = plan_remove_for_everyone(list, ids, false);
    EXPECT_TRUE(result.plan.items.empty());
    EXPECT_TRUE(result.planned.empty());
    EXPECT_EQ(codes(result), (std::vector<Code>{Code::IrreversibleNeedsConfirmation, Code::Protected}));
}

TEST(WindowsAppsChangesTest, RemoveForEveryoneRemovesEveryVersion) {
    const auto list = sample_list();
    const WindowsApp& versions = app_of(list, L"Contoso.Versions");
    const WindowsApp& alone = app_of(list, L"Contoso.Alone");
    const WindowsApp& theirs = app_of(list, L"Contoso.Theirs");
    const auto result =
        plan_remove_for_everyone(list, std::vector<std::wstring>{versions.id, alone.id, theirs.id}, true);
    EXPECT_TRUE(result.issues.empty());
    EXPECT_EQ(result.planned, (std::vector<std::wstring>{versions.id, alone.id, theirs.id}));
    // The executor deprovisions a provisioned family itself; no provisioning item is planned.
    ASSERT_EQ(result.plan.items.size(), 4u);

    const std::vector<std::wstring> removed{
        full(L"Contoso.Versions", L"1.1.0.0"), full(L"Contoso.Versions", L"1.0.0.0"),
        full(L"Contoso.Alone", L"1.0.0.0"), full(L"Contoso.Theirs", L"1.0.0.0")};
    for (std::size_t i = 0; i < removed.size(); ++i) {
        const auto& item = result.plan.items[i];
        const auto* location = std::get_if<PackageLocation>(&item.location);
        ASSERT_NE(location, nullptr);
        EXPECT_EQ(location->full_name, removed[i]);
        EXPECT_TRUE(location->all_users);
        EXPECT_TRUE(location->user_sid.empty());
        EXPECT_EQ(item.category, Category::Packages);
    }
    EXPECT_EQ(result.plan.items[0].programs, std::vector<std::wstring>{L"Versions"});
}

// A package that gives a desktop program its identity goes with that program: no mode plans it,
// even when another user has it and it is provisioned.
TEST(WindowsAppsChangesTest, PackagesOfDesktopProgramsAreNeverPlanned) {
    PackageListing listing;
    listing.all_users = true;
    PackageInfo sparse = package(L"Microsoft.PowerToys.SparseApp", L"0.102.0.0", {kMe, kOther});
    sparse.external_location = LR"(C:\Users\me\AppData\Local\PowerToys\WinUI3Apps)";
    listing.packages = {sparse};
    listing.provisioned = {sparse.full_name};
    WindowsAppsEnvironment environment;
    environment.listing = std::move(listing);
    environment.current_user_sid = kMe;
    const auto list = list_windows_apps(environment);
    ASSERT_EQ(list.apps.size(), 1u);
    EXPECT_EQ(list.apps[0].protection, AppProtection::ExternalLocation);
    const std::vector<std::wstring> ids{list.apps[0].id};
    for (const auto& result : {plan_remove_for_me(list, ids), plan_deprovision(list, ids),
                               plan_remove_for_everyone(list, ids, true)}) {
        EXPECT_TRUE(result.plan.items.empty());
        EXPECT_TRUE(result.planned.empty());
        EXPECT_EQ(codes(result), std::vector<Code>{Code::Protected});
    }
}

TEST(WindowsAppsChangesTest, IssueCodesHaveNames) {
    EXPECT_EQ(to_string(Code::UnknownId), "unknown-id");
    EXPECT_EQ(to_string(Code::Protected), "protected");
    EXPECT_EQ(to_string(Code::IncompleteListing), "incomplete-listing");
    EXPECT_EQ(to_string(Code::NotInstalledForCurrentUser), "not-installed-for-current-user");
    EXPECT_EQ(to_string(Code::WouldDeletePackage), "would-delete-package");
    EXPECT_EQ(to_string(Code::NotProvisioned), "not-provisioned");
    EXPECT_EQ(to_string(Code::IrreversibleNeedsConfirmation), "irreversible-needs-confirmation");
}

}  // namespace
}  // namespace avel::tools
