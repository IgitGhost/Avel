#include "leftover_model.h"

#include <QFont>
#include <QLocale>
#include <QStringList>

#include <algorithm>
#include <array>

#include "leftover_text.h"
#include "program_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using leftovers::Confidence;

namespace {

bool unlikely(Confidence confidence) { return confidence <= Confidence::Bad; }

Confidence threshold(LeftoverModel::Preset preset) {
    switch (preset) {
        case LeftoverModel::Preset::Safe:
            return Confidence::VeryGood;
        case LeftoverModel::Preset::Moderate:
            return Confidence::Good;
        case LeftoverModel::Preset::Advanced:
            return Confidence::Questionable;
    }
    return Confidence::Good;
}

LeftoverModel::Group groupOf(const leftovers::Leftover& leftover) {
    using leftovers::Category;
    using Group = LeftoverModel::Group;
    switch (leftover.category) {
        case Category::Files:
            return Group::Files;
        case Category::Shortcuts:
            return Group::Shortcuts;
        case Category::Registry:
            if (leftover.found_by == leftovers::Scanner::ComClasses) {
                return Group::ComRegistrations;
            }
            return leftover.found_by == leftovers::Scanner::MsiRegistration ? Group::InstallerRegistration
                                                                            : Group::Registry;
        case Category::Services:
            return Group::Services;
        case Category::Tasks:
            return Group::Tasks;
        case Category::Packages:
            return Group::Packages;
        case Category::Firewall:
            return Group::Firewall;
    }
    return Group::Files;
}

constexpr std::size_t kGroupCount = static_cast<std::size_t>(LeftoverModel::Group::Firewall) + 1;

}  // namespace

LeftoverModel::LeftoverModel(QObject* parent) : QAbstractItemModel(parent) {}

void LeftoverModel::setScan(std::shared_ptr<const leftovers::ScanResult> scan, std::vector<Program> programs,
                            Preset preset) {
    beginResetModel();
    scan_ = std::move(scan);
    programs_ = std::move(programs);
    levels_.clear();
    checked_.clear();
    if (scan_) {
        const auto& items = scan_->leftovers;
        levels_.reserve(items.size());
        for (const auto& leftover : items) {
            levels_.push_back(leftovers::level(leftover.records));
            // Owners nobody named still get a group of their own.
            for (const auto& id : leftover.owner_ids) {
                if (std::none_of(programs_.begin(), programs_.end(),
                                 [&](const Program& p) { return p.id == id; })) {
                    programs_.push_back({id, toQString(id)});
                }
            }
        }
        if (programs_.empty() && !items.empty()) {
            programs_.push_back({std::wstring(), tr("Other programs")});
        }
        checked_.resize(items.size(), 0);
        const Confidence minimum = threshold(preset);
        for (std::size_t i = 0; i < items.size(); ++i) {
            checked_[i] = levels_[i] >= minimum ? 1 : 0;
        }
    }
    rebuild();
    normalize();
    recount();
    endResetModel();
    Q_EMIT checkedCountChanged();
}

void LeftoverModel::applyPreset(Preset preset) {
    const Confidence minimum = threshold(preset);
    for (std::size_t i = 0; i < checked_.size(); ++i) {
        checked_[i] = levels_[i] >= minimum ? 1 : 0;
    }
    normalize();
    recount();
    notifyAll();
    Q_EMIT checkedCountChanged();
}

void LeftoverModel::setShowUnlikely(bool show) {
    if (show == showUnlikely_) {
        return;
    }
    beginResetModel();
    showUnlikely_ = show;
    rebuild();
    normalize();
    recount();
    endResetModel();
    Q_EMIT checkedCountChanged();
}

int LeftoverModel::itemCount() const {
    int count = 0;
    for (const int root : roots_) {
        count += nodes_[static_cast<std::size_t>(root)].items;
    }
    return count;
}

int LeftoverModel::checkedCount() const {
    int count = 0;
    for (const int root : roots_) {
        count += nodes_[static_cast<std::size_t>(root)].checked;
    }
    return count;
}

int LeftoverModel::hiddenCount() const {
    return static_cast<int>(std::count(nodeOf_.begin(), nodeOf_.end(), -1));
}

int LeftoverModel::checkedPackageCount() const {
    int count = 0;
    for (std::size_t i = 0; i < nodeOf_.size(); ++i) {
        count += nodeOf_[i] >= 0 && checked_[i] &&
                 std::holds_alternative<leftovers::PackageLocation>(scan_->leftovers[i].location);
    }
    return count;
}

