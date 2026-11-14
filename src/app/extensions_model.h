#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QSortFilterProxyModel>
#include <QString>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/inventory/inventory.h"
#include "core/leftovers/environment.h"
#include "core/tools/browser_extensions.h"

namespace avel::app {

// One listing of the browser extensions window, as its loader made it.
struct ExtensionsListing {
    tools::BrowserExtensionsList list;
    // The installed programs the sources' owners were found among, for their names.
    std::shared_ptr<const inventory::Inventory> inventory;
    std::vector<leftovers::ScanDiagnostic> environmentDiagnostics;
    std::wstring currentUserSid;  // The user running Avel
    // The user the desktop's shell runs as, who an extensions page opens for (unelevated_launch.h);
    // empty when there is no shell.
    std::wstring shellUserSid;
    std::chrono::milliseconds elapsed{0};
};

// One row per extension entry (an extension in a profile, or a source with no profile entry). Rows
// look alike whether or not Avel can remove them: they are the user's own extensions.
// ExtensionsFilterModel decides which rows show.
class ExtensionsTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        NameColumn,
        BrowserColumn,
        ProfileColumn,
        UserColumn,
        StateColumn,
        InstallColumn,
        StoreColumn,
        VersionColumn,
        RemovalColumn,
        ColumnCount
    };
    static constexpr int SortRole = Qt::UserRole + 1;

    explicit ExtensionsTableModel(QObject* parent = nullptr);

    void setListing(std::shared_ptr<const ExtensionsListing> listing);
    const ExtensionsListing* listing() const { return listing_.get(); }
    const tools::BrowserExtension* entryAt(int row) const;
    // The row of the entry with this id, or -1.
    int rowOf(const std::wstring& id) const;
    // The entry with this id, or null.
    const tools::BrowserExtension* entryWithId(const std::wstring& id) const;
    // The owner program's name, by the program's id; empty when it isn't installed (any more).
    QString programName(const std::wstring& programId) const;
    // The User column's text for the entry.
    QString userText(const tools::BrowserExtension& entry) const;

    // Headers and labels are translated text; call after the UI language changes.
    void retranslate();

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    std::shared_ptr<const ExtensionsListing> listing_;
    QHash<QString, QString> programNames_;  // By program id
};

// The search text, the browser, the user and "only removable" on top of ExtensionsTableModel;
// sorting is locale-aware.
class ExtensionsFilterModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit ExtensionsFilterModel(QObject* parent = nullptr);

    void setSourceExtensionsModel(ExtensionsTableModel* model);
    // Matches the name, the extension id, the description, the profile and the user.
    void setSearchText(const QString& text);
    // nullopt: every browser.
    void setBrowser(std::optional<tools::BrowserKind> browser);
    // nullopt: every user; an empty SID: the machine's sources' entries.
    void setUser(std::optional<std::wstring> sid);
    void setOnlyRemovable(bool only);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    ExtensionsTableModel* entries_ = nullptr;
    QString searchText_;
    std::optional<tools::BrowserKind> browser_;
    std::optional<std::wstring> user_;
    bool onlyRemovable_ = false;
};

}  // namespace avel::app
