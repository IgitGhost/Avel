#include "startup_model.h"

#include <QGuiApplication>
#include <QPalette>

#include "core/platform/text.h"
#include "program_text.h"
#include "startup_text.h"

namespace avel::app {

using tools::StartupEntry;
using tools::StartupProtection;

StartupTableModel::StartupTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void StartupTableModel::setListing(std::shared_ptr<const StartupListing> listing) {
    beginResetModel();
    listing_ = std::move(listing);
    programNames_.clear();
    if (listing_ && listing_->inventory) {
        for (const auto& program : listing_->inventory->programs) {
            programNames_.insert(toQString(program.id), toQString(program.display_name));
        }
    }
    endResetModel();
}

const StartupEntry* StartupTableModel::entryAt(int row) const {
    if (!listing_ || row < 0 || row >= static_cast<int>(listing_->entries.size())) {
        return nullptr;
    }
    return &listing_->entries[static_cast<std::size_t>(row)];
}

int StartupTableModel::rowOf(const std::wstring& id) const {
    if (!listing_) {
        return -1;
    }
    for (std::size_t row = 0; row < listing_->entries.size(); ++row) {
        if (platform::iequals(listing_->entries[row].id, id)) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

QString StartupTableModel::programName(const std::wstring& programId) const {
    return programId.empty() ? QString() : programNames_.value(toQString(programId));
}

void StartupTableModel::retranslate() {
    Q_EMIT headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);
    if (rowCount() > 0) {
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
    }
}

int StartupTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() || !listing_ ? 0 : static_cast<int>(listing_->entries.size());
}

int StartupTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant StartupTableModel::data(const QModelIndex& index, int role) const {
    const StartupEntry* entry = index.isValid() ? entryAt(index.row()) : nullptr;
    if (entry == nullptr) {
        return {};
    }
    const bool windowsOwn = entry->protection == StartupProtection::WindowsOwn;
    switch (role) {
        case Qt::DisplayRole:
        case SortRole:
            switch (index.column()) {
                case NameColumn:
                    return StartupText::nameText(*entry);
                case StateColumn:
                    return StartupText::stateText(*entry);
                case SourceColumn:
                    return StartupText::sourceText(*entry);
                case UserColumn:
                    return StartupText::userText(*entry);
                case PublisherColumn:
                    return StartupText::publisherText(*entry);
                case CommandColumn:
                    return toQString(entry->command);
            }
            break;
        case Qt::ToolTipRole:
            if (windowsOwn) {
                return StartupText::issueText(tools::StartupPlanIssueCode::Protected,
                                              tools::StartupAction::Delete);
            }
            if (index.column() == CommandColumn) {
                return toQString(entry->command);
            }
            if (index.column() == StateColumn) {
                return StartupText::stateDetails(*entry);
            }
            break;
        case Qt::ForegroundRole:
            // Windows' own entries: shown, never offered for a change.
            if (windowsOwn) {
                return QGuiApplication::palette().brush(QPalette::Disabled, QPalette::Text);
            }
            break;
        default:
            break;
    }
    return {};
}

QVariant StartupTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
        case NameColumn:
            return tr("Name");
        case StateColumn:
            return tr("State");
        case SourceColumn:
            return tr("Source");
        case UserColumn:
            return tr("User");
        case PublisherColumn:
            return tr("Publisher");
        case CommandColumn:
            return tr("Command");
    }
    return {};
}

StartupFilterModel::StartupFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    setSortRole(StartupTableModel::SortRole);
    setSortLocaleAware(true);
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

void StartupFilterModel::setSourceStartupModel(StartupTableModel* model) {
    entries_ = model;
    setSourceModel(model);
}

void StartupFilterModel::setSearchText(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed == searchText_) {
        return;
    }
    beginFilterChange();
    searchText_ = trimmed;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void StartupFilterModel::setKind(std::optional<tools::StartupKind> kind) {
    if (kind == kind_) {
        return;
    }
    beginFilterChange();
    kind_ = kind;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void StartupFilterModel::setUser(std::optional<std::wstring> sid) {
    if (sid == user_) {
        return;
    }
    beginFilterChange();
    user_ = std::move(sid);
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool StartupFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    const StartupEntry* entry =
        entries_ != nullptr && !sourceParent.isValid() ? entries_->entryAt(sourceRow) : nullptr;
    if (entry == nullptr) {
        return false;
    }
    if (kind_ && entry->kind != *kind_) {
        return false;
    }
    if (user_ && !platform::iequals(entry->user_sid, *user_)) {
        return false;
    }
    if (searchText_.isEmpty()) {
        return true;
    }
    for (const QString& text :
         {StartupText::nameText(*entry), toQString(entry->name), toQString(entry->command),
          StartupText::publisherText(*entry), toQString(entry->target),
          toQString(entry->product.product_name), entries_->programName(entry->program_id)}) {
        if (text.contains(searchText_, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

}  // namespace avel::app
