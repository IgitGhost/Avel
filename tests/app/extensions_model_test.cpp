// The GUI's browser extensions table: its rows and columns, the search, browser, user and "only
// removable" filters on top, and the command line an extensions page is started with.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "extensions_model.h"
#include "unelevated_launch.h"

using namespace Qt::StringLiterals;

namespace avel::app {
namespace {

using tools::BrowserExtension;
using tools::BrowserKind;
using tools::ExtensionInstall;
using tools::ExtensionRemoval;

BrowserExtension entry(std::wstring id, BrowserKind browser, std::wstring name, std::wstring sid,
                       ExtensionRemoval removal = ExtensionRemoval::NotRemovable) {
    BrowserExtension result;
    result.id = std::move(id);
    result.browser = browser;
    result.name = std::move(name);
    result.user_sid = std::move(sid);
    result.user_name = result.user_sid.empty() ? std::wstring() : L"PC\\" + result.user_sid;
    result.profile_directory = L"Default";
    result.profile_name = L"Person 1";
    result.removal = removal;
    return result;
}

class ExtensionsModelTest : public testing::Test {
protected:
    void SetUp() override {
        auto listing = std::make_shared<ExtensionsListing>();
        listing->currentUserSid = L"S-1-5-21-1";

        auto helper =
            entry(L"a1", BrowserKind::Chrome, L"Contoso Helper", L"S-1-5-21-1", ExtensionRemoval::Removable);
        helper.extension_id = L"abcdefghijklmnopabcdefghijklmnop";
        helper.install = ExtensionInstall::ExternalRegistry;
        helper.version = L"1.10.0";
        listing->list.extensions.push_back(helper);
        auto blocker = entry(L"b2", BrowserKind::Edge, L"Ad Blocker", L"S-1-5-21-2");
        blocker.install = ExtensionInstall::Store;
        blocker.from_store = true;
        blocker.store = tools::ExtensionStore::EdgeAddons;
        blocker.enabled = false;
        blocker.version = L"1.9.0";
        blocker.description = L"Blocks fabrikam ads";
        listing->list.extensions.push_back(blocker);
        auto forced = entry(L"c3", BrowserKind::Chrome, {}, {}, ExtensionRemoval::Removable);
        forced.extension_id = L"ponmlkjihgfedcbaponmlkjihgfedcba";
        forced.in_profile = false;
        forced.profile_directory.clear();
        forced.profile_name.clear();
        forced.install = ExtensionInstall::Policy;
        listing->list.extensions.push_back(forced);
        auto managed = entry(L"d4", BrowserKind::Firefox, L"Litware", L"S-1-5-21-1",
                             ExtensionRemoval::ManagedByPoliciesJson);
        listing->list.extensions.push_back(managed);

        model_.setListing(listing);
        filter_.setSourceExtensionsModel(&model_);
    }

    QString text(int row, int column) const { return model_.index(row, column).data().toString(); }

    std::wstring shownId(int row) const {
        return model_.entryAt(filter_.mapToSource(filter_.index(row, 0)).row())->id;
    }

