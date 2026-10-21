// The GUI's leftover tree: grouping, containment, tri-state checks, presets and the plan it
// hands to the executor.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/changes/change_set.h"
#include "core/leftovers/scan.h"
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

TEST_F(LeftoverModelTest, EmptyScanHasNoRows) {
    load({});
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(model.itemCount(), 0);
    EXPECT_TRUE(model.plan(L"x").items.empty());
}

}  // namespace
}  // namespace avel::app
