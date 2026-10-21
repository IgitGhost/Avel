#pragma once

#include <QAbstractItemModel>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/changes/change_set.h"
#include "core/leftovers/scan.h"

namespace avel::app {

// What a leftover scan found, as a tree to review: each program, its groups (files, registry,
// services, ...) and their items. An item sits under the item that contains it, whatever its
// group, because removing a folder or key takes everything in it along. So checking an item
// checks everything below it, and unchecking one unchecks the items it is inside of.
class LeftoverModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Column { ItemColumn, TypeColumn, ConfidenceColumn, ReasonColumn, ColumnCount };

    enum Role {
        ItemCountRole = Qt::UserRole + 1,  // int: the items in a row's subtree, the row's own included
        LeftoverRole,                      // qulonglong: an item row's index in the scan; none for groups
    };

    // Which items start out checked.
    enum class Preset : std::uint8_t {
        Safe,      // Very likely ones only
        Moderate,  // Likely ones too
        Advanced,  // Uncertain ones too
    };

    // The groups under a program: the scan's categories, with COM registrations, of which a
    // program can leave thousands, apart from the rest of the registry.
    enum class Group : std::uint8_t {
        Files,
        Shortcuts,
        Registry,
        ComRegistrations,
        Services,
        Tasks,
        Packages,
        Firewall,
    };

    struct Program {
        std::wstring id;
        QString name;
    };

    explicit LeftoverModel(QObject* parent = nullptr);

    // `programs` names the items' owners, in the order they're listed.
    void setScan(std::shared_ptr<const leftovers::ScanResult> scan, std::vector<Program> programs,
                 Preset preset);
    // Checks what the preset wants and unchecks the rest.
    void applyPreset(Preset preset);
    // Shows or hides the unlikely items. Hidden items are never removed.
    void setShowUnlikely(bool show);
    bool showUnlikely() const { return showUnlikely_; }

    int itemCount() const;  // Items shown
    int checkedCount() const;
    int hiddenCount() const;  // Unlikely items not shown
    int checkedPackageCount() const;
    void uncheckPackages();

    // The checked items, in scan order.
    changes::ChangePlan plan(std::wstring description) const;

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

Q_SIGNALS:
    void checkedCountChanged();

private:
    struct Node {
        enum class Kind : std::uint8_t { Program, Group, Item };
        Kind kind = Kind::Item;
        Group group = Group::Files;
        int parent = -1;
        int row = 0;
        std::size_t index = 0;  // Program: into programs_; Item: into the scan's leftovers
        std::vector<int> children;
        // Over the subtree, this node included.
        int items = 0;
        int checkable = 0;
        int checked = 0;
    };

    void rebuild();
    // A checked item takes what's inside it along, so whatever is shown inside it is checked too.
    void normalize();
    void setSubtree(int node, bool checked);
    void recount();
    void recount(int node);
    void notifySubtree(int node);
    void notifyAll();
    QModelIndex indexOf(int node, int column = ItemColumn) const;
    Qt::CheckState checkState(const Node& node) const;
    bool removable(std::size_t leftover) const;
    QString itemText(const Node& node) const;
    static QString groupLabel(Group group);
    QString ownerNames(const leftovers::Leftover& leftover) const;

    std::shared_ptr<const leftovers::ScanResult> scan_;
    std::vector<Program> programs_;
    std::vector<leftovers::Confidence> levels_;  // Per scan item
    std::vector<std::uint8_t> checked_;          // Per scan item: its own check
    std::vector<int> nodeOf_;                    // Per scan item: its node, or -1 while hidden
    std::vector<Node> nodes_;
    std::vector<int> roots_;
    bool showUnlikely_ = false;
};

}  // namespace avel::app
