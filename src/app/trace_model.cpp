#include "trace_model.h"

#include <QFont>
#include <QLocale>
#include <QStringList>

#include <algorithm>
#include <array>

#include "core/changes/change_set.h"
#include "leftover_text.h"
#include "program_text.h"
#include "trace_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using tracker::ChangeKind;
using tracker::NoiseReason;

namespace {

constexpr std::size_t kGroupCount = static_cast<std::size_t>(TraceModel::Group::Noise) + 1;
constexpr std::size_t kCategoryCount = static_cast<std::size_t>(leftovers::Category::Firewall) + 1;
// A created folder's or key's tooltip names this many of the items inside it.
constexpr std::size_t kInsideShown = 20;

}  // namespace

TraceModel::TraceModel(QObject* parent) : QAbstractItemModel(parent) {}

void TraceModel::setTrace(std::shared_ptr<const tracker::TraceResult> result,
                          std::vector<tracker::TiedItem> tied, tracker::TraceReview review,
                          const QString& program) {
    beginResetModel();
    result_ = std::move(result);
    tied_ = std::move(tied);
    std::sort(tied_.begin(), tied_.end(),
              [](const tracker::TiedItem& a, const tracker::TiedItem& b) { return a.change < b.change; });
    review_ = std::move(review);
    program_ = program;
    excluded_.clear();
    excludedValue_.clear();
    if (result_) {
        for (const auto& change : result_->changes) {
            excluded_.push_back(tracker::is_excluded(review_, change) ? 1 : 0);
        }
        for (const auto& change : result_->known_values) {
            excludedValue_.push_back(tracker::is_excluded(review_, change) ? 1 : 0);
        }
    }
    rebuild();
    endResetModel();
}

void TraceModel::setShowNoise(bool show) {
    if (show == showNoise_) {
        return;
    }
    beginResetModel();
    showNoise_ = show;
    rebuild();
    endResetModel();
}

int TraceModel::itemCount() const {
    int count = 0;
    for (const int root : roots_) {
        count += nodes_[static_cast<std::size_t>(root)].items;
    }
    return count;
}

int TraceModel::noiseCount() const {
    if (!result_) {
        return 0;
    }
    return static_cast<int>(
        std::count_if(result_->changes.begin(), result_->changes.end(),
                      [](const auto& change) { return change.noise != NoiseReason::None; }));
}

int TraceModel::checkableCount() const {
    int count = 0;
    for (const int root : roots_) {
        count += nodes_[static_cast<std::size_t>(root)].checkable;
    }
    return count;
}

int TraceModel::excludedCount() const {
    int count = 0;
    for (const int root : roots_) {
        const Node& node = nodes_[static_cast<std::size_t>(root)];
        count += node.checkable - node.checked;
    }
    return count;
}

QModelIndex TraceModel::groupIndex(Group group) const {
    for (const int root : roots_) {
        if (nodes_[static_cast<std::size_t>(root)].group == group) {
            return indexOf(root);
        }
    }
    return {};
}

