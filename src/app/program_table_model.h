#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QList>
#include <QStringList>

#include <memory>
#include <vector>

#include "core/inventory/inventory.h"

namespace avel::app {

// One row per inventory entry. The proxy model (ProgramFilterModel) decides which rows show.
class ProgramTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column {
        NameColumn,
        PublisherColumn,
        VersionColumn,
        SizeColumn,
        DateColumn,
        TypeColumn,
        ColumnCount
    };
    // Raw values for sorting: numbers for sizes, QDate for dates, text otherwise.
    static constexpr int SortRole = Qt::UserRole + 1;

    explicit ProgramTableModel(QObject* parent = nullptr);

    void setInventory(std::shared_ptr<const inventory::Inventory> inventory);
    const inventory::Inventory* inventory() const { return inventory_.get(); }
    const inventory::Program* programAt(int row) const;

    // Distinct icon sources of all entries, for IconLoader.
    QStringList iconSources() const;
    void setIcon(const QString& source, const QImage& image);

    // Headers and type labels are translated text; call after the UI language changes.
    void retranslate();

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    struct Row {
        const inventory::Program* program = nullptr;
        QString name;
        QString publisher;
        QString version;
        QString iconSource;
    };

    std::shared_ptr<const inventory::Inventory> inventory_;
    std::vector<Row> rows_;
    QHash<QString, QIcon> icons_;
    QHash<QString, QList<int>> rowsByIconSource_;
    QIcon defaultIcon_;
};

}  // namespace avel::app
