// The GUI's leftover tree: grouping, containment, tri-state checks, presets and the plan it
// hands to the executor; value edits and what install traces saw change, and the text for them;
// the text for state changes.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/changes/change_set.h"
#include "core/leftovers/scan.h"
#include "core/leftovers/startup_approved.h"
#include "core/leftovers/value_edits.h"
#include "leftover_model.h"
#include "leftover_text.h"

namespace avel::app {
namespace {

using leftovers::Category;
using leftovers::Confidence;
using leftovers::Evidence;
using leftovers::Leftover;
using leftovers::Record;
using Model = LeftoverModel;

// Records that give each level: +20, +4, 0 and -7.
std::vector<Record> records_for(Confidence confidence) {
    switch (confidence) {
        case Confidence::VeryGood:
            return {{Evidence::UninstallerKey}};
        case Confidence::Good:
            return {{Evidence::ExplicitConnection}};
        case Confidence::Questionable:
            return {{Evidence::Depth, 1}};
        default:
            return {{Evidence::DirectoryStillUsed}};
    }
}

Leftover folder(std::wstring path, Confidence confidence, std::wstring owner = L"a",
                std::optional<std::size_t> inside = std::nullopt) {
    Leftover leftover;
    leftover.location = leftovers::FileLocation{std::move(path), true};
    leftover.records = records_for(confidence);
    leftover.owner_ids = {std::move(owner)};
    leftover.category = Category::Files;
    leftover.inside = inside;
    return leftover;
}

Leftover key(std::wstring path, Confidence confidence,
             leftovers::Scanner scanner = leftovers::Scanner::SoftwareKey, std::wstring owner = L"a") {
    Leftover leftover;
    leftover.location = leftovers::RegistryKeyLocation{{platform::Hive::Machine, L"", std::move(path)}};
    leftover.records = records_for(confidence);
    leftover.owner_ids = {std::move(owner)};
    leftover.category = Category::Registry;
    leftover.found_by = scanner;
    return leftover;
}

Leftover package(std::wstring name, Confidence confidence) {
    Leftover leftover;
    leftover.location = leftovers::PackageLocation{std::move(name), L"S-1-5-21-1"};
    leftover.records = records_for(confidence);
    leftover.owner_ids = {L"a"};
    leftover.category = Category::Packages;
    leftover.found_by = leftovers::Scanner::RelatedPackages;
    return leftover;
}

Leftover firewall_rule(std::wstring id) {
    Leftover leftover;
    leftover.location = leftovers::FirewallRuleLocation{std::move(id)};
    leftover.records = records_for(Confidence::Good);
    leftover.owner_ids = {L"a"};
    leftover.category = Category::Firewall;
    leftover.found_by = leftovers::Scanner::Firewall;
    return leftover;
}

Leftover service(std::wstring name, bool driver) {
    Leftover leftover;
    leftover.location = leftovers::ServiceLocation{std::move(name), driver};
    leftover.records = records_for(Confidence::VeryGood);
    leftover.owner_ids = {L"a"};
    leftover.category = Category::Services;
    leftover.found_by = leftovers::Scanner::Services;
    return leftover;
}

platform::RawValue string_value(std::wstring name, std::wstring_view text, DWORD type = REG_EXPAND_SZ) {
    platform::RawValue value{std::move(name), type, {}};
    value.data.resize((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(value.data.data(), text.data(), text.size() * sizeof(wchar_t));
    return value;
}

// An install trace's value edit, as Scanner::Trace reports it.
Leftover value_edit(leftovers::ValueEditLocation edit, std::vector<Record> records) {
    Leftover leftover;
    leftover.location = std::move(edit);
    leftover.records = std::move(records);
    leftover.owner_ids = {L"a"};
    leftover.category = Category::Registry;
    leftover.found_by = leftovers::Scanner::Trace;
    return leftover;
}

// An item an install trace recorded.
leftovers::TracedItem traced(leftovers::Location location, leftovers::TracedChange change) {
    leftovers::TracedItem item;
    item.category = leftovers::category_of(location);
    item.location = std::move(location);
    item.change = change;
    return item;
}

class LeftoverModelTest : public ::testing::Test {
protected:
    void load(std::vector<Leftover> items, Model::Preset preset = Model::Preset::Moderate) {
        auto scan = std::make_shared<leftovers::ScanResult>();
        scan->leftovers = std::move(items);
        model.setScan(scan, {{L"a", QStringLiteral("Contoso")}, {L"b", QStringLiteral("Fabrikam")}}, preset);
    }

    QModelIndex find(const QString& text, const QModelIndex& parent = {}) const {
        for (int row = 0; row < model.rowCount(parent); ++row) {
            const QModelIndex index = model.index(row, Model::ItemColumn, parent);
            if (index.data().toString() == text) {
                return index;
            }
            if (const QModelIndex found = find(text, index); found.isValid()) {
                return found;
            }
        }
        return {};
    }

    Qt::CheckState state(const QModelIndex& index) const {
        return static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());
    }

    void set(const QModelIndex& index, Qt::CheckState state) {
        ASSERT_TRUE(model.setData(index, state, Qt::CheckStateRole));
    }

    std::vector<std::wstring> planned() const {
        std::vector<std::wstring> paths;
        for (const auto& item : model.plan(L"test").items) {
            paths.push_back(LeftoverText::locationText(item.location).toStdWString());
        }
        return paths;
    }

    Model model;
};

TEST_F(LeftoverModelTest, GroupsByProgramThenGroupWithContainedItemsUnderTheirContainer) {
    load({
        folder(LR"(C:\Program Files\Contoso)", Confidence::VeryGood),                             // 0
        folder(LR"(C:\Program Files\Contoso\bin)", Confidence::Good, L"a", 0),                    // 1
        key(LR"(SOFTWARE\Classes\CLSID\{1})", Confidence::Good, leftovers::Scanner::ComClasses),  // 2
        key(LR"(SOFTWARE\Contoso)", Confidence::Good),                                            // 3
        folder(LR"(C:\ProgramData\Fabrikam)", Confidence::Good, L"b"),                            // 4
    });

    ASSERT_EQ(model.rowCount(), 2);
    const QModelIndex contoso = model.index(0, 0);
    EXPECT_EQ(contoso.data().toString(), QStringLiteral("Contoso"));
    EXPECT_EQ(model.index(1, 0).data().toString(), QStringLiteral("Fabrikam"));
    ASSERT_EQ(model.rowCount(contoso), 3);
    EXPECT_EQ(model.index(0, 0, contoso).data().toString(), QStringLiteral("Files and folders (2)"));
    EXPECT_EQ(model.index(1, 0, contoso).data().toString(), QStringLiteral("Registry (1)"));
    EXPECT_EQ(model.index(2, 0, contoso).data().toString(),
              QStringLiteral("COM classes and shell extensions (1)"));

    const QModelIndex files = model.index(0, 0, contoso);
    ASSERT_EQ(model.rowCount(files), 1);
    const QModelIndex install = model.index(0, 0, files);
    EXPECT_EQ(install.data().toString(), QStringLiteral(R"(C:\Program Files\Contoso)"));
    ASSERT_EQ(model.rowCount(install), 1);
    // Shown relative to its container.
    EXPECT_EQ(model.index(0, 0, install).data().toString(), QStringLiteral("bin"));
    EXPECT_EQ(model.index(0, 0, install).parent(), install);
    EXPECT_EQ(model.data(model.index(0, Model::TypeColumn, files)).toString(), QStringLiteral("Folder"));
    EXPECT_EQ(model.data(model.index(0, Model::ConfidenceColumn, files)).toString(),
              QStringLiteral("Very likely"));
    EXPECT_EQ(model.itemCount(), 5);
}

TEST_F(LeftoverModelTest, PresetsCheckByConfidenceAndUnlikelyItemsStayHidden) {
    load({
        folder(LR"(C:\A)", Confidence::VeryGood),
        folder(LR"(C:\B)", Confidence::Good),
        folder(LR"(C:\C)", Confidence::Questionable),
        folder(LR"(C:\D)", Confidence::Bad),
    });
    EXPECT_EQ(model.itemCount(), 3);
    EXPECT_EQ(model.hiddenCount(), 1);
    EXPECT_FALSE(find(QStringLiteral(R"(C:\D)")).isValid());
    EXPECT_EQ(planned(), (std::vector<std::wstring>{LR"(C:\A)", LR"(C:\B)"}));

    model.applyPreset(Model::Preset::Safe);
    EXPECT_EQ(planned(), (std::vector<std::wstring>{LR"(C:\A)"}));
    model.applyPreset(Model::Preset::Advanced);
    EXPECT_EQ(planned(), (std::vector<std::wstring>{LR"(C:\A)", LR"(C:\B)", LR"(C:\C)"}));

    model.setShowUnlikely(true);
    EXPECT_EQ(model.itemCount(), 4);
    EXPECT_EQ(model.hiddenCount(), 0);
    const QModelIndex unlikely = find(QStringLiteral(R"(C:\D)"));
    ASSERT_TRUE(unlikely.isValid());
    EXPECT_EQ(state(unlikely), Qt::Unchecked);
    set(unlikely, Qt::Checked);
    EXPECT_EQ(model.checkedCount(), 4);

    // Hidden again: never removed, whatever its check.
    model.setShowUnlikely(false);
    EXPECT_EQ(planned(), (std::vector<std::wstring>{LR"(C:\A)", LR"(C:\B)", LR"(C:\C)"}));
}

TEST_F(LeftoverModelTest, CheckedContainerTakesItsContentsAlong) {
    load({
        folder(LR"(C:\Program Files\Contoso)", Confidence::Good),
        folder(LR"(C:\Program Files\Contoso\data)", Confidence::Questionable, L"a", 0),
        folder(LR"(C:\Program Files\Contoso\logs)", Confidence::Good, L"a", 0),
    });
    // The uncertain folder goes with its container, so it shows checked.
    const QModelIndex container = find(QStringLiteral(R"(C:\Program Files\Contoso)"));
    const QModelIndex data = find(QStringLiteral("data"), container);
    const QModelIndex logs = find(QStringLiteral("logs"), container);
    EXPECT_EQ(state(container), Qt::Checked);
    EXPECT_EQ(state(data), Qt::Checked);
    EXPECT_EQ(model.checkedCount(), 3);

    // Keeping a child means keeping its container; its other contents still go.
    set(data, Qt::Unchecked);
    EXPECT_EQ(state(data), Qt::Unchecked);
    EXPECT_EQ(state(container), Qt::PartiallyChecked);
    EXPECT_EQ(state(logs), Qt::Checked);
    EXPECT_EQ(state(container.parent()), Qt::PartiallyChecked);
    EXPECT_EQ(planned(), (std::vector<std::wstring>{LR"(C:\Program Files\Contoso\logs)"}));

    // Checking the container again checks what's in it.
    set(container, Qt::Checked);
    EXPECT_EQ(state(data), Qt::Checked);
    EXPECT_EQ(model.checkedCount(), 3);

    set(container, Qt::Unchecked);
    EXPECT_EQ(model.checkedCount(), 0);
    EXPECT_EQ(state(logs), Qt::Unchecked);
}

TEST_F(LeftoverModelTest, HiddenContainerLeavesItsContentsInTheGroup) {
    load({
        folder(LR"(C:\ProgramData\Contoso)", Confidence::Bad),
        folder(LR"(C:\ProgramData\Contoso\Cache)", Confidence::Good, L"a", 0),
    });
    const QModelIndex cache = find(QStringLiteral(R"(C:\ProgramData\Contoso\Cache)"));
    ASSERT_TRUE(cache.isValid());
    EXPECT_EQ(cache.parent().data().toString(), QStringLiteral("Files and folders (1)"));
    EXPECT_EQ(state(cache), Qt::Checked);

    model.setShowUnlikely(true);
    const QModelIndex container = find(QStringLiteral(R"(C:\ProgramData\Contoso)"));
    ASSERT_TRUE(container.isValid());
    EXPECT_EQ(state(container), Qt::PartiallyChecked);
    EXPECT_EQ(find(QStringLiteral("Cache"), container).parent(), container);
    EXPECT_EQ(planned(), (std::vector<std::wstring>{LR"(C:\ProgramData\Contoso\Cache)"}));
}

TEST_F(LeftoverModelTest, GroupChecksCoverTheirItemsButNotReportOnlyOnes) {
    load({
        key(LR"(SOFTWARE\Contoso)", Confidence::Good),
        key(LR"(SOFTWARE\Contoso Updater)", Confidence::Questionable),
        firewall_rule(L"{rule}"),
    });
    const QModelIndex program = model.index(0, 0);
    const QModelIndex registry = model.index(0, 0, program);
    const QModelIndex firewall = model.index(1, 0, program);
    EXPECT_EQ(firewall.data().toString(), QStringLiteral("Firewall rules (shown only) (1)"));
    EXPECT_FALSE(firewall.flags().testFlag(Qt::ItemIsUserCheckable));
    EXPECT_FALSE(model.index(0, 0, firewall).flags().testFlag(Qt::ItemIsUserCheckable));
    EXPECT_FALSE(model.index(0, 0, firewall).data(Qt::CheckStateRole).isValid());
    EXPECT_EQ(state(registry), Qt::PartiallyChecked);
    EXPECT_EQ(state(program), Qt::PartiallyChecked);

    set(program, Qt::Checked);
    EXPECT_EQ(state(registry), Qt::Checked);
    EXPECT_EQ(state(program), Qt::Checked);
    EXPECT_EQ(model.checkedCount(), 2);
    EXPECT_EQ(model.plan(L"x").items.size(), 2u);

    set(registry, Qt::Unchecked);
    EXPECT_EQ(model.checkedCount(), 0);
    EXPECT_EQ(state(program), Qt::Unchecked);
}

TEST_F(LeftoverModelTest, WindowsInstallerRegistrationHasAGroupOfItsOwn) {
    load({
        key(LR"(SOFTWARE\Contoso)", Confidence::Good),
        key(LR"(SOFTWARE\Classes\Installer\Products\96F071321C0420726200000010000000)", Confidence::VeryGood,
            leftovers::Scanner::MsiRegistration),
        key(LR"(SOFTWARE\Classes\CLSID\{1})", Confidence::Good, leftovers::Scanner::ComClasses),
    });
    const QModelIndex program = model.index(0, 0);
    ASSERT_EQ(model.rowCount(program), 3);
    EXPECT_EQ(model.index(0, 0, program).data().toString(), QStringLiteral("Registry (1)"));
    EXPECT_EQ(model.index(1, 0, program).data().toString(),
              QStringLiteral("COM classes and shell extensions (1)"));
    const QModelIndex installer = model.index(2, 0, program);
    EXPECT_EQ(installer.data().toString(), QStringLiteral("Windows Installer registration (1)"));
    EXPECT_EQ(state(installer), Qt::Checked);
    EXPECT_EQ(model.plan(L"x").items.size(), 3u);
}

TEST_F(LeftoverModelTest, DriversAreShownButNeverRemoved) {
    load({service(L"ContosoService", false), service(L"ContosoFilter", true)});
    const QModelIndex services = model.index(0, 0, model.index(0, 0));
    ASSERT_EQ(model.rowCount(services), 2);
    const QModelIndex driver = find(QStringLiteral("ContosoFilter"));
    ASSERT_TRUE(driver.isValid());
    EXPECT_EQ(model.index(driver.row(), Model::TypeColumn, driver.parent()).data().toString(),
              QStringLiteral("Driver (shown only)"));
    EXPECT_EQ(model.index(0, Model::TypeColumn, services).data().toString(), QStringLiteral("Service"));
    EXPECT_FALSE(driver.flags().testFlag(Qt::ItemIsUserCheckable));
    EXPECT_FALSE(driver.data(Qt::CheckStateRole).isValid());
    EXPECT_EQ(model.checkedCount(), 1);

    model.applyPreset(Model::Preset::Advanced);
    set(services, Qt::Checked);
    EXPECT_EQ(planned(), (std::vector<std::wstring>{L"ContosoService"}));
}

TEST_F(LeftoverModelTest, PlanNamesTheProgramsAndKeepsScanOrder) {
    auto shared = folder(LR"(C:\ProgramData\Shared)", Confidence::Good, L"b");
    shared.owner_ids = {L"b", L"a"};
    load({folder(LR"(C:\A)", Confidence::Good), shared});
    // Listed under the first program in the dialog's order.
    EXPECT_EQ(find(QStringLiteral(R"(C:\ProgramData\Shared)")).parent().parent().data().toString(),
              QStringLiteral("Contoso"));

    const auto plan = model.plan(L"Leftovers of 2 programs");
    EXPECT_EQ(plan.description, L"Leftovers of 2 programs");
    ASSERT_EQ(plan.items.size(), 2u);
    EXPECT_EQ(plan.items[1].programs, (std::vector<std::wstring>{L"Fabrikam", L"Contoso"}));
    EXPECT_EQ(plan.items[1].confidence, Confidence::Good);
    EXPECT_EQ(plan.items[1].score, 4);
}

TEST_F(LeftoverModelTest, PackagesCanBeLeftOutTogether) {
    load({package(L"Contoso_1.0_neutral__x", Confidence::Good), folder(LR"(C:\A)", Confidence::Good)});
    EXPECT_EQ(model.checkedPackageCount(), 1);
    model.uncheckPackages();
    EXPECT_EQ(model.checkedPackageCount(), 0);
    EXPECT_EQ(planned(), (std::vector<std::wstring>{LR"(C:\A)"}));
}

TEST_F(LeftoverModelTest, ExplainsWhyItMatched) {
    auto item = folder(LR"(C:\Program Files\Contoso\data)", Confidence::Good);
    item.records = {{Evidence::ProductNamePerfectMatch},
                    {Evidence::PathReference, 0, leftovers::ReferenceKind::ComServer, L"HKLM\\x"},
                    {Evidence::QuestionableDirectoryName},
                    {Evidence::Depth, 0}};
    load({item}, Model::Preset::Advanced);
    const QModelIndex row = find(QStringLiteral(R"(C:\Program Files\Contoso\data)"));
    const QString why = model.index(row.row(), Model::ReasonColumn, row.parent()).data().toString();
    EXPECT_EQ(why,
              QStringLiteral("Points into the program's folder (COM server); Named exactly like the program; "
                             "against it: Generic name, such as “data” or “config”"));
    const QString details =
        model.index(row.row(), Model::ReasonColumn, row.parent()).data(Qt::ToolTipRole).toString();
    EXPECT_TRUE(details.contains(QStringLiteral("HKLM\\x")));
    EXPECT_TRUE(details.contains(QStringLiteral("Score +5: Very likely")));
}

TEST_F(LeftoverModelTest, EvidenceWorthNoPointsStillExplainsTheItem) {
    auto orphan = folder(LR"(C:\ProgramData\Northwind)", Confidence::Questionable);
    orphan.records = {{Evidence::NoOwner}, {Evidence::Depth, 1}};
    orphan.found_by = leftovers::Scanner::Orphans;
    load({orphan});
    const QModelIndex row = find(QStringLiteral(R"(C:\ProgramData\Northwind)"));
    ASSERT_TRUE(row.isValid());
    EXPECT_EQ(model.index(row.row(), Model::ReasonColumn, row.parent()).data().toString(),
              QStringLiteral("No installed program owns it"));
}

// Value edits (install traces) are a group of their own, described as what they do, and checked
// by their confidence like any item.
TEST_F(LeftoverModelTest, ValueEditsHaveAGroupOfTheirOwnDescribedInWords) {
    using leftovers::ValueEditKind;
    leftovers::ValueEditLocation path;
    path.key = {platform::Hive::Machine, L"",
                LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)"};
    path.value_name = L"Path";
    path.kind = ValueEditKind::RemovePathEntries;
    path.expected = string_value(L"Path", LR"(C:\Windows;C:\Contoso\bin)");
    path.desired = string_value(L"Path", LR"(C:\Windows)");
    path.entries = {LR"(C:\Contoso\bin)"};

    leftovers::ValueEditLocation dll;
    dll.key = {platform::Hive::Machine, L"", LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\SharedDLLs)"};
    dll.value_name = LR"(C:\Program Files\Common Files\Contoso\shared.dll)";
    dll.kind = ValueEditKind::DecrementSharedDll;
    dll.expected = leftovers::with_shared_dll_count({dll.value_name, REG_DWORD, {}}, 2);
    dll.desired = leftovers::with_shared_dll_count({dll.value_name, REG_DWORD, {}}, 1);
    dll.delta = 1;

    // A user Avel doesn't know of is named by their SID.
    leftovers::ValueEditLocation restore;
    restore.key = {platform::Hive::User, L"S-1-5-21-1-2-3-9999", L"Environment"};
    restore.value_name = L"CONTOSO_MODE";
    restore.kind = ValueEditKind::RestoreValue;
    restore.expected = string_value(L"CONTOSO_MODE", L"new", REG_SZ);
    restore.desired = string_value(L"CONTOSO_MODE", L"old", REG_SZ);

    leftovers::ValueEditLocation created = path;
    created.value_name = L"CONTOSO_HOME";
    created.kind = ValueEditKind::DeleteValue;
    created.expected = string_value(L"CONTOSO_HOME", LR"(C:\Contoso)");
    created.desired.reset();
    created.entries.clear();

    load({
        key(LR"(SOFTWARE\Contoso)", Confidence::Good),
        value_edit(path,
                   {{Evidence::TracedValueUndo},
                    {Evidence::PathReference, 0, leftovers::ReferenceKind::None, LR"(C:\Contoso\bin)"}}),
        value_edit(dll, {{Evidence::TracedValueUndo}}),
        value_edit(restore, {{Evidence::TracedValueRestore}}),
        value_edit(created, {{Evidence::TracedValueUndo}}),
    });
    const QModelIndex program = model.index(0, 0);
    ASSERT_EQ(model.rowCount(program), 2);
    EXPECT_EQ(model.index(0, 0, program).data().toString(), QStringLiteral("Registry (1)"));
    const QModelIndex edits = model.index(1, 0, program);
    EXPECT_EQ(edits.data().toString(), QStringLiteral("Environment and shared DLLs (4)"));
    ASSERT_EQ(model.rowCount(edits), 4);

    const auto text = [&](int row, int column = Model::ItemColumn) {
        return model.index(row, column, edits).data().toString();
    };
    EXPECT_EQ(text(0), QStringLiteral(R"(Remove C:\Contoso\bin from the system variable Path)"));
    EXPECT_EQ(
        text(1),
        QStringLiteral(R"(Lower the shared DLL count of C:\Program Files\Common Files\Contoso\shared.dll )"
                       "from 2 to 1"));
    EXPECT_EQ(text(2),
              QStringLiteral("Restore the variable CONTOSO_MODE of user S-1-5-21-1-2-3-9999 to its value "
                             "before the installation"));
    EXPECT_EQ(text(3),
              QStringLiteral("Delete the system variable CONTOSO_HOME, which the installer created"));
    EXPECT_EQ(text(0, Model::TypeColumn), QStringLiteral("Entries in an environment variable"));
    EXPECT_EQ(text(1, Model::TypeColumn), QStringLiteral("Shared DLL reference count"));
    EXPECT_EQ(text(2, Model::TypeColumn), QStringLiteral("Environment variable to restore"));
    EXPECT_EQ(text(0, Model::ConfidenceColumn), QStringLiteral("Very likely"));
    EXPECT_EQ(text(1, Model::ReasonColumn),
              QStringLiteral("Takes back exactly what the program's installer changed (install trace)"));

    // The tooltip names the value, and its text now and afterwards.
    const QString tooltip = model.index(2, 0, edits).data(Qt::ToolTipRole).toString();
    EXPECT_TRUE(tooltip.contains(QStringLiteral(R"(S-1-5-21-1-2-3-9999\Environment : CONTOSO_MODE)")))
        << tooltip.toStdString();
    EXPECT_TRUE(tooltip.contains(QStringLiteral("Now: new")));
    EXPECT_TRUE(tooltip.contains(QStringLiteral("Afterwards: old")));
    EXPECT_TRUE(model.index(3, 0, edits)
                    .data(Qt::ToolTipRole)
                    .toString()
                    .contains(QStringLiteral("Afterwards: deleted")));

    // A restore may undo what the user wants now: never checked by a preset.
    EXPECT_EQ(state(model.index(0, 0, edits)), Qt::Checked);
    EXPECT_EQ(state(model.index(1, 0, edits)), Qt::Checked);
    EXPECT_EQ(state(model.index(2, 0, edits)), Qt::Unchecked);
    EXPECT_EQ(state(edits), Qt::PartiallyChecked);
    const auto plan = model.plan(L"x");
    ASSERT_EQ(plan.items.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<leftovers::ValueEditLocation>(plan.items[1].location));
}

// What a trace saw the installer change of what existed before is listed apart, under the program
// whose trace saw it, and never offered.
TEST_F(LeftoverModelTest, ChangesToWhatExistedBeforeAreShownButNeverChecked) {
    using leftovers::TracedChange;
    const leftovers::RegistryValueLocation theme{{platform::Hive::Machine, L"", LR"(SOFTWARE\Shared)"},
                                                 L"Theme"};
    const leftovers::FileLocation old_file{LR"(C:\ProgramData\Shared\old.txt)", false};
    leftovers::ScanTarget target;
    target.program_id = L"a";
    target.display_name = L"Contoso 1.0";
    target.trace_id = L"20261030-120000-00000001";
    target.traced = {
        traced(leftovers::FileLocation{LR"(C:\Program Files\Contoso)", true}, TracedChange::Created),
        traced(theme, TracedChange::Modified), traced(old_file, TracedChange::Deleted)};
    // A bundle's package shares its trace: each change is listed once.
    leftovers::ScanTarget package = target;
    package.program_id = L"b";
    // A program the dialog didn't name gets a heading of its own, by its target's name.
    leftovers::ScanTarget other;
    other.program_id = L"trace:20261030-120000-00000002";
    other.display_name = L"Northwind";
    other.trace_id = L"20261030-120000-00000002";
    other.traced = {
        traced(leftovers::FileLocation{LR"(C:\Northwind\settings.ini)", false}, TracedChange::Renamed)};

    auto scan = std::make_shared<leftovers::ScanResult>();
    scan->leftovers = {folder(LR"(C:\Program Files\Contoso)", Confidence::VeryGood)};
    scan->targets = {target, package, other};
    model.setScan(scan, {{L"a", QStringLiteral("Contoso")}, {L"b", QStringLiteral("Contoso Core")}},
                  Model::Preset::Moderate);

    ASSERT_EQ(model.rowCount(), 2);
    const QModelIndex contoso = model.index(0, 0);
    ASSERT_EQ(model.rowCount(contoso), 2);
    EXPECT_EQ(model.index(0, 0, contoso).data().toString(), QStringLiteral("Files and folders (1)"));
    const QModelIndex changed = model.index(1, 0, contoso);
    EXPECT_EQ(changed.data().toString(), QStringLiteral("Changed by the installer (2)"));
    EXPECT_FALSE(changed.flags().testFlag(Qt::ItemIsUserCheckable));
    EXPECT_FALSE(changed.data(Qt::CheckStateRole).isValid());
    EXPECT_FALSE(changed.data(Qt::ToolTipRole).toString().isEmpty());
    ASSERT_EQ(model.rowCount(changed), 2);
    const QModelIndex value = model.index(0, 0, changed);
    EXPECT_EQ(value.data().toString(), QStringLiteral(R"(HKLM\SOFTWARE\Shared : Theme)"));
    EXPECT_FALSE(value.flags().testFlag(Qt::ItemIsUserCheckable));
    EXPECT_FALSE(value.data(Qt::CheckStateRole).isValid());
    EXPECT_FALSE(value.data(Model::LeftoverRole).isValid());
    EXPECT_FALSE(model.setData(value, Qt::Checked, Qt::CheckStateRole));
    EXPECT_EQ(model.index(0, Model::TypeColumn, changed).data().toString(), QStringLiteral("Registry value"));
    EXPECT_EQ(model.index(0, Model::ConfidenceColumn, changed).data().toString(),
              QStringLiteral("Shown only"));
    EXPECT_EQ(model.index(0, Model::ReasonColumn, changed).data().toString(),
              QStringLiteral("Changed by the installer; it existed before"));
    EXPECT_EQ(model.index(1, Model::ReasonColumn, changed).data().toString(),
              QStringLiteral("Deleted by the installer; it existed before"));

    const QModelIndex northwind = model.index(1, 0);
    EXPECT_EQ(northwind.data().toString(), QStringLiteral("Northwind"));
    EXPECT_FALSE(northwind.data(Qt::CheckStateRole).isValid());
    EXPECT_EQ(model.index(0, Model::ReasonColumn, model.index(0, 0, northwind)).data().toString(),
              QStringLiteral("Renamed or moved here by the installer; it existed before"));

    // Not leftovers: not counted, not checked, never planned, whatever the preset.
    EXPECT_EQ(model.itemCount(), 1);
    EXPECT_EQ(model.changedCount(), 3);
    EXPECT_EQ(model.checkedCount(), 1);
    EXPECT_EQ(state(contoso), Qt::Checked);
    set(contoso, Qt::Unchecked);
    set(contoso, Qt::Checked);
    model.applyPreset(Model::Preset::Advanced);
    EXPECT_EQ(planned(), (std::vector<std::wstring>{LR"(C:\Program Files\Contoso)"}));
    model.setShowUnlikely(true);
    EXPECT_EQ(model.changedCount(), 3);
    EXPECT_EQ(model.plan(L"x").items.size(), 1u);
}

// A tied item's detail is the tracker's code for why: the tooltip says it in words.
TEST_F(LeftoverModelTest, ExplainsWhyATracedItemIsTiedToTheProgram) {
    auto item = folder(LR"(C:\Program Files\Contoso)", Confidence::VeryGood);
    item.records = {{Evidence::TracedInstallRoot, 0, leftovers::ReferenceKind::None, L"install-folder"}};
    item.found_by = leftovers::Scanner::Trace;
    load({item});
    const QModelIndex row = find(QStringLiteral(R"(C:\Program Files\Contoso)"));
    ASSERT_TRUE(row.isValid());
    const QString details =
        model.index(row.row(), Model::ReasonColumn, row.parent()).data(Qt::ToolTipRole).toString();
    EXPECT_TRUE(details.contains(QStringLiteral("Tied to the program: its install folder, or in it")))
        << details.toStdString();
    EXPECT_FALSE(details.contains(QStringLiteral("install-folder")));
    EXPECT_EQ(LeftoverText::tieText(L"references-tied"),
              QStringLiteral("Tied to the program: refers to another of its items or folders"));
    EXPECT_TRUE(LeftoverText::tieText(L"something-else").isEmpty());
}

TEST_F(LeftoverModelTest, SaysWhatAnInstallTraceBrought) {
    leftovers::TraceUse use;
    use.items = 12;
    use.tied = 10;
    use.value_edits = 2;
    use.shown_only = 3;
    EXPECT_EQ(
        LeftoverText::traceUseText(use, QStringLiteral("Contoso")),
        QStringLiteral("Install trace of Contoso: 12 item(s) its installer created; 10 of them tied to the "
                       "program; 2 edit(s) to environment variables or shared DLL counts; 3 change(s) to "
                       "what existed before, shown only."));
    leftovers::TraceUse gone;
    gone.gone = 4;
    gone.protected_count = 1;
    EXPECT_EQ(LeftoverText::traceUseText(gone, QStringLiteral("Contoso")),
              QStringLiteral("Install trace of Contoso: 0 item(s) its installer created; 4 already gone; 1 "
                             "protected, left in place."));
}

// The user excluded these in the install trace's review; other scanners found them by name. They
// are shown at their level and say why they aren't checked: no preset checks them, nor what holds
// them, since removing that would take them along. Checking them by hand works.
TEST_F(LeftoverModelTest, NoPresetChecksWhatATracesReviewExcluded) {
    const Record excluded{Evidence::ExcludedInTraceReview, 0, leftovers::ReferenceKind::None,
                          L"20261030-120000-00000001"};
    auto install = folder(LR"(C:\Program Files\Contoso)", Confidence::VeryGood);             // 0
    auto shared = folder(LR"(C:\Program Files\Contoso\Shared)", Confidence::Good, L"a", 0);  // 1
    shared.records.push_back(excluded);
    auto logs = folder(LR"(C:\Program Files\Contoso\logs)", Confidence::Good, L"a", 0);  // 2
    auto settings = key(LR"(SOFTWARE\Contoso)", Confidence::VeryGood);                   // 3
    settings.records.push_back(excluded);
    load({install, shared, logs, settings, folder(LR"(C:\ProgramData\Contoso)", Confidence::Good)});

    const QModelIndex container = find(QStringLiteral(R"(C:\Program Files\Contoso)"));
    const QModelIndex sharedRow = find(QStringLiteral("Shared"), container);
    const QModelIndex keyRow = find(QStringLiteral(R"(HKLM\SOFTWARE\Contoso)"));
    ASSERT_TRUE(sharedRow.isValid());
    ASSERT_TRUE(keyRow.isValid());
    EXPECT_EQ(state(sharedRow), Qt::Unchecked);
    EXPECT_EQ(state(keyRow), Qt::Unchecked);
    EXPECT_EQ(state(container), Qt::PartiallyChecked);
    const std::vector<std::wstring> others = {LR"(C:\Program Files\Contoso\logs)",
                                              LR"(C:\ProgramData\Contoso)"};
    EXPECT_EQ(planned(), others);
    model.applyPreset(Model::Preset::Safe);
    EXPECT_TRUE(planned().empty());
    model.applyPreset(Model::Preset::Advanced);
    EXPECT_EQ(planned(), others);
    model.setShowUnlikely(true);
    EXPECT_EQ(planned(), others);

    // Its level stands; the reason comes first, and the tooltips say it too.
    EXPECT_EQ(model.index(keyRow.row(), Model::ConfidenceColumn, keyRow.parent()).data().toString(),
              QStringLiteral("Very likely"));
    EXPECT_EQ(model.index(keyRow.row(), Model::ReasonColumn, keyRow.parent()).data().toString(),
              QStringLiteral("Excluded in the install trace's review: not checked automatically; "
                             "The program's Uninstall entry"));
    const QString details =
        model.index(keyRow.row(), Model::ReasonColumn, keyRow.parent()).data(Qt::ToolTipRole).toString();
    EXPECT_TRUE(details.contains(QStringLiteral("Install trace 20261030-120000-00000001")))
        << details.toStdString();
    EXPECT_TRUE(details.contains(QStringLiteral("Score +20: Very likely")));
    EXPECT_TRUE(
        sharedRow.data(Qt::ToolTipRole)
            .toString()
            .endsWith(QStringLiteral("Excluded in the install trace's review: not checked automatically")));
    EXPECT_TRUE(container.data(Qt::ToolTipRole)
                    .toString()
                    .endsWith(QStringLiteral(
                        "Holds an item excluded in the install trace's review: not checked automatically")));
    EXPECT_FALSE(find(QStringLiteral("logs"), container)
                     .data(Qt::ToolTipRole)
                     .toString()
                     .contains(QStringLiteral("review")));

    // By hand, like any item.
    set(sharedRow, Qt::Checked);
    set(keyRow, Qt::Checked);
    EXPECT_EQ(state(sharedRow), Qt::Checked);
    EXPECT_EQ(state(keyRow), Qt::Checked);
    EXPECT_EQ(planned(), (std::vector<std::wstring>{
                             LR"(C:\Program Files\Contoso\Shared)", LR"(C:\Program Files\Contoso\logs)",
                             LR"(HKLM\SOFTWARE\Contoso)", LR"(C:\ProgramData\Contoso)"}));
    set(container, Qt::Checked);
    EXPECT_EQ(model.plan(L"x").items.size(), 5u);
    // A preset starts over.
    model.applyPreset(Model::Preset::Moderate);
    EXPECT_EQ(planned(), others);

    leftovers::TraceUse use;
    use.items = 3;
    use.excluded = 2;
    EXPECT_EQ(
        LeftoverText::traceUseText(use, QStringLiteral("Contoso")),
        QStringLiteral("Install trace of Contoso: 3 item(s) its installer created; 2 item(s) excluded in "
                       "its review, not checked automatically."));
}

// The executor's outcomes as the removal and the Undo Center show them.
TEST_F(LeftoverModelTest, ValueEditOutcomesAreWordedForEdits) {
    leftovers::ValueEditLocation edit;
    edit.key = {platform::Hive::Machine, L"",
                LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)"};
    edit.value_name = L"Path";
    const leftovers::Location location = edit;
    const leftovers::Location folder_location = leftovers::FileLocation{LR"(C:\Contoso)", true};
    using changes::ItemState;
    using changes::Reason;
    EXPECT_EQ(LeftoverText::statusText({ItemState::Done, Reason::None, 0}, location),
              QStringLiteral("Edited"));
    EXPECT_EQ(LeftoverText::statusText({ItemState::Done, Reason::None, 0}, folder_location),
              QStringLiteral("Removed"));
    // Compare-and-set: the value changed after the scan, so it is left as it is.
    EXPECT_EQ(LeftoverText::statusText({ItemState::Skipped, Reason::Changed, 0}, location),
              QStringLiteral("Skipped: the value has changed since"));
    EXPECT_EQ(LeftoverText::statusText({ItemState::RestoreSkipped, Reason::Changed, 0}, location),
              QStringLiteral("Not restored: the value has changed since"));
}

TEST_F(LeftoverModelTest, StateChangeOutcomesAreWordedForTheirKind) {
    using changes::ItemState;
    using changes::Reason;
    const platform::RegistryAddress run{
        platform::Hive::Machine, L"",
        LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run)"};
    const leftovers::Location disable = leftovers::StartupApprovedLocation{
        run, L"Contoso", std::nullopt, leftovers::disabled_approval(L"Contoso", FILETIME{1, 2})};
    const leftovers::Location enable = leftovers::StartupApprovedLocation{
        run, L"Contoso", std::nullopt, leftovers::enabled_approval(L"Contoso")};
    const leftovers::Location task = leftovers::TaskStateLocation{LR"(\Contoso\Updater)", true, false};
    const leftovers::Location service = leftovers::ServiceStartLocation{
        L"ContosoSvc", {SERVICE_AUTO_START, true}, {SERVICE_DEMAND_START, true}};
    const changes::ItemStatus done{ItemState::Done, Reason::None, 0};
    EXPECT_EQ(LeftoverText::statusText(done, disable), QStringLiteral("Disabled"));
    EXPECT_EQ(LeftoverText::statusText(done, enable), QStringLiteral("Enabled"));
    EXPECT_EQ(LeftoverText::statusText(done, task), QStringLiteral("Disabled"));
    EXPECT_EQ(LeftoverText::statusText(done, service), QStringLiteral("Start type changed"));
    EXPECT_EQ(LeftoverText::statusText({ItemState::Skipped, Reason::Changed, 0}, task),
              QStringLiteral("Skipped: its state has changed since"));
    EXPECT_EQ(LeftoverText::statusText({ItemState::RestoreSkipped, Reason::Changed, 0}, disable),
              QStringLiteral("Not restored: its state has changed since"));
    EXPECT_EQ(LeftoverText::statusText({ItemState::Restored, Reason::None, 0}, service),
              QStringLiteral("Restored"));

    EXPECT_EQ(LeftoverText::locationText(disable),
              QStringLiteral(
                  "Disable Contoso at startup "
                  "(HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run)"));
    EXPECT_EQ(LeftoverText::locationText(task),
              QStringLiteral("Disable the scheduled task \\Contoso\\Updater"));
    EXPECT_EQ(
        LeftoverText::locationText(service),
        QStringLiteral("Change the start type of the service ContosoSvc from Automatic (delayed start) to "
                       "Manual"));
    EXPECT_EQ(LeftoverText::kindLabel(service, Category::Services), QStringLiteral("Service start type"));
}

TEST_F(LeftoverModelTest, EmptyScanHasNoRows) {
    load({});
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(model.itemCount(), 0);
    EXPECT_TRUE(model.plan(L"x").items.empty());
}

}  // namespace
}  // namespace avel::app
