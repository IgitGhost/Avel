#include "windows_apps_model.h"

#include <QGuiApplication>
#include <QPalette>
#include <QStringList>

#include <algorithm>

#include "core/platform/text.h"
#include "program_text.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using tools::AppCategory;
using tools::AppProtection;
using tools::WindowsApp;

namespace {

QString key(const std::wstring& id) { return toQString(id).toLower(); }

// "1.2.10.0" sorts after "1.2.9.0": every part padded.
QString versionSortKey(const std::wstring& version) {
    QStringList parts = toQString(version).split(u'.');
    for (QString& part : parts) {
        part = part.rightJustified(10, u'0');
    }
    return parts.join(u'.');
}

}  // namespace

WindowsAppsTableModel::WindowsAppsTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void WindowsAppsTableModel::setListing(std::shared_ptr<const WindowsAppsListing> listing) {
    beginResetModel();
    listing_ = std::move(listing);
    QSet<QString> kept;
    if (listing_) {
        for (const auto& app : listing_->list.apps) {
            if (checkable(app) && checked_.contains(key(app.id))) {
                kept.insert(key(app.id));
            }
        }
    }
    const bool changed = kept != checked_;
    checked_ = std::move(kept);
    endResetModel();
    if (changed) {
        Q_EMIT checksChanged();
    }
}

const WindowsApp* WindowsAppsTableModel::appAt(int row) const {
    if (!listing_ || row < 0 || row >= static_cast<int>(listing_->list.apps.size())) {
        return nullptr;
    }
    return &listing_->list.apps[static_cast<std::size_t>(row)];
}

int WindowsAppsTableModel::rowOf(const std::wstring& id) const {
    if (!listing_) {
        return -1;
    }
    for (std::size_t row = 0; row < listing_->list.apps.size(); ++row) {
        if (platform::iequals(listing_->list.apps[row].id, id)) {
            return static_cast<int>(row);
        }
    }
    return -1;
}

bool WindowsAppsTableModel::checkable(const WindowsApp& app) { return app.protection == AppProtection::None; }

bool WindowsAppsTableModel::fits(const WindowsApp& app, WindowsAppsAction action) {
    switch (action) {
        case WindowsAppsAction::RemoveForMe:
            return app.can_remove_for_me;
        case WindowsAppsAction::Deprovision:
            return app.can_deprovision;
        case WindowsAppsAction::RemoveForEveryone:
            return app.can_remove_for_everyone;
    }
    return false;
}

int WindowsAppsTableModel::fitting(const std::vector<const WindowsApp*>& apps, WindowsAppsAction action) {
    return static_cast<int>(std::count_if(apps.begin(), apps.end(),
                                          [action](const WindowsApp* app) { return fits(*app, action); }));
}

bool WindowsAppsTableModel::isChecked(const std::wstring& id) const { return checked_.contains(key(id)); }

bool WindowsAppsTableModel::setChecked(const std::wstring& id, bool checked) {
    const int row = rowOf(id);
    const WindowsApp* app = appAt(row);
    if (app == nullptr || !checkable(*app)) {
        return false;
    }
    if (checked == checked_.contains(key(app->id))) {
        return true;
    }
    if (checked) {
        checked_.insert(key(app->id));
    } else {
        checked_.remove(key(app->id));
    }
    emitCheckChanged(row);
    Q_EMIT checksChanged();
    return true;
}

int WindowsAppsTableModel::checkOftenRemoved() {
    if (!listing_) {
        return 0;
    }
    int added = 0;
    for (std::size_t row = 0; row < listing_->list.apps.size(); ++row) {
        const WindowsApp& app = listing_->list.apps[row];
        if (app.category != AppCategory::OftenRemoved || !checkable(app) || checked_.contains(key(app.id))) {
            continue;
        }
        checked_.insert(key(app.id));
        emitCheckChanged(static_cast<int>(row));
        ++added;
    }
    if (added > 0) {
        Q_EMIT checksChanged();
    }
    return added;
}

void WindowsAppsTableModel::uncheckAll() {
    if (checked_.isEmpty()) {
        return;
    }
    checked_.clear();
    if (rowCount() > 0) {
        Q_EMIT dataChanged(index(0, CheckColumn), index(rowCount() - 1, CheckColumn), {Qt::CheckStateRole});
    }
    Q_EMIT checksChanged();
}

std::vector<const WindowsApp*> WindowsAppsTableModel::checkedApps() const {
    std::vector<const WindowsApp*> apps;
    if (listing_) {
        for (const auto& app : listing_->list.apps) {
            if (checked_.contains(key(app.id))) {
                apps.push_back(&app);
            }
        }
    }
    return apps;
}

std::vector<std::wstring> WindowsAppsTableModel::checkedIds() const {
    std::vector<std::wstring> ids;
    for (const auto* app : checkedApps()) {
        ids.push_back(app->id);
    }
    return ids;
}

void WindowsAppsTableModel::emitCheckChanged(int row) {
    const QModelIndex cell = index(row, CheckColumn);
    Q_EMIT dataChanged(cell, cell, {Qt::CheckStateRole, SortRole});
}

void WindowsAppsTableModel::retranslate() {
    Q_EMIT headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);
    if (rowCount() > 0) {
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
    }
}

int WindowsAppsTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() || !listing_ ? 0 : static_cast<int>(listing_->list.apps.size());
}

int WindowsAppsTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant WindowsAppsTableModel::data(const QModelIndex& index, int role) const {
    const WindowsApp* app = index.isValid() ? appAt(index.row()) : nullptr;
    if (app == nullptr) {
        return {};
    }
    const bool isProtected = !checkable(*app);
    switch (role) {
        case Qt::DisplayRole:
            switch (index.column()) {
                case NameColumn:
                    return WindowsAppsText::nameText(*app);
                case PublisherColumn:
                    return toQString(app->publisher);
                case VersionColumn:
                    return toQString(app->version);
                case InstalledForColumn:
                    return WindowsAppsText::installedForText(*app);
                case CategoryColumn:
                    return WindowsAppsText::categoryColumnText(*app);
            }
            break;
        case SortRole:
            switch (index.column()) {
                case CheckColumn:
                    // Checked first, then the checkable ones, when sorted descending.
                    return isChecked(app->id) ? 2 : (isProtected ? 0 : 1);
                case VersionColumn:
                    return versionSortKey(app->version);
                case InstalledForColumn:
                    return static_cast<int>(app->users.size()) * 2 + (app->provisioned ? 1 : 0);
                default:
                    return data(index, Qt::DisplayRole);
            }
        case Qt::CheckStateRole:
            if (index.column() == CheckColumn && !isProtected) {
                return isChecked(app->id) ? Qt::Checked : Qt::Unchecked;
            }
            break;
        case Qt::ToolTipRole:
            if (isProtected) {
                return WindowsAppsText::protectionText(*app);
            }
            if (index.column() == CategoryColumn && app->warning != tools::AppWarning::None) {
                return WindowsAppsText::warningText(app->warning);
            }
            if (index.column() == NameColumn) {
                return toQString(app->family_name);
            }
            break;
        case Qt::AccessibleTextRole:
            // The check box cell has no text of its own: UI Automation names it after the app.
            if (index.column() == CheckColumn) {
                return WindowsAppsText::nameText(*app);
            }
            break;
        case Qt::ForegroundRole:
            // Protected apps: shown, never offered for a change.
            if (isProtected) {
                return QGuiApplication::palette().brush(QPalette::Disabled, QPalette::Text);
            }
            break;
        default:
            break;
    }
    return {};
}

bool WindowsAppsTableModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    const WindowsApp* app = index.isValid() ? appAt(index.row()) : nullptr;
    if (app == nullptr || role != Qt::CheckStateRole || index.column() != CheckColumn) {
        return false;
    }
    return setChecked(app->id, value.value<Qt::CheckState>() == Qt::Checked);
}

Qt::ItemFlags WindowsAppsTableModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags flags = QAbstractTableModel::flags(index);
    const WindowsApp* app = index.isValid() ? appAt(index.row()) : nullptr;
    if (app != nullptr && index.column() == CheckColumn && checkable(*app)) {
        flags |= Qt::ItemIsUserCheckable;
    }
    return flags;
}

QVariant WindowsAppsTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    if (role == Qt::ToolTipRole && section == CheckColumn) {
        return tr("Checked apps: the actions apply to them");
    }
    if (role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
        case CheckColumn:
            return QString();
        case NameColumn:
            return tr("Name");
        case PublisherColumn:
            return tr("Publisher");
        case VersionColumn:
            return tr("Version");
        case InstalledForColumn:
            return tr("Installed for");
        case CategoryColumn:
            return tr("Category");
    }
    return {};
}

WindowsAppsFilterModel::WindowsAppsFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    setSortRole(WindowsAppsTableModel::SortRole);
    setSortLocaleAware(true);
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

void WindowsAppsFilterModel::setSourceAppsModel(WindowsAppsTableModel* model) {
    apps_ = model;
    setSourceModel(model);
}

void WindowsAppsFilterModel::setSearchText(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed == searchText_) {
        return;
    }
    beginFilterChange();
    searchText_ = trimmed;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void WindowsAppsFilterModel::setCategory(Category category) {
    if (category == category_) {
        return;
    }
    beginFilterChange();
    category_ = category;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void WindowsAppsFilterModel::setUser(User user) {
    if (user == user_) {
        return;
    }
    beginFilterChange();
    user_ = std::move(user);
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool WindowsAppsFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    const WindowsApp* app = apps_ != nullptr && !sourceParent.isValid() ? apps_->appAt(sourceRow) : nullptr;
    if (app == nullptr) {
        return false;
    }
    const bool isProtected = !WindowsAppsTableModel::checkable(*app);
    switch (category_) {
        case Category::All:
            break;
        case Category::Removable:
            if (isProtected) {
                return false;
            }
            break;
        case Category::OftenRemoved:
            if (app->category != AppCategory::OftenRemoved) {
                return false;
            }
            break;
        case Category::SemiCritical:
            if (app->category != AppCategory::SemiCritical) {
                return false;
            }
            break;
        case Category::Protected:
            if (!isProtected) {
                return false;
            }
            break;
    }
    switch (user_.kind) {
        case User::Kind::All:
            break;
        case User::Kind::NewUsers:
            if (!app->provisioned) {
                return false;
            }
            break;
        case User::Kind::Sid:
            if (std::none_of(app->users.begin(), app->users.end(),
                             [this](const auto& user) { return platform::iequals(user.sid, user_.sid); })) {
                return false;
            }
            break;
    }
    if (searchText_.isEmpty()) {
        return true;
    }
    for (const QString& text : {WindowsAppsText::nameText(*app), toQString(app->name),
                                toQString(app->family_name), toQString(app->publisher)}) {
        if (text.contains(searchText_, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

}  // namespace avel::app