void LeftoverModel::uncheckPackages() {
    for (std::size_t i = 0; i < nodeOf_.size(); ++i) {
        if (nodeOf_[i] < 0 ||
            !std::holds_alternative<leftovers::PackageLocation>(scan_->leftovers[i].location)) {
            continue;
        }
        checked_[i] = 0;
        for (int parent = nodes_[static_cast<std::size_t>(nodeOf_[i])].parent;
             parent >= 0 && nodes_[static_cast<std::size_t>(parent)].kind == Node::Kind::Item;
             parent = nodes_[static_cast<std::size_t>(parent)].parent) {
            checked_[nodes_[static_cast<std::size_t>(parent)].index] = 0;
        }
    }
    recount();
    notifyAll();
    Q_EMIT checkedCountChanged();
}

changes::ChangePlan LeftoverModel::plan(std::wstring description) const {
    changes::ChangePlan plan;
    plan.description = std::move(description);
    for (std::size_t i = 0; i < nodeOf_.size(); ++i) {
        if (nodeOf_[i] < 0 || !checked_[i] || !removable(i)) {
            continue;
        }
        const auto& leftover = scan_->leftovers[i];
        std::vector<std::wstring> names;
        for (const auto& id : leftover.owner_ids) {
            const auto program = std::find_if(programs_.begin(), programs_.end(),
                                              [&](const Program& p) { return p.id == id; });
            names.push_back(program != programs_.end() ? program->name.toStdWString() : id);
        }
        plan.items.push_back(changes::make_planned_item(leftover, std::move(names)));
    }
    return plan;
}

