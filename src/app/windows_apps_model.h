#pragma once

#include <QAbstractTableModel>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/tools/windows_apps.h"
#include "windows_apps_text.h"

namespace avel::app {

// One listing of the Windows apps window, as its loader made it.
struct WindowsAppsListing {
    tools::WindowsAppsList list;
    std::chrono::milliseconds elapsed{0};
};

// One row per app, with a check box. Protected apps are greyed and can't be checked; nothing is
// checked by default. WindowsAppsFilterModel decides which rows show.
class WindowsAppsTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        CheckColumn,
        NameColumn,
        PublisherColumn,
        VersionColumn,
        InstalledForColumn,
        CategoryColumn,
        ColumnCount
    };
    static constexpr int SortRole = Qt::UserRole + 1;

    explicit WindowsAppsTableModel(QObject* parent = nullptr);

    // Keeps the checks of the apps still listed and still checkable.
    void setListing(std::shared_ptr<const WindowsAppsListing> listing);
    const WindowsAppsListing* listing() const { return listing_.get(); }
    const tools::WindowsApp* appAt(int row) const;
    // The row of the app with this id, or -1.
    int rowOf(const std::wstring& id) const;

    // Whether an app can be checked: it isn't protected.
    static bool checkable(const tools::WindowsApp& app);
    // Whether `action` is offered for the app (its mode in the listing).
    static bool fits(const tools::WindowsApp& app, WindowsAppsAction action);
    // How many of `apps` the action is offered for.
    static int fitting(const std::vector<const tools::WindowsApp*>& apps, WindowsAppsAction action);

    bool isChecked(const std::wstring& id) const;
    // False when there is no such app or it can't be checked.
    bool setChecked(const std::wstring& id, bool checked);
    // Checks every checkable app of the "often removed" category; returns how many it newly checked.
    int checkOftenRemoved();
    void uncheckAll();
    // The checked apps, in the listing's order.
    std::vector<const tools::WindowsApp*> checkedApps() const;
    std::vector<std::wstring> checkedIds() const;

    // Headers and labels are translated text; call after the UI language changes.
    void retranslate();

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

Q_SIGNALS:
    // A check changed, by the user or a call above.
    void checksChanged();

private:
    void emitCheckChanged(int row);

    std::shared_ptr<const WindowsAppsListing> listing_;
    QSet<QString> checked_;  // Ids, lowercase
};

// The search text, the category and the user on top of WindowsAppsTableModel; sorting is
// locale-aware.
class WindowsAppsFilterModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    enum class Category {
        All,
        Removable,  // Not protected
        OftenRemoved,
        SemiCritical,
        Protected,
    };
    // Who has the app.
    struct User {
        enum class Kind { All, Sid, NewUsers } kind = Kind::All;
        std::wstring sid;  // For Sid
        bool operator==(const User&) const = default;
    };

    explicit WindowsAppsFilterModel(QObject* parent = nullptr);

    void setSourceAppsModel(WindowsAppsTableModel* model);
    // Matches the display name, the identity and family names and the publisher.
    void setSearchText(const QString& text);
    void setCategory(Category category);
    void setUser(User user);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    WindowsAppsTableModel* apps_ = nullptr;
    QString searchText_;
    Category category_ = Category::All;
    User user_;
};

}  // namespace avel::app
