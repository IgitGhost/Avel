// The GUI's trace review tree: its groups (tied, untied, known values, existing items, noise), what
// can be checked, the hidden noise, and the exclusions it hands back as the trace's review.

#include <gtest/gtest.h>

#include <windows.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/tracker/trace_review.h"
#include "trace_model.h"

namespace avel::app {
namespace {

using leftovers::FileLocation;
using leftovers::RegistryKeyLocation;
using leftovers::RegistryValueLocation;
using platform::Hive;
using tracker::ChangeKind;
using tracker::NoiseReason;
using tracker::TieReason;
using Model = TraceModel;
using Group = TraceModel::Group;

tracker::TraceChange change(leftovers::Location location, ChangeKind kind = ChangeKind::Created,
                            NoiseReason noise = NoiseReason::None) {
    tracker::TraceChange result;
    result.change = kind;
    result.location = std::move(location);
    result.noise = noise;
    return result;
}

platform::RawValue path_value(std::wstring_view text) {
    platform::RawValue value{L"Path", REG_EXPAND_SZ, {}};
    value.data.resize((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(value.data.data(), text.data(), text.size() * sizeof(wchar_t));
    return value;
}

platform::RegistryAddress machine(std::wstring path) { return {Hive::Machine, L"", std::move(path)}; }

// A trace of Contoso's install: its folder and Uninstall key (tied), a file elsewhere and a firewall
// rule (untied), a changed value of an existing key, a renamed file, two items of noise, and an
// entry added to the machine's Path.
std::shared_ptr<tracker::TraceResult> contoso_trace() {
    auto result = std::make_shared<tracker::TraceResult>();
    result->changes = {
        change(FileLocation{LR"(C:\Program Files\Contoso)", true}),                                     // 0
        change(FileLocation{LR"(C:\ProgramData\Elsewhere\x.bin)", false}),                              // 1
        change(RegistryValueLocation{machine(LR"(SOFTWARE\Shared)"), L"Theme"}, ChangeKind::Modified),  // 2
        change(FileLocation{LR"(C:\Windows\Prefetch\SETUP.pf)", false}, ChangeKind::Created,
               NoiseReason::SystemActivity),  // 3
        change(RegistryKeyLocation{
            machine(LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Contoso)")}),     // 4
        change(leftovers::FirewallRuleLocation{L"{RULE-1}"}),                                 // 5
        change(FileLocation{LR"(C:\Users\Me\Desktop\new.txt)", false}, ChangeKind::Renamed),  // 6
        change(FileLocation{LR"(C:\Users\Me\AppData\Local\Temp\x.tmp)", false}, ChangeKind::Created,
               NoiseReason::Temporary),  // 7
    };
    result->changes[0].contents = 2;
    result->changes[0].inside = std::vector<leftovers::ContainedItem>{
        {L"bin", std::nullopt, true}, {LR"(bin\app.exe)", std::nullopt, false}};
    result->changes[6].old_path = LR"(C:\Users\Me\Desktop\old.txt)";
    result->known_values = {{tracker::KnownSpot::MachineEnvironment,
                             machine(LR"(SYSTEM\CurrentControlSet\Control\Session Manager\Environment)"),
                             L"Path", path_value(L"C:\\Windows"),
                             path_value(L"C:\\Windows;C:\\Program Files\\Contoso\\bin")}};
    return result;
}

// The tying of contoso_trace's changes that aren't noise.
std::vector<tracker::TiedItem> contoso_ties() {
    return {{0, true, TieReason::InstallFolder, 0, std::nullopt},
            {1, false, TieReason::None, 0, std::nullopt},
            {2, false, TieReason::None, 0, std::nullopt},
            {4, true, TieReason::UninstallKey, 0, std::nullopt},
            {5, false, TieReason::None, 0, std::nullopt},
            {6, false, TieReason::None, 0, std::nullopt}};
}

class TraceModelTest : public ::testing::Test {
protected:
    void load(tracker::TraceReview review = {}) {
        result_ = contoso_trace();
        model.setTrace(result_, contoso_ties(), std::move(review), QStringLiteral("Contoso"));
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

    static Qt::CheckState state(const QModelIndex& index) {
        return static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());
    }

    static bool checkable(const QModelIndex& index) {
        return index.flags().testFlag(Qt::ItemIsUserCheckable) && index.data(Qt::CheckStateRole).isValid();
    }

    void set(const QModelIndex& index, Qt::CheckState state) {
        ASSERT_TRUE(model.setData(index, state, Qt::CheckStateRole));
    }

    std::shared_ptr<tracker::TraceResult> result_;
    Model model;
};

TEST_F(TraceModelTest, GroupsTiedUntiedValuesAndExistingItemsByKind) {
    load();
    // Noise is hidden; the groups come in a fixed order.
    ASSERT_EQ(model.rowCount(), 4);
    EXPECT_EQ(model.index(0, 0).data().toString(), QStringLiteral("Tied to Contoso (2)"));
    EXPECT_EQ(model.index(1, 0).data().toString(), QStringLiteral("Not tied to the program (2)"));
    EXPECT_EQ(model.index(2, 0).data().toString(),
              QStringLiteral("Environment variables and shared DLLs (1)"));
    EXPECT_EQ(model.index(3, 0).data().toString(),
              QStringLiteral("Changes to items that existed before, shown only (2)"));
    EXPECT_FALSE(model.groupIndex(Group::Noise).isValid());
    EXPECT_EQ(model.itemCount(), 7);
    EXPECT_EQ(model.noiseCount(), 2);

    // Under each group, its items by kind.
    const QModelIndex tied = model.groupIndex(Group::Tied);
    ASSERT_EQ(model.rowCount(tied), 2);
    EXPECT_EQ(model.index(0, 0, tied).data().toString(), QStringLiteral("Files and folders (1)"));
    EXPECT_EQ(model.index(1, 0, tied).data().toString(), QStringLiteral("Registry (1)"));
    const QModelIndex folder = model.index(0, 0, model.index(0, 0, tied));
    EXPECT_EQ(folder.data().toString(), QStringLiteral(R"(C:\Program Files\Contoso)"));
    EXPECT_EQ(folder.data(Model::ChangeRole).toULongLong(), 0u);
    EXPECT_EQ(folder.siblingAtColumn(Model::TypeColumn).data().toString(), QStringLiteral("Folder"));
    EXPECT_EQ(folder.siblingAtColumn(Model::ChangeColumn).data().toString(),
              QStringLiteral("Created, 2 item(s) inside"));
    EXPECT_EQ(folder.siblingAtColumn(Model::DetailsColumn).data().toString(),
              QStringLiteral("In the program's install folder"));
    EXPECT_TRUE(folder.data(Qt::ToolTipRole).toString().contains(QStringLiteral(R"(bin\app.exe)")));

    const QModelIndex untied = model.groupIndex(Group::Untied);
    ASSERT_EQ(model.rowCount(untied), 2);
    EXPECT_EQ(model.index(1, 0, untied).data().toString(), QStringLiteral("Firewall rules (shown only) (1)"));

    const QModelIndex values = model.groupIndex(Group::KnownValues);
    ASSERT_EQ(model.rowCount(values), 1);
    const QModelIndex path = model.index(0, 0, values);
    EXPECT_TRUE(path.data().toString().endsWith(QStringLiteral(R"(Session Manager\Environment : Path)")));
    EXPECT_EQ(path.data(Model::KnownValueRole).toULongLong(), 0u);
    EXPECT_EQ(path.siblingAtColumn(Model::ChangeColumn).data().toString(), QStringLiteral("Modified"));
    EXPECT_EQ(path.siblingAtColumn(Model::DetailsColumn).data().toString(),
              QStringLiteral(R"(C:\Windows → C:\Windows;C:\Program Files\Contoso\bin)"));

    const QModelIndex renamed = find(QStringLiteral(R"(C:\Users\Me\Desktop\new.txt)"));
    ASSERT_TRUE(renamed.isValid());
    EXPECT_EQ(renamed.data(Model::GroupRole).toInt(), static_cast<int>(Group::Existing));
    EXPECT_EQ(renamed.siblingAtColumn(Model::DetailsColumn).data().toString(),
              QStringLiteral(R"(Was C:\Users\Me\Desktop\old.txt)"));
}

TEST_F(TraceModelTest, OnlyWhatRemovalCouldOfferIsCheckable) {
    load();
    // Created items and value changes start checked: nothing is excluded.
    EXPECT_EQ(model.checkableCount(), 4);
    EXPECT_EQ(model.excludedCount(), 0);
    EXPECT_EQ(state(model.groupIndex(Group::Tied)), Qt::Checked);
    EXPECT_TRUE(checkable(find(QStringLiteral(R"(C:\Program Files\Contoso)"))));
    EXPECT_TRUE(checkable(find(QStringLiteral(R"(C:\ProgramData\Elsewhere\x.bin)"))));
    EXPECT_TRUE(checkable(model.index(0, 0, model.groupIndex(Group::KnownValues))));

    // Changes to existing items, and a firewall rule (never removed), are shown only.
    const QModelIndex existing = model.groupIndex(Group::Existing);
    EXPECT_FALSE(checkable(existing));
    EXPECT_FALSE(checkable(find(QStringLiteral(R"(C:\Users\Me\Desktop\new.txt)"))));
    const QModelIndex rule = find(QStringLiteral("{RULE-1}"));
    ASSERT_TRUE(rule.isValid());
    EXPECT_FALSE(checkable(rule));
    EXPECT_FALSE(model.setData(rule, Qt::Unchecked, Qt::CheckStateRole));
    EXPECT_FALSE(model.setData(existing, Qt::Unchecked, Qt::CheckStateRole));
    EXPECT_TRUE(model.review().empty());
}

TEST_F(TraceModelTest, NoiseIsHiddenUntilAskedForAndNeverCheckable) {
    load();
    EXPECT_FALSE(find(QStringLiteral(R"(C:\Windows\Prefetch\SETUP.pf)")).isValid());

    model.setShowNoise(true);
    const QModelIndex noise = model.groupIndex(Group::Noise);
    ASSERT_TRUE(noise.isValid());
    EXPECT_EQ(noise.row(), model.rowCount() - 1);
    EXPECT_EQ(noise.data().toString(), QStringLiteral("Noise: what Windows, the user or Avel changed (2)"));
    EXPECT_EQ(model.itemCount(), 9);
    const QModelIndex prefetch = find(QStringLiteral(R"(C:\Windows\Prefetch\SETUP.pf)"));
    ASSERT_TRUE(prefetch.isValid());
    EXPECT_FALSE(checkable(prefetch));
    EXPECT_FALSE(checkable(noise));
    // With its reason.
    EXPECT_EQ(prefetch.siblingAtColumn(Model::DetailsColumn).data().toString(),
              QStringLiteral("Windows' own activity (logs, caches, updates, indexing)"));
    EXPECT_EQ(model.checkableCount(), 4);

    model.setShowNoise(false);
    EXPECT_FALSE(model.groupIndex(Group::Noise).isValid());
    EXPECT_EQ(model.itemCount(), 7);
}

TEST_F(TraceModelTest, UncheckingExcludesAndTheReviewRoundTrips) {
    load();
    int changed = 0;
    QObject::connect(&model, &Model::reviewChanged, [&changed] { ++changed; });

    const QModelIndex file = find(QStringLiteral(R"(C:\ProgramData\Elsewhere\x.bin)"));
    set(file, Qt::Unchecked);
    set(model.index(0, 0, model.groupIndex(Group::KnownValues)), Qt::Unchecked);
    EXPECT_EQ(changed, 2);
    EXPECT_EQ(model.excludedCount(), 2);
    EXPECT_EQ(state(model.groupIndex(Group::Untied)), Qt::Unchecked);  // The firewall rule doesn't count
    EXPECT_TRUE(tracker::is_excluded(model.review(), result_->changes[1]));
    EXPECT_TRUE(tracker::is_excluded(model.review(), result_->known_values[0]));
    EXPECT_FALSE(tracker::is_excluded(model.review(), result_->changes[0]));

    // Saved and read back, the same items start unchecked.
    tracker::TraceReview saved;
    ASSERT_TRUE(tracker::from_json(changes::Json::parse(tracker::to_json(model.review()).dump()), saved));
    Model reopened;
    reopened.setTrace(result_, contoso_ties(), saved, QStringLiteral("Contoso"));
    EXPECT_EQ(reopened.excludedCount(), 2);
    EXPECT_EQ(state(reopened.groupIndex(Group::Tied)), Qt::Checked);
    EXPECT_EQ(state(reopened.groupIndex(Group::Untied)), Qt::Unchecked);
    EXPECT_EQ(state(reopened.groupIndex(Group::KnownValues)), Qt::Unchecked);

    // Unchecking a group excludes all of it; checking it again takes everything back in.
    set(model.groupIndex(Group::Tied), Qt::Unchecked);
    EXPECT_EQ(model.excludedCount(), 4);
    EXPECT_EQ(model.review().excluded.size(), 3u);
    for (const Group group : {Group::Tied, Group::Untied, Group::KnownValues}) {
        set(model.groupIndex(group), Qt::Checked);
    }
    EXPECT_EQ(model.excludedCount(), 0);
    EXPECT_TRUE(model.review().empty());
}

TEST_F(TraceModelTest, KeepsExclusionsOfItemsItDoesntShow) {
    // An exclusion the trace has no item for (a review written for another version of the trace)
    // stays in the review.
    tracker::TraceReview review;
    review.excluded = {FileLocation{LR"(C:\Gone)", true}};
    load(review);
    EXPECT_EQ(model.excludedCount(), 0);
    set(find(QStringLiteral(R"(C:\Program Files\Contoso)")), Qt::Unchecked);
    EXPECT_EQ(model.review().excluded.size(), 2u);
}

TEST_F(TraceModelTest, EmptyTraceHasNoRows) {
    model.setTrace(std::make_shared<tracker::TraceResult>(), {}, {});
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_EQ(model.itemCount(), 0);
    model.setTrace(nullptr, {}, {});
    EXPECT_EQ(model.rowCount(), 0);
}

}  // namespace
}  // namespace avel::app
