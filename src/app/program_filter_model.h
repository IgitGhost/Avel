#pragma once

#include <QSortFilterProxyModel>
#include <QString>

namespace avel::app {

class ProgramTableModel;

// Category and search filtering on top of ProgramTableModel; sorting is locale-aware.
class ProgramFilterModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    enum class Category {
        Apps,              // What Settings > Apps shows (the default)
        DesktopApps,       // ...limited to classic installers
        StoreApps,         // ...limited to packaged apps
        Updates,           // Patches and hotfixes
        SystemComponents,  // Hidden entries: system components, frameworks, Windows Installer internals
        Everything,
        Broken,  // Entries that can't uninstall themselves (inventory::is_broken), for Avel Force
    };

    explicit ProgramFilterModel(QObject* parent = nullptr);

    void setSourceProgramModel(ProgramTableModel* model);
    void setCategory(Category category);
    Category category() const { return category_; }
    void setSearchText(const QString& text);

    // Rows the category admits, before the search text is applied.
    int categoryRowCount() const;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    bool inCategory(int sourceRow) const;

    ProgramTableModel* programs_ = nullptr;
    Category category_ = Category::Apps;
    QString searchText_;
};

}  // namespace avel::app
