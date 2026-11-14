#include "extensions_model.h"

#include <QStringList>

#include "core/platform/text.h"
#include "extensions_text.h"
#include "program_text.h"

namespace avel::app {

using tools::BrowserExtension;

namespace {

// "1.2.10" sorts after "1.2.9": every part padded.
QString versionSortKey(const std::wstring& version) {
    QStringList parts = toQString(version).split(u'.');
    for (QString& part : parts) {
        part = part.rightJustified(10, u'0');
    }
    return parts.join(u'.');
}

}  // namespace

ExtensionsTableModel::ExtensionsTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void ExtensionsTableModel::setListing(std::shared_ptr<const ExtensionsListing> listing) {
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

const BrowserExtension* ExtensionsTableModel::entryAt(int row) const {
    if (!listing_ || row < 0 || row >= static_cast<int>(listing_->list.extensions.size())) {
        return nullptr;
    }
    return &listing_->list.extensions[static_cast<std::size_t>(row)];
}

int ExtensionsTableModel::rowOf(const std::wstring& id) const {
    if (!listing_) {
        return -1;
    }
    for (std::size_t row = 0; row < listing_->list.extensions.size(); ++row) {
        if (platform::iequals(listing_->list.extensions[row].id, id)) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

const BrowserExtension* ExtensionsTableModel::entryWithId(const std::wstring& id) const {
    return entryAt(rowOf(id));
}

QString ExtensionsTableModel::programName(const std::wstring& programId) const {
    return programId.empty() ? QString() : programNames_.value(toQString(programId));
}

QString ExtensionsTableModel::userText(const BrowserExtension& entry) const {
    return ExtensionsText::userText(entry.user_sid, entry.user_name,
                                    listing_ ? listing_->currentUserSid : std::wstring());
}

void ExtensionsTableModel::retranslate() {
    Q_EMIT headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);
    if (rowCount() > 0) {
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
    }
}

int ExtensionsTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() || !listing_ ? 0 : static_cast<int>(listing_->list.extensions.size());
}

int ExtensionsTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ExtensionsTableModel::data(const QModelIndex& index, int role) const {
    const BrowserExtension* entry = index.isValid() ? entryAt(index.row()) : nullptr;
    if (entry == nullptr) {
        return {};
    }
    switch (role) {
        case Qt::DisplayRole:
            switch (index.column()) {
                case NameColumn:
                    return ExtensionsText::nameText(*entry);
                case BrowserColumn:
                    return ExtensionsText::browserName(entry->browser);
                case ProfileColumn:
                    return ExtensionsText::profileText(*entry);
                case UserColumn:
                    return userText(*entry);
                case StateColumn:
                    return ExtensionsText::stateText(*entry);
                case InstallColumn:
                    return ExtensionsText::installText(entry->install);
                case StoreColumn:
                    return entry->from_store ? tr("Yes") : tr("No");
                case VersionColumn:
                    return toQString(entry->version);
                case RemovalColumn:
                    return ExtensionsText::removalText(entry->removal);
            }
            break;
        case SortRole:
            if (index.column() == VersionColumn) {
                return versionSortKey(entry->version);
            }
            return data(index, Qt::DisplayRole);
        case Qt::ToolTipRole:
            switch (index.column()) {
                case NameColumn:
                    return entry->description.empty() ? toQString(entry->extension_id)
                                                      : toQString(entry->description);
                case ProfileColumn:
                    return toQString(entry->profile_path);
                case StateColumn: {
                    const QString details = ExtensionsText::stateDetails(*entry);
                    return details.isEmpty() ? QVariant() : QVariant(details);
                }
                case InstallColumn:
                    return ExtensionsText::installDetails(entry->install);
                case StoreColumn: {
                    const QString store = ExtensionsText::storeText(entry->store);
                    return store.isEmpty() ? QVariant() : QVariant(store);
                }
                case RemovalColumn:
                    return ExtensionsText::removalDetails(entry->removal);
                default:
                    break;
            }
            break;
        default:
            break;
    }
    return {};
}

QVariant ExtensionsTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
        case NameColumn:
            return tr("Name");
        case BrowserColumn:
            return tr("Browser");
        case ProfileColumn:
            return tr("Profile");
        case UserColumn:
            return tr("User");
        case StateColumn:
            return tr("State");
        case InstallColumn:
            return tr("Installed by");
        case StoreColumn:
            return tr("From store");
        case VersionColumn:
            return tr("Version");
        case RemovalColumn:
            return tr("Removal");
    }
    return {};
}

ExtensionsFilterModel::ExtensionsFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    setSortRole(ExtensionsTableModel::SortRole);
    setSortLocaleAware(true);
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

void ExtensionsFilterModel::setSourceExtensionsModel(ExtensionsTableModel* model) {
    entries_ = model;
    setSourceModel(model);
}

void ExtensionsFilterModel::setSearchText(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed == searchText_) {
        return;
    }
    beginFilterChange();
    searchText_ = trimmed;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void ExtensionsFilterModel::setBrowser(std::optional<tools::BrowserKind> browser) {
    if (browser == browser_) {
        return;
    }
    beginFilterChange();
    browser_ = browser;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void ExtensionsFilterModel::setUser(std::optional<std::wstring> sid) {
    if (sid == user_) {
        return;
    }
    beginFilterChange();
    user_ = std::move(sid);
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void ExtensionsFilterModel::setOnlyRemovable(bool only) {
    if (only == onlyRemovable_) {
        return;
    }
    beginFilterChange();
    onlyRemovable_ = only;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool ExtensionsFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    const BrowserExtension* entry =
        entries_ != nullptr && !sourceParent.isValid() ? entries_->entryAt(sourceRow) : nullptr;
    if (entry == nullptr) {
        return false;
    }
    if (browser_ && entry->browser != *browser_) {
        return false;
    }
    if (user_ && !platform::iequals(entry->user_sid, *user_)) {
        return false;
    }
    if (onlyRemovable_ && entry->removal != tools::ExtensionRemoval::Removable) {
        return false;
    }
    if (searchText_.isEmpty()) {
        return true;
    }
    for (const QString& text :
         {ExtensionsText::nameText(*entry), toQString(entry->extension_id), toQString(entry->description),
          ExtensionsText::profileText(*entry), entries_->userText(*entry)}) {
        if (text.contains(searchText_, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

}  // namespace avel::app