    ExtensionsTableModel model_;
    ExtensionsFilterModel filter_;
};

TEST_F(ExtensionsModelTest, ShowsTheColumns) {
    ASSERT_EQ(model_.rowCount(), 4);
    EXPECT_EQ(text(0, ExtensionsTableModel::NameColumn), u"Contoso Helper"_s);
    EXPECT_EQ(text(0, ExtensionsTableModel::BrowserColumn), u"Google Chrome"_s);
    EXPECT_EQ(text(0, ExtensionsTableModel::ProfileColumn), u"Person 1"_s);
    EXPECT_EQ(text(0, ExtensionsTableModel::UserColumn), u"PC\\S-1-5-21-1 (you)"_s);
    EXPECT_EQ(text(0, ExtensionsTableModel::StateColumn), u"Enabled"_s);
    EXPECT_EQ(text(0, ExtensionsTableModel::InstallColumn), u"Registry"_s);
    EXPECT_EQ(text(0, ExtensionsTableModel::StoreColumn), u"No"_s);
    EXPECT_EQ(text(0, ExtensionsTableModel::RemovalColumn), u"Removable"_s);
    EXPECT_EQ(text(1, ExtensionsTableModel::StateColumn), u"Disabled"_s);
    EXPECT_EQ(text(1, ExtensionsTableModel::StoreColumn), u"Yes"_s);
    EXPECT_EQ(text(1, ExtensionsTableModel::UserColumn), u"PC\\S-1-5-21-2"_s);
    EXPECT_EQ(text(1, ExtensionsTableModel::RemovalColumn), u"In the browser"_s);
    // A machine policy's extension that no profile has yet: named by its id.
    EXPECT_EQ(text(2, ExtensionsTableModel::NameColumn), u"ponmlkjihgfedcbaponmlkjihgfedcba"_s);
    EXPECT_EQ(text(2, ExtensionsTableModel::ProfileColumn), u"Not in a profile"_s);
    EXPECT_EQ(text(2, ExtensionsTableModel::UserColumn), u"Machine (all users)"_s);
    EXPECT_EQ(text(2, ExtensionsTableModel::StateColumn), u"Not installed"_s);
    EXPECT_EQ(text(3, ExtensionsTableModel::RemovalColumn), u"policies.json"_s);
    EXPECT_EQ(model_.rowOf(L"B2"), 1);
    EXPECT_EQ(model_.rowOf(L"zz"), -1);
    EXPECT_EQ(model_.entryWithId(L"c3")->browser, BrowserKind::Chrome);
}

TEST_F(ExtensionsModelTest, SortsVersionsByNumber) {
    const auto key = [&](int row) {
        return model_.index(row, ExtensionsTableModel::VersionColumn)
            .data(ExtensionsTableModel::SortRole)
            .toString();
    };
    EXPECT_GT(key(0), key(1));  // 1.10.0 after 1.9.0
}

TEST_F(ExtensionsModelTest, FiltersByBrowserUserRemovableAndSearch) {
    EXPECT_EQ(filter_.rowCount(), 4);

    filter_.setBrowser(BrowserKind::Chrome);
    EXPECT_EQ(filter_.rowCount(), 2);
    filter_.setBrowser(std::nullopt);

    filter_.setUser(std::wstring());  // The machine's sources' entries
    ASSERT_EQ(filter_.rowCount(), 1);
    EXPECT_EQ(shownId(0), L"c3");
    filter_.setUser(L"s-1-5-21-1");  // Compared ignoring case
    EXPECT_EQ(filter_.rowCount(), 2);
    filter_.setUser(std::nullopt);

    filter_.setOnlyRemovable(true);
    EXPECT_EQ(filter_.rowCount(), 2);
    filter_.setOnlyRemovable(false);

    filter_.setSearchText(u"  FABRIKAM "_s);  // The description
    ASSERT_EQ(filter_.rowCount(), 1);
    EXPECT_EQ(shownId(0), L"b2");
    filter_.setSearchText(u"abcdefghijklmnop"_s);  // The extension id
    ASSERT_EQ(filter_.rowCount(), 1);
    EXPECT_EQ(shownId(0), L"a1");
    filter_.setSearchText(u"nothing like it"_s);
    EXPECT_EQ(filter_.rowCount(), 0);
}

TEST(UnelevatedLaunchTest, JoinsArgumentsAsTheyAreReadBack) {
    EXPECT_EQ(joinArguments({L"--profile-directory=Default", L"chrome://extensions"}),
              L"--profile-directory=Default chrome://extensions");
    EXPECT_EQ(joinArguments({L"--profile-directory=Profile 1", L"edge://extensions"}),
              L"\"--profile-directory=Profile 1\" edge://extensions");
    EXPECT_EQ(joinArguments({L"-profile", LR"(C:\Users\Ann\AppData\Roaming\Mozilla\Firefox\Profiles\x y\)",
                             L"about:addons"}),
              LR"(-profile "C:\Users\Ann\AppData\Roaming\Mozilla\Firefox\Profiles\x y\\" about:addons)");
    EXPECT_EQ(joinArguments({L"say \"hi\"", L""}), LR"("say \"hi\"" "")");
}

}  // namespace
}  // namespace avel::app