void TraceModel::rebuild() {
    nodes_.clear();
    roots_.clear();
    if (!result_) {
        return;
    }
    std::array<int, kGroupCount> groupNodes;
    groupNodes.fill(-1);
    std::array<std::array<int, kCategoryCount>, kGroupCount> categoryNodes;
    for (auto& categories : categoryNodes) {
        categories.fill(-1);
    }
    const auto groupNode = [&](Group group) {
        int& node = groupNodes[static_cast<std::size_t>(group)];
        if (node < 0) {
            node = static_cast<int>(nodes_.size());
            Node created;
            created.kind = Node::Kind::Group;
            created.group = group;
            nodes_.push_back(std::move(created));
        }
        return node;
    };
    const auto categoryNode = [&](Group group, leftovers::Category category) {
        int& node = categoryNodes[static_cast<std::size_t>(group)][static_cast<std::size_t>(category)];
        if (node < 0) {
            const int parent = groupNode(group);
            node = static_cast<int>(nodes_.size());
            Node created;
            created.kind = Node::Kind::Category;
            created.group = group;
            created.category = category;
            created.parent = parent;
            nodes_.push_back(std::move(created));
        }
        return node;
    };
    const auto addItem = [&](Node::Kind kind, Group group, std::size_t index, int parent) {
        const int node = static_cast<int>(nodes_.size());
        Node created;
        created.kind = kind;
        created.group = group;
        created.index = index;
        created.parent = parent;
        nodes_.push_back(std::move(created));
        nodes_[static_cast<std::size_t>(parent)].children.push_back(node);
    };

    const auto& changes = result_->changes;
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const auto& change = changes[i];
        Group group = Group::Untied;
        if (change.noise != NoiseReason::None) {
            if (!showNoise_) {
                continue;
            }
            group = Group::Noise;
        } else if (change.change != ChangeKind::Created) {
            group = Group::Existing;
        } else if (const auto* tie = tieOf(i); tie && tie->tied) {
            group = Group::Tied;
        }
        addItem(Node::Kind::Change, group, i, categoryNode(group, leftovers::category_of(change.location)));
    }
    for (std::size_t i = 0; i < result_->known_values.size(); ++i) {
        addItem(Node::Kind::Value, Group::KnownValues, i, groupNode(Group::KnownValues));
    }

    // Groups in their enum's order, kinds in theirs, items in the trace's order.
    for (std::size_t group = 0; group < kGroupCount; ++group) {
        const int node = groupNodes[group];
        if (node < 0) {
            continue;
        }
        roots_.push_back(node);
        for (const int category : categoryNodes[group]) {
            if (category >= 0) {
                nodes_[static_cast<std::size_t>(node)].children.push_back(category);
            }
        }
    }
    for (std::size_t row = 0; row < roots_.size(); ++row) {
        nodes_[static_cast<std::size_t>(roots_[row])].row = static_cast<int>(row);
    }
    for (auto& node : nodes_) {
        for (std::size_t row = 0; row < node.children.size(); ++row) {
            nodes_[static_cast<std::size_t>(node.children[row])].row = static_cast<int>(row);
        }
    }
    for (const int root : roots_) {
        recount(root);
    }
}

void TraceModel::recount(int index) {
    Node& node = nodes_[static_cast<std::size_t>(index)];
    node.items = 0;
    node.checkable = 0;
    node.checked = 0;
    if (node.kind == Node::Kind::Change || node.kind == Node::Kind::Value) {
        node.items = 1;
        if (isCheckable(node)) {
            node.checkable = 1;
            node.checked = isChecked(node) ? 1 : 0;
        }
    }
    for (const int child : node.children) {
        recount(child);
        const Node& counted = nodes_[static_cast<std::size_t>(child)];
        node.items += counted.items;
        node.checkable += counted.checkable;
        node.checked += counted.checked;
    }
}

void TraceModel::setSubtree(int index, bool checked) {
    const Node& node = nodes_[static_cast<std::size_t>(index)];
    if (isCheckable(node)) {
        if (node.kind == Node::Kind::Change) {
            excluded_[node.index] = checked ? 0 : 1;
            tracker::set_excluded(review_, result_->changes[node.index], !checked);
        } else {
            excludedValue_[node.index] = checked ? 0 : 1;
            tracker::set_excluded(review_, result_->known_values[node.index], !checked);
        }
    }
    for (const int child : node.children) {
        setSubtree(child, checked);
    }
}

void TraceModel::notifySubtree(int index) {
    const Node& node = nodes_[static_cast<std::size_t>(index)];
    if (node.children.empty()) {
        return;
    }
    Q_EMIT dataChanged(indexOf(node.children.front()), indexOf(node.children.back()), {Qt::CheckStateRole});
    for (const int child : node.children) {
        notifySubtree(child);
    }
}

QModelIndex TraceModel::indexOf(int node, int column) const {
    return createIndex(nodes_[static_cast<std::size_t>(node)].row, column, static_cast<quintptr>(node));
}

bool TraceModel::isCheckable(const Node& node) const {
    if (node.kind == Node::Kind::Value) {
        return true;
    }
    return node.kind == Node::Kind::Change && (node.group == Group::Tied || node.group == Group::Untied) &&
           changes::is_removable(result_->changes[node.index].location);
}

bool TraceModel::isChecked(const Node& node) const {
    return node.kind == Node::Kind::Change ? !excluded_[node.index] : !excludedValue_[node.index];
}

