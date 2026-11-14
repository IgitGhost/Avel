// The GUI's Windows apps table: its rows, columns and check boxes (protected apps can't be checked,
// "Check often removed", which checked apps each action fits), and the search, category and user
// filters on top. The listing is made up: no test touches real packages.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "windows_apps_model.h"

using namespace Qt::StringLiterals;

namespace avel::app {
namespace {

using tools::AppCategory;
using tools::AppProtection;
using tools::WindowsApp;

constexpr wchar_t kMe[] = L"S-1-5-21-1-2-3-1001";
constexpr wchar_t kOther[] = L"S-1-5-21-1-2-3-1002";

WindowsApp app(std::wstring id, std::wstring name, AppCategory category, bool mine, bool other,
               bool provisioned) {
    WindowsApp result;
    result.id = std::move(id);
    result.display_name = std::move(name);
    result.name = L"Contoso." + result.id;
    result.family_name = result.name + L"_8wekyb3d8bbwe";
    result.publisher = L"Contoso";
    result.version = L"1.2.9.0";
    result.category = category;
    result.provisioned = provisioned;
    if (mine) {
        result.users.push_back({kMe, L"PC\\me", true});
        result.current_user_full_name = result.name + L"_1.2.9.0_x64__8wekyb3d8bbwe";
    }
    if (other) {
        result.users.push_back({kOther, L"PC\\other", false});
    }
    result.packages.push_back({result.name + L"_1.2.9.0_x64__8wekyb3d8bbwe", result.version, {}});
    result.soft_undoable = provisioned || other;
    result.can_remove_for_me = mine && result.soft_undoable;
    result.can_deprovision = provisioned;
    result.can_remove_for_everyone = true;
    return result;
}

WindowsApp protectedApp(std::wstring id, std::wstring name, AppProtection protection) {
    WindowsApp result = app(std::move(id), std::move(name), AppCategory::Other, true, false, true);
    result.protection = protection;
    if (protection == AppProtection::Critical) {
        result.category = AppCategory::Critical;
    }
    result.can_remove_for_me = false;
    result.can_deprovision = false;
    result.can_remove_for_everyone = false;
    return result;
}

class WindowsAppsModelTest : public testing::Test {
protected:
    void SetUp() override {
        auto listing = std::make_shared<WindowsAppsListing>();
        listing->list.all_users = true;
        auto& apps = listing->list.apps;
        apps.push_back(app(L"news", L"Microsoft News", AppCategory::OftenRemoved, true, true, true));
        apps.push_back(app(L"chess", L"Chess", AppCategory::OftenRemoved, true, false, false));
        auto photos = app(L"photos", L"Photos", AppCategory::SemiCritical, true, false, true);
        photos.warning = tools::AppWarning::OpensFilesByDefault;
        photos.version = L"1.2.10.0";
        apps.push_back(photos);
        apps.push_back(app(L"todo", L"To Do", AppCategory::Other, false, true, false));
        apps.push_back(protectedApp(L"store", L"Microsoft Store", AppProtection::Critical));
        auto clipchamp = protectedApp(L"clip", L"Clipchamp", AppProtection::ExternalLocation);
        clipchamp.category = AppCategory::OftenRemoved;  // Protection wins over the category
        clipchamp.external_location = LR"(C:\Program Files\Clipchamp)";
        apps.push_back(clipchamp);
        apps.push_back(protectedApp(L"shell", L"Shell host", AppProtection::System));

        model_.setListing(listing);
        filter_.setSourceAppsModel(&model_);
    }

    QModelIndex cell(const std::wstring& id, int column) const {
        return model_.index(model_.rowOf(id), column);
    }
    QString text(const std::wstring& id, int column) const { return cell(id, column).data().toString(); }
    std::vector<std::wstring> shown() const {
        std::vector<std::wstring> ids;
        for (int row = 0; row < filter_.rowCount(); ++row) {
            ids.push_back(model_.appAt(filter_.mapToSource(filter_.index(row, 0)).row())->id);
        }
        return ids;
    }

