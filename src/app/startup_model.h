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
#include "core/tools/startup.h"

namespace avel::app {

// One listing of the Startup manager, as the window's loader made it.
struct StartupListing {
    std::vector<tools::StartupEntry> entries;
    // The installed programs the owners were found among, for their names.
    std::shared_ptr<const inventory::Inventory> inventory;
    std::vector<leftovers::ScanDiagnostic> environmentDiagnostics;
    std::vector<tools::StartupDiagnostic> diagnostics;
    // The change store couldn't be read (busy or unavailable): services Avel disabled may be missing.
    bool avelDisabledUnknown = false;
    std::chrono::milliseconds elapsed{0};
};

// One row per startup entry. StartupFilterModel decides which rows show.
class StartupTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        NameColumn,
        StateColumn,
        SourceColumn,
        UserColumn,
        PublisherColumn,
        CommandColumn,
        ColumnCount
    };
    static constexpr int SortRole = Qt::UserRole + 1;

    explicit StartupTableModel(QObject* parent = nullptr);

    void setListing(std::shared_ptr<const StartupListing> listing);
    const StartupListing* listing() const { return listing_.get(); }
    const tools::StartupEntry* entryAt(int row) const;
    // The row of the entry with this id, or -1.
    int rowOf(const std::wstring& id) const;
    // The owner program's name, by the program's id; empty when it isn't installed (any more).
    QString programName(const std::wstring& programId) const;

    // Headers and labels are translated text; call after the UI language changes.
    void retranslate();

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    std::shared_ptr<const StartupListing> listing_;
    QHash<QString, QString> programNames_;  // By program id
};

// The search text, the source and the user on top of StartupTableModel; sorting is locale-aware.
class StartupFilterModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit StartupFilterModel(QObject* parent = nullptr);

    void setSourceStartupModel(StartupTableModel* model);
    // Matches the name, the command, the publisher, the target and the owner program.
    void setSearchText(const QString& text);
    // nullopt: every source.
    void setKind(std::optional<tools::StartupKind> kind);
    // nullopt: every user; an empty SID: the machine's entries.
    void setUser(std::optional<std::wstring> sid);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    StartupTableModel* entries_ = nullptr;
    QString searchText_;
    std::optional<tools::StartupKind> kind_;
    std::optional<std::wstring> user_;
};

}  // namespace avel::app
