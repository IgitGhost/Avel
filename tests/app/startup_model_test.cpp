// The GUI's Startup table: its rows and columns, and the search, source and user filters on top.

#include <gtest/gtest.h>

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "startup_model.h"

using namespace Qt::StringLiterals;

namespace avel::app {
namespace {

using tools::StartupEntry;
using tools::StartupKind;
using tools::StartupState;

StartupEntry entry(std::wstring id, StartupKind kind, std::wstring name, std::wstring sid = {},
                   std::wstring command = {}) {
    StartupEntry result;
    result.id = std::move(id);
    result.kind = kind;
    result.name = std::move(name);
    result.user_sid = std::move(sid);
    result.user_name = result.user_sid.empty() ? std::wstring() : L"PC\\" + result.user_sid;
    result.command = std::move(command);
    return result;
}

class StartupModelTest : public testing::Test {
protected:
    void SetUp() override {
        auto listing = std::make_shared<StartupListing>();
        auto inventory = std::make_shared<inventory::Inventory>();
        inventory::Program contoso;
        contoso.id = L"contoso-id";
        contoso.display_name = L"Contoso Suite";
        inventory->programs.push_back(contoso);
        listing->inventory = inventory;

        auto run = entry(L"a1", StartupKind::Run, L"ContosoTray", {}, LR"("C:\Contoso\tray.exe" /min)");
        run.program_id = L"contoso-id";
        run.signer = {true, true, false, L"Contoso Ltd"};
        listing->entries.push_back(run);
        auto service = entry(L"b2", StartupKind::Service, L"FabSvc", {}, LR"(C:\Fabrikam\svc.exe)");
        service.display_name = L"Fabrikam Updater";
        service.state = StartupState::Disabled;
        service.service_start = SERVICE_DISABLED;
        service.product.company_name = L"Fabrikam";
        listing->entries.push_back(service);
        listing->entries.push_back(
            entry(L"c3", StartupKind::Task, L"\\Litware\\Sync", L"S-1-5-21-1", L"sync.exe"));
        listing->entries.push_back(
            entry(L"d4", StartupKind::StartupFolder, L"Notes.lnk", L"S-1-5-21-2", L"notes.exe"));

        model_.setListing(listing);
        filter_.setSourceStartupModel(&model_);
    }

    QString text(int row, int column) const { return model_.index(row, column).data().toString(); }

    StartupTableModel model_;
    StartupFilterModel filter_;
};

TEST_F(StartupModelTest, ShowsTheColumns) {
    ASSERT_EQ(model_.rowCount(), 4);
    EXPECT_EQ(text(0, StartupTableModel::NameColumn), u"ContosoTray"_s);
    EXPECT_EQ(text(0, StartupTableModel::PublisherColumn), u"Contoso Ltd"_s);  // The valid signer
    EXPECT_EQ(text(0, StartupTableModel::UserColumn), u"Machine"_s);
    EXPECT_EQ(text(1, StartupTableModel::NameColumn), u"Fabrikam Updater"_s);  // A service's display name
    EXPECT_EQ(text(1, StartupTableModel::PublisherColumn), u"Fabrikam"_s);     // Unsigned: the company
    EXPECT_EQ(text(1, StartupTableModel::StateColumn), u"Disabled completely"_s);
    EXPECT_EQ(text(2, StartupTableModel::UserColumn), u"PC\\S-1-5-21-1"_s);
    EXPECT_EQ(model_.programName(L"contoso-id"), u"Contoso Suite"_s);
    EXPECT_EQ(model_.rowOf(L"C3"), 2);
    EXPECT_EQ(model_.rowOf(L"zz"), -1);
}

TEST_F(StartupModelTest, FiltersBySourceUserAndSearch) {
    EXPECT_EQ(filter_.rowCount(), 4);

    filter_.setKind(StartupKind::Service);
    ASSERT_EQ(filter_.rowCount(), 1);
    EXPECT_EQ(model_.entryAt(filter_.mapToSource(filter_.index(0, 0)).row())->id, L"b2");
    filter_.setKind(std::nullopt);

    filter_.setUser(std::wstring());  // The machine's
    EXPECT_EQ(filter_.rowCount(), 2);
    filter_.setUser(L"s-1-5-21-2");  // Compared ignoring case
    ASSERT_EQ(filter_.rowCount(), 1);
    EXPECT_EQ(model_.entryAt(filter_.mapToSource(filter_.index(0, 0)).row())->id, L"d4");
    filter_.setUser(std::nullopt);

    filter_.setSearchText(u"  suite "_s);  // The owner program's name
    ASSERT_EQ(filter_.rowCount(), 1);
    EXPECT_EQ(model_.entryAt(filter_.mapToSource(filter_.index(0, 0)).row())->id, L"a1");
    filter_.setSearchText(u"fabrikam\\svc"_s);  // The command
    EXPECT_EQ(filter_.rowCount(), 1);
    filter_.setSearchText(u"nothing like it"_s);
    EXPECT_EQ(filter_.rowCount(), 0);
}

}  // namespace
}  // namespace avel::app