    WindowsAppsTableModel model_;
    WindowsAppsFilterModel filter_;
};

TEST_F(WindowsAppsModelTest, ShowsTheColumns) {
    using Model = WindowsAppsTableModel;
    ASSERT_EQ(model_.rowCount(), 7);
    EXPECT_EQ(text(L"news", Model::NameColumn), u"Microsoft News"_s);
    EXPECT_EQ(text(L"news", Model::PublisherColumn), u"Contoso"_s);
    EXPECT_EQ(text(L"news", Model::VersionColumn), u"1.2.9.0"_s);
    EXPECT_EQ(text(L"news", Model::InstalledForColumn),
              u"You and 1 other user(s), new users"_s);  // English plurals come from avel_en.ts
    EXPECT_EQ(text(L"chess", Model::InstalledForColumn), u"You"_s);
    EXPECT_EQ(text(L"todo", Model::InstalledForColumn), u"1 other user(s)"_s);
    EXPECT_EQ(text(L"news", Model::CategoryColumn), u"Often removed"_s);
    EXPECT_TRUE(text(L"photos", Model::CategoryColumn).startsWith(u"⚠"_s));  // Semi-critical: a warning
    EXPECT_FALSE(cell(L"photos", Model::CategoryColumn).data(Qt::ToolTipRole).toString().isEmpty());
    EXPECT_EQ(text(L"store", Model::CategoryColumn), u"Critical"_s);
    EXPECT_EQ(text(L"clip", Model::CategoryColumn), u"Part of a desktop program"_s);
    EXPECT_EQ(text(L"shell", Model::CategoryColumn), u"Part of Windows"_s);
    // The protection's reason, with the desktop program's folder.
    EXPECT_TRUE(cell(L"clip", Model::NameColumn)
                    .data(Qt::ToolTipRole)
                    .toString()
                    .contains(u"C:\\Program Files\\Clipchamp"_s));
    EXPECT_EQ(model_.rowOf(L"NEWS"), 0);
    EXPECT_EQ(model_.rowOf(L"zz"), -1);
}

TEST_F(WindowsAppsModelTest, NothingIsCheckedAndProtectedAppsCantBe) {
    using Model = WindowsAppsTableModel;
    EXPECT_TRUE(model_.checkedApps().empty());
    EXPECT_EQ(cell(L"news", Model::CheckColumn).data(Qt::CheckStateRole).value<Qt::CheckState>(),
              Qt::Unchecked);
    EXPECT_TRUE(model_.flags(cell(L"news", Model::CheckColumn)).testFlag(Qt::ItemIsUserCheckable));
    EXPECT_FALSE(model_.flags(cell(L"news", Model::NameColumn)).testFlag(Qt::ItemIsUserCheckable));

    for (const auto* id : {L"store", L"clip", L"shell"}) {
        const QModelIndex check = cell(id, Model::CheckColumn);
        EXPECT_FALSE(model_.flags(check).testFlag(Qt::ItemIsUserCheckable)) << id;
        EXPECT_FALSE(check.data(Qt::CheckStateRole).isValid()) << id;  // No check box at all
        EXPECT_FALSE(model_.setData(check, Qt::Checked, Qt::CheckStateRole)) << id;
        EXPECT_FALSE(model_.setChecked(id, true)) << id;
        // Still selectable, for its details.
        EXPECT_TRUE(model_.flags(check).testFlag(Qt::ItemIsSelectable)) << id;
    }
    EXPECT_TRUE(model_.checkedApps().empty());

    int changes = 0;
    QObject::connect(&model_, &WindowsAppsTableModel::checksChanged, [&] { ++changes; });
    EXPECT_TRUE(model_.setData(cell(L"todo", Model::CheckColumn), Qt::Checked, Qt::CheckStateRole));
    EXPECT_TRUE(model_.isChecked(L"TODO"));
    EXPECT_EQ(cell(L"todo", Model::CheckColumn).data(Qt::CheckStateRole).value<Qt::CheckState>(),
              Qt::Checked);
    EXPECT_EQ(changes, 1);
    model_.uncheckAll();
    EXPECT_TRUE(model_.checkedIds().empty());
    EXPECT_EQ(changes, 2);
}

TEST_F(WindowsAppsModelTest, ChecksOftenRemovedOnlyWhereTheyCanBeChanged) {
    EXPECT_EQ(model_.checkOftenRemoved(), 2);  // Not Clipchamp: protected
    EXPECT_EQ(model_.checkedIds(), (std::vector<std::wstring>{L"news", L"chess"}));
    EXPECT_EQ(model_.checkOftenRemoved(), 0);  // Nothing new

    // A refresh keeps the checks of the apps still listed and checkable.
    auto listing = std::make_shared<WindowsAppsListing>(*model_.listing());
    listing->list.apps.erase(listing->list.apps.begin());  // News is gone
    model_.setListing(listing);
    EXPECT_EQ(model_.checkedIds(), (std::vector<std::wstring>{L"chess"}));
}

TEST_F(WindowsAppsModelTest, TellsWhichCheckedAppsEachActionFits) {
    ASSERT_TRUE(model_.setChecked(L"news", true));   // Mine, another user's, provisioned
    ASSERT_TRUE(model_.setChecked(L"chess", true));  // Mine alone: removing it for me would delete it
    ASSERT_TRUE(model_.setChecked(L"todo", true));   // Another user's only
    const auto checked = model_.checkedApps();
    ASSERT_EQ(checked.size(), 3u);
    EXPECT_EQ(WindowsAppsTableModel::fitting(checked, WindowsAppsAction::RemoveForMe), 1);
    EXPECT_EQ(WindowsAppsTableModel::fitting(checked, WindowsAppsAction::Deprovision), 1);
    EXPECT_EQ(WindowsAppsTableModel::fitting(checked, WindowsAppsAction::RemoveForEveryone), 3);

    const auto* store = model_.appAt(model_.rowOf(L"store"));
    for (const auto action : {WindowsAppsAction::RemoveForMe, WindowsAppsAction::Deprovision,
                              WindowsAppsAction::RemoveForEveryone}) {
        EXPECT_FALSE(WindowsAppsTableModel::fits(*store, action));
    }
}

TEST_F(WindowsAppsModelTest, FiltersByCategoryUserAndSearch) {
    using Category = WindowsAppsFilterModel::Category;
    using User = WindowsAppsFilterModel::User;
    EXPECT_EQ(filter_.rowCount(), 7);

    filter_.setCategory(Category::Removable);
    EXPECT_EQ(shown(), (std::vector<std::wstring>{L"news", L"chess", L"photos", L"todo"}));
    filter_.setCategory(Category::OftenRemoved);
    EXPECT_EQ(shown(), (std::vector<std::wstring>{L"news", L"chess", L"clip"}));
    filter_.setCategory(Category::SemiCritical);
    EXPECT_EQ(shown(), (std::vector<std::wstring>{L"photos"}));
    filter_.setCategory(Category::Protected);
    EXPECT_EQ(shown(), (std::vector<std::wstring>{L"store", L"clip", L"shell"}));
    filter_.setCategory(Category::All);

    filter_.setUser({User::Kind::Sid, L"s-1-5-21-1-2-3-1002"});  // Compared ignoring case
    EXPECT_EQ(shown(), (std::vector<std::wstring>{L"news", L"todo"}));
    filter_.setUser({User::Kind::NewUsers, {}});
    EXPECT_EQ(shown(), (std::vector<std::wstring>{L"news", L"photos", L"store", L"clip", L"shell"}));
    filter_.setUser({});

    filter_.setSearchText(u"  microsoft "_s);  // The display name
    EXPECT_EQ(shown(), (std::vector<std::wstring>{L"news", L"store"}));
    filter_.setSearchText(u"contoso.todo_8wek"_s);  // The family name
    EXPECT_EQ(shown(), (std::vector<std::wstring>{L"todo"}));
    filter_.setSearchText(u"nothing like it"_s);
    EXPECT_EQ(filter_.rowCount(), 0);
}

TEST_F(WindowsAppsModelTest, SortsVersionsByNumber) {
    filter_.sort(WindowsAppsTableModel::VersionColumn, Qt::DescendingOrder);
    EXPECT_EQ(shown().front(), L"photos");  // 1.2.10.0 after 1.2.9.0
}

}  // namespace
}  // namespace avel::app