const tracker::TiedItem* TraceModel::tieOf(std::size_t change) const {
    const auto found = std::lower_bound(
        tied_.begin(), tied_.end(), change,
        [](const tracker::TiedItem& item, std::size_t index) { return item.change < index; });
    return found != tied_.end() && found->change == change ? &*found : nullptr;
}

QString TraceModel::groupLabel(const Node& node) const {
    const QString count = QLocale().toString(node.items);
    if (node.kind == Node::Kind::Category) {
        return tr("%1 (%2)").arg(LeftoverText::categoryLabel(node.category), count);
    }
    switch (node.group) {
        case Group::Tied:
            return program_.isEmpty() ? tr("Tied to the program (%1)").arg(count)
                                      : tr("Tied to %1 (%2)").arg(program_, count);
        case Group::Untied:
            return tr("Not tied to the program (%1)").arg(count);
        case Group::KnownValues:
            return tr("Environment variables and shared DLLs (%1)").arg(count);
        case Group::Existing:
            return tr("Changes to items that existed before, shown only (%1)").arg(count);
        case Group::Noise:
            return tr("Noise: what Windows, the user or Avel changed (%1)").arg(count);
    }
    return {};
}

QVariant TraceModel::changeData(const Node& node, int column, int role) const {
    const auto& change = result_->changes[node.index];
    const auto category = leftovers::category_of(change.location);
    switch (column) {
        case ItemColumn:
            if (role == Qt::DisplayRole) {
                return LeftoverText::locationText(change.location);
            }
            if (role == Qt::ToolTipRole) {
                QString text = LeftoverText::locationText(change.location);
                if (change.inside && !change.inside->empty()) {
                    QStringList inside;
                    for (std::size_t i = 0; i < change.inside->size() && i < kInsideShown; ++i) {
                        const auto& item = (*change.inside)[i];
                        QString name = toQString(item.path);
                        if (item.value) {
                            const QString value =
                                item.value->empty() ? tr("(Default)") : toQString(*item.value);
                            name = name.isEmpty() ? value : name + u" : "_s + value;
                        }
                        inside << u"  "_s + name;
                    }
                    if (change.inside->size() > kInsideShown) {
                        inside << u"  "_s + tr("… and %n more", nullptr,
                                               static_cast<int>(change.inside->size() - kInsideShown));
                    }
                    text += u'\n' + tr("Created inside it:") + u'\n' + inside.join(u'\n');
                }
                return text;
            }
            break;
        case TypeColumn:
            if (role == Qt::DisplayRole) {
                return LeftoverText::kindLabel(change.location, category);
            }
            break;
        case ChangeColumn:
            if (role == Qt::DisplayRole) {
                QString text = TraceText::changeText(change.change);
                if (change.contents > 0) {
                    text = tr("%1, %n item(s) inside", nullptr, static_cast<int>(change.contents)).arg(text);
                }
                if (change.replaced) {
                    text = tr("%1, replaced by a new file").arg(text);
                }
                return text;
            }
            break;
        case DetailsColumn:
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
                QStringList details;
                if (change.noise != NoiseReason::None) {
                    details << TraceText::noiseText(change.noise);
                } else if (const auto* tie = tieOf(node.index); tie && tie->tied) {
                    details << TraceText::tieText(tie->reason);
                }
                if (!change.old_path.empty()) {
                    details << tr("Was %1").arg(toQString(change.old_path));
                }
                return details.join(u"; "_s);
            }
            break;
        default:
            break;
    }
    return {};
}

QVariant TraceModel::valueData(const Node& node, int column, int role) const {
    const auto& change = result_->known_values[node.index];
    switch (column) {
        case ItemColumn:
            if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
                return LeftoverText::locationText(leftovers::RegistryValueLocation{change.key, change.name});
            }
            break;
        case TypeColumn:
            if (role == Qt::DisplayRole) {
                return TraceText::spotText(change.spot);
            }
            break;
        case ChangeColumn:
            if (role == Qt::DisplayRole) {
                return TraceText::changeText(!change.before  ? ChangeKind::Created
                                             : !change.after ? ChangeKind::Deleted
                                                             : ChangeKind::Modified);
            }
            break;
        case DetailsColumn:
            if (role == Qt::DisplayRole) {
                return tr("%1 → %2").arg(TraceText::valueText(change.before),
                                         TraceText::valueText(change.after));
            }
            if (role == Qt::ToolTipRole) {
                return tr("Before: %1\nAfter: %2")
                    .arg(TraceText::valueText(change.before), TraceText::valueText(change.after));
            }
            break;
        default:
            break;
    }
    return {};
}

