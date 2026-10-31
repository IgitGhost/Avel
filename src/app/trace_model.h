#pragma once

#include <QAbstractItemModel>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/leftovers/leftover.h"
#include "core/tracker/trace_items.h"
#include "core/tracker/trace_record.h"
#include "core/tracker/trace_review.h"

namespace avel::app {

// What a finished trace recorded, as a tree to review: groups for the items tied to the program,
// the untied ones, the environment variables and shared DLL counts it changed, the changes to
// items that existed before (shown only) and the noise (hidden unless asked for, never checkable),
// each split by kind (files, registry, services, ...). A checked item is the program's: removing
// the program by its trace offers it. Unchecking one excludes it, and review() says so.
class TraceModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Column { ItemColumn, TypeColumn, ChangeColumn, DetailsColumn, ColumnCount };

    enum Role {
        ItemCountRole = Qt::UserRole + 1,  // int: the items in a row's subtree, the row's own included
        ChangeRole,                        // qulonglong: a change's index in TraceResult::changes
        KnownValueRole,                    // qulonglong: a known-value change's index in known_values
        GroupRole,                         // int (Group): the group a row is in
    };

    enum class Group : std::uint8_t {
        Tied,         // Created items tied to the program
        Untied,       // Created items nothing ties to it
        KnownValues,  // Environment variables and SharedDLLs counts it changed
        Existing,     // Items that existed before: modified, deleted or renamed (shown only)
        Noise,        // Set aside as Windows', the user's or Avel's (shown only, hidden by default)
    };

    explicit TraceModel(QObject* parent = nullptr);

    // `tied` is tie_trace_items' result for `result`; `program` names the group of tied items
    // (empty: "the program").
    void setTrace(std::shared_ptr<const tracker::TraceResult> result, std::vector<tracker::TiedItem> tied,
                  tracker::TraceReview review, const QString& program = {});
    void setShowNoise(bool show);
    bool showNoise() const { return showNoise_; }

    int itemCount() const;       // Items shown
    int noiseCount() const;      // Noise, shown or not
    int checkableCount() const;  // Items the review can exclude
    int excludedCount() const;   // Of those, the unchecked ones

    // The review as the checks say: the one given, with the items unchecked since added and those
    // checked again taken out.
    const tracker::TraceReview& review() const { return review_; }

    // The group's row; invalid while it is empty or hidden.
    QModelIndex groupIndex(Group group) const;

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

Q_SIGNALS:
    void reviewChanged();

private:
    struct Node {
        enum class Kind : std::uint8_t { Group, Category, Change, Value };
        Kind kind = Kind::Change;
        Group group = Group::Tied;
        leftovers::Category category = leftovers::Category::Files;
        int parent = -1;
        int row = 0;
        std::size_t index = 0;  // Change: into the changes; Value: into the known values
        std::vector<int> children;
        // Over the subtree, this node included.
        int items = 0;
        int checkable = 0;
        int checked = 0;
    };

    void rebuild();
    void recount(int node);
    void setSubtree(int node, bool checked);
    void notifySubtree(int node);
    QModelIndex indexOf(int node, int column = ItemColumn) const;
    bool isCheckable(const Node& node) const;
    bool isChecked(const Node& node) const;
    const tracker::TiedItem* tieOf(std::size_t change) const;
    QString groupLabel(const Node& node) const;
    QVariant changeData(const Node& node, int column, int role) const;
    QVariant valueData(const Node& node, int column, int role) const;

    std::shared_ptr<const tracker::TraceResult> result_;
    std::vector<tracker::TiedItem> tied_;  // By change index
    tracker::TraceReview review_;
    QString program_;
    std::vector<std::uint8_t> excluded_;       // Per change
    std::vector<std::uint8_t> excludedValue_;  // Per known value
    std::vector<Node> nodes_;
    std::vector<int> roots_;
    bool showNoise_ = false;
};

}  // namespace avel::app