void LeftoverModel::rebuild() {
    nodes_.clear();
    roots_.clear();
    nodeOf_.assign(scan_ ? scan_->leftovers.size() : 0, -1);
    if (!scan_) {
        return;
    }
    const auto& items = scan_->leftovers;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (showUnlikely_ || !unlikely(levels_[i])) {
            nodeOf_[i] = static_cast<int>(nodes_.size());
            Node node;
            node.index = i;
            nodes_.push_back(std::move(node));
        }
    }

    std::vector<int> programNodes(programs_.size(), -1);
    std::vector<std::array<int, kGroupCount>> groupNodes(programs_.size());
    for (auto& groups : groupNodes) {
        groups.fill(-1);
    }
    const auto groupNode = [&](std::size_t program, Group group) {
        int& node = groupNodes[program][static_cast<std::size_t>(group)];
        if (node < 0) {
            if (programNodes[program] < 0) {
                programNodes[program] = static_cast<int>(nodes_.size());
                Node programNode;
                programNode.kind = Node::Kind::Program;
                programNode.index = program;
                nodes_.push_back(std::move(programNode));
            }
            node = static_cast<int>(nodes_.size());
            Node groupNode;
            groupNode.kind = Node::Kind::Group;
            groupNode.group = group;
            groupNode.index = program;
            groupNode.parent = programNodes[program];
            nodes_.push_back(std::move(groupNode));
        }
        return node;
    };
    // The first listed program that left the item.
    const auto programOf = [&](const leftovers::Leftover& leftover) {
        for (std::size_t p = 0; p < programs_.size(); ++p) {
            if (std::find(leftover.owner_ids.begin(), leftover.owner_ids.end(), programs_[p].id) !=
                leftover.owner_ids.end()) {
                return p;
            }
        }
        return std::size_t{0};
    };
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (nodeOf_[i] < 0) {
            continue;
        }
        // The nearest container that is shown, if any.
        auto container = items[i].inside;
        for (std::size_t steps = 0; container && nodeOf_[*container] < 0 && steps < items.size(); ++steps) {
            container = items[*container].inside;
        }
        const int parent = container && nodeOf_[*container] >= 0
                               ? nodeOf_[*container]
                               : groupNode(programOf(items[i]), groupOf(items[i]));
        nodes_[static_cast<std::size_t>(nodeOf_[i])].parent = parent;
        nodes_[static_cast<std::size_t>(parent)].children.push_back(nodeOf_[i]);
    }

    // Programs in the order given, groups in their enum's order, items in scan order.
    for (const int program : programNodes) {
        if (program >= 0) {
            roots_.push_back(program);
        }
    }
    for (std::size_t p = 0; p < programNodes.size(); ++p) {
        if (programNodes[p] < 0) {
            continue;
        }
        auto& children = nodes_[static_cast<std::size_t>(programNodes[p])].children;
        for (const int group : groupNodes[p]) {
            if (group >= 0) {
                children.push_back(group);
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
}

void LeftoverModel::normalize() {
    for (const auto& node : nodes_) {
        if (node.kind == Node::Kind::Item && checked_[node.index] && removable(node.index)) {
            for (const int child : node.children) {
                setSubtree(child, true);
            }
        }
    }
}

void LeftoverModel::setSubtree(int index, bool checked) {
    const Node& node = nodes_[static_cast<std::size_t>(index)];
    if (node.kind == Node::Kind::Item && removable(node.index)) {
        checked_[node.index] = checked ? 1 : 0;
    }
    for (const int child : node.children) {
        setSubtree(child, checked);
    }
}

void LeftoverModel::recount() {
    for (const int root : roots_) {
        recount(root);
    }
}

void LeftoverModel::recount(int index) {
    Node& node = nodes_[static_cast<std::size_t>(index)];
    node.items = 0;
    node.checkable = 0;
    node.checked = 0;
    if (node.kind == Node::Kind::Item) {
        node.items = 1;
        if (removable(node.index)) {
            node.checkable = 1;
            node.checked = checked_[node.index] ? 1 : 0;
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

void LeftoverModel::notifySubtree(int index) {
    const Node& node = nodes_[static_cast<std::size_t>(index)];
    if (node.children.empty()) {
        return;
    }
    Q_EMIT dataChanged(indexOf(node.children.front()), indexOf(node.children.back()), {Qt::CheckStateRole});
    for (const int child : node.children) {
        notifySubtree(child);
    }
}

void LeftoverModel::notifyAll() {
    if (roots_.empty()) {
        return;
    }
    Q_EMIT dataChanged(indexOf(roots_.front()), indexOf(roots_.back()), {Qt::CheckStateRole});
    for (const int root : roots_) {
        notifySubtree(root);
    }
}

QModelIndex LeftoverModel::indexOf(int node, int column) const {
    return createIndex(nodes_[static_cast<std::size_t>(node)].row, column, static_cast<quintptr>(node));
}

Qt::CheckState LeftoverModel::checkState(const Node& node) const {
    if (node.kind == Node::Kind::Item && removable(node.index) && checked_[node.index]) {
        return Qt::Checked;
    }
    if (node.checked == 0) {
        return Qt::Unchecked;
    }
    return node.checked == node.checkable ? Qt::Checked : Qt::PartiallyChecked;
}

bool LeftoverModel::removable(std::size_t leftover) const {
    return changes::is_removable(scan_->leftovers[leftover].location);
}

// An item inside another one shows only the part of its path below its container's.
QString LeftoverModel::itemText(const Node& node) const {
    const QString text = LeftoverText::locationText(scan_->leftovers[node.index].location);
    if (node.parent < 0) {
        return text;
    }
    const Node& parent = nodes_[static_cast<std::size_t>(node.parent)];
    if (parent.kind != Node::Kind::Item) {
        return text;
    }
    const QString outer = LeftoverText::locationText(scan_->leftovers[parent.index].location);
    for (const QString& separator : {u"\\"_s, u" : "_s}) {
        if (text.size() > outer.size() + separator.size() &&
            text.startsWith(outer + separator, Qt::CaseInsensitive)) {
            return text.mid(outer.size() + separator.size());
        }
    }
    return text;
}

QString LeftoverModel::groupLabel(Group group) {
    using leftovers::Category;
    switch (group) {
        case Group::Files:
            return LeftoverText::categoryLabel(Category::Files);
        case Group::Shortcuts:
            return LeftoverText::categoryLabel(Category::Shortcuts);
        case Group::Registry:
            return LeftoverText::categoryLabel(Category::Registry);
        case Group::ComRegistrations:
            return tr("COM classes and shell extensions");
        case Group::InstallerRegistration:
            return tr("Windows Installer registration");
        case Group::Services:
            return LeftoverText::categoryLabel(Category::Services);
        case Group::Tasks:
            return LeftoverText::categoryLabel(Category::Tasks);
        case Group::Packages:
            return LeftoverText::categoryLabel(Category::Packages);
        case Group::Firewall:
            return LeftoverText::categoryLabel(Category::Firewall);
    }
    return {};
}

QString LeftoverModel::ownerNames(const leftovers::Leftover& leftover) const {
    QStringList names;
    for (const auto& id : leftover.owner_ids) {
        const auto program =
            std::find_if(programs_.begin(), programs_.end(), [&](const Program& p) { return p.id == id; });
        names << (program != programs_.end() ? program->name : toQString(id));
    }
    return names.join(u", "_s);
}

QModelIndex LeftoverModel::index(int row, int column, const QModelIndex& parent) const {
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

QModelIndex LeftoverModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return {};
    }
    const int parent = nodes_[child.internalId()].parent;
    return parent < 0 ? QModelIndex() : indexOf(parent);
}

int LeftoverModel::rowCount(const QModelIndex& parent) const {
    if (!parent.isValid()) {
        return static_cast<int>(roots_.size());
    }
    return parent.column() == ItemColumn ? static_cast<int>(nodes_[parent.internalId()].children.size()) : 0;
}

int LeftoverModel::columnCount(const QModelIndex&) const { return ColumnCount; }

QVariant LeftoverModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const Node& node = nodes_[index.internalId()];
    const int column = index.column();
    if (role == ItemCountRole) {
        return node.items;
    }
    if (role == Qt::CheckStateRole) {
        return column == ItemColumn && node.checkable > 0 ? QVariant(checkState(node)) : QVariant();
    }

    if (node.kind == Node::Kind::Program) {
        if (column == ItemColumn && role == Qt::DisplayRole) {
            return programs_[node.index].name;
        }
        if (column == ItemColumn && role == Qt::FontRole) {
            QFont font;
            font.setBold(true);
            return font;
        }
        return {};
    }
    if (node.kind == Node::Kind::Group) {
        if (column == ItemColumn && role == Qt::DisplayRole) {
            return tr("%1 (%2)").arg(groupLabel(node.group), QLocale().toString(node.items));
        }
        return {};
    }

    const auto& leftover = scan_->leftovers[node.index];
    const Confidence level = levels_[node.index];
    if (role == LeftoverRole) {
        return static_cast<qulonglong>(node.index);
    }
    switch (column) {
        case ItemColumn:
            if (role == Qt::DisplayRole) {
                return itemText(node);
            }
            if (role == Qt::ToolTipRole) {
                return tr("%1\nLeft by %2")
                    .arg(LeftoverText::locationText(leftover.location), ownerNames(leftover));
            }
            break;
        case TypeColumn:
            if (role == Qt::DisplayRole) {
                return LeftoverText::kindLabel(leftover.location, leftover.category);
            }
            break;
        case ConfidenceColumn:
            if (role == Qt::DisplayRole) {
                return LeftoverText::confidenceLabel(level);
            }
            if (role == Qt::DecorationRole) {
                return LeftoverText::confidenceColor(level);
            }
            if (role == Qt::ToolTipRole) {
                return LeftoverText::evidenceDetails(leftover.records);
            }
            break;
        case ReasonColumn:
            if (role == Qt::DisplayRole) {
                return LeftoverText::evidenceSummary(leftover.records);
            }
            if (role == Qt::ToolTipRole) {
                return LeftoverText::evidenceDetails(leftover.records);
            }
            break;
        default:
            break;
    }
    return {};
}

bool LeftoverModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || role != Qt::CheckStateRole || index.column() != ItemColumn) {
        return false;
    }
    const int nodeIndex = static_cast<int>(index.internalId());
    if (nodes_[static_cast<std::size_t>(nodeIndex)].checkable == 0) {
        return false;
    }
    const bool check = value.toInt() != Qt::Unchecked;
    setSubtree(nodeIndex, check);
    // Its containers would take it along.
    int top = nodeIndex;
    for (int parent = nodes_[static_cast<std::size_t>(nodeIndex)].parent; parent >= 0;
         parent = nodes_[static_cast<std::size_t>(parent)].parent) {
        const Node& container = nodes_[static_cast<std::size_t>(parent)];
        if (!check && container.kind == Node::Kind::Item) {
            checked_[container.index] = 0;
        }
        top = parent;
    }
    recount();
    for (int node = nodeIndex; node != top; node = nodes_[static_cast<std::size_t>(node)].parent) {
        const QModelIndex changed = indexOf(nodes_[static_cast<std::size_t>(node)].parent);
        Q_EMIT dataChanged(changed, changed, {Qt::CheckStateRole});
    }
    Q_EMIT dataChanged(index, index, {Qt::CheckStateRole});
    notifySubtree(nodeIndex);
    Q_EMIT checkedCountChanged();
    return true;
}

Qt::ItemFlags LeftoverModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == ItemColumn && nodes_[index.internalId()].checkable > 0) {
        flags |= Qt::ItemIsUserCheckable;
    }
    return flags;
}

QVariant LeftoverModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
        case ItemColumn:
            return tr("Item");
        case TypeColumn:
            return tr("Type");
        case ConfidenceColumn:
            return tr("Confidence");
        case ReasonColumn:
            return tr("Why it matched");
        default:
            return {};
    }
}

}  // namespace avel::app