QModelIndex TraceModel::index(int row, int column, const QModelIndex& parent) const {
    if (row < 0 || column < 0 || column >= ColumnCount ||
        (parent.isValid() && parent.column() != ItemColumn)) {
        return {};
    }
    const auto& children = parent.isValid() ? nodes_[parent.internalId()].children : roots_;
    if (static_cast<std::size_t>(row) >= children.size()) {
        return {};
    }
    return createIndex(row, column, static_cast<quintptr>(children[static_cast<std::size_t>(row)]));
}

QModelIndex TraceModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return {};
    }
    const int parent = nodes_[child.internalId()].parent;
    return parent < 0 ? QModelIndex() : indexOf(parent);
}

int TraceModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid()) {
        return static_cast<int>(roots_.size());
    }
    return parent.column() == ItemColumn ? static_cast<int>(nodes_[parent.internalId()].children.size()) : 0;
}

int TraceModel::columnCount(const QModelIndex&) const { return ColumnCount; }

QVariant TraceModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const Node& node = nodes_[index.internalId()];
    const int column = index.column();
    if (role == ItemCountRole) {
        return node.items;
    }
    if (role == GroupRole) {
        return static_cast<int>(node.group);
    }
    if (role == Qt::CheckStateRole) {
        if (column != ItemColumn || node.checkable == 0) {
            return {};
        }
        const Qt::CheckState state = node.checked == 0                ? Qt::Unchecked
                                     : node.checked == node.checkable ? Qt::Checked
                                                                      : Qt::PartiallyChecked;
        return QVariant(state);
    }
    switch (node.kind) {
        case Node::Kind::Group:
        case Node::Kind::Category:
            if (column == ItemColumn && role == Qt::DisplayRole) {
                return groupLabel(node);
            }
            if (column == ItemColumn && role == Qt::FontRole && node.kind == Node::Kind::Group) {
                QFont font;
                font.setBold(true);
                return font;
            }
            return {};
        case Node::Kind::Change:
            if (role == ChangeRole) {
                return static_cast<qulonglong>(node.index);
            }
            return changeData(node, column, role);
        case Node::Kind::Value:
            if (role == KnownValueRole) {
                return static_cast<qulonglong>(node.index);
            }
            return valueData(node, column, role);
    }
    return {};
}

bool TraceModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || role != Qt::CheckStateRole || index.column() != ItemColumn) {
        return false;
    }
    const int nodeIndex = static_cast<int>(index.internalId());
    if (nodes_[static_cast<std::size_t>(nodeIndex)].checkable == 0) {
        return false;
    }
    const bool check = value.toInt() != Qt::Unchecked;
    setSubtree(nodeIndex, check);
    for (const int root : roots_) {
        recount(root);
    }
    for (int parent = nodes_[static_cast<std::size_t>(nodeIndex)].parent; parent >= 0;
         parent = nodes_[static_cast<std::size_t>(parent)].parent) {
        const QModelIndex changed = indexOf(parent);
        Q_EMIT dataChanged(changed, changed, {Qt::CheckStateRole});
    }
    Q_EMIT dataChanged(index, index, {Qt::CheckStateRole});
    notifySubtree(nodeIndex);
    Q_EMIT reviewChanged();
    return true;
}

Qt::ItemFlags TraceModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == ItemColumn && nodes_[index.internalId()].checkable > 0) {
        flags |= Qt::ItemIsUserCheckable;
    }
    return flags;
}

QVariant TraceModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
        case ItemColumn:
            return tr("Item");
        case TypeColumn:
            return tr("Type");
        case ChangeColumn:
            return tr("Change");
        case DetailsColumn:
            return tr("Why");
        default:
            return {};
    }
}

}  // namespace avel::app
