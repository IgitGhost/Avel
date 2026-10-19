#include "program_table_model.h"

#include <QDate>
#include <QLocale>
#include <QPixmap>

#include <windows.h>
#include <shellapi.h>

#include "program_text.h"

namespace avel::app {

namespace {

// Windows' stock "application" icon, used until (or instead of) a program's own icon.
QIcon stock_application_icon() {
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(::SHGetStockIconInfo(SIID_APPLICATION, SHGSI_ICON | SHGSI_LARGEICON, &info)) || !info.hIcon) {
        return {};
    }
    const QIcon icon(QPixmap::fromImage(QImage::fromHICON(info.hIcon)));
    ::DestroyIcon(info.hIcon);
    return icon;
}

QDate to_qdate(std::chrono::sys_days date) {
    return QDate(1970, 1, 1).addDays(date.time_since_epoch().count());
}

}  // namespace

ProgramTableModel::ProgramTableModel(QObject* parent)
    : QAbstractTableModel(parent), defaultIcon_(stock_application_icon()) {}

void ProgramTableModel::setInventory(std::shared_ptr<const inventory::Inventory> inventory) {
    beginResetModel();
    inventory_ = std::move(inventory);
    rows_.clear();
    rowsByIconSource_.clear();
    if (inventory_) {
        rows_.reserve(inventory_->programs.size());
        for (const auto& program : inventory_->programs) {
            Row row;
            row.program = &program;
            row.name = toQString(program.display_name);
            if (row.name.isEmpty() && program.registry) {
                // Nameless entries are shown by their registry key so they can still be found.
                row.name = u'(' + toQString(program.registry->key_name) + u')';
            }
            row.publisher = toQString(program.publisher);
            row.version = toQString(program.display_version);
            row.iconSource = toQString(program.display_icon);
            if (!row.iconSource.isEmpty()) {
                rowsByIconSource_[row.iconSource].append(static_cast<int>(rows_.size()));
            }
            rows_.push_back(std::move(row));
        }
    }
    endResetModel();
}

const inventory::Program* ProgramTableModel::programAt(int row) const {
    return row >= 0 && row < static_cast<int>(rows_.size()) ? rows_[static_cast<size_t>(row)].program
                                                            : nullptr;
}

QStringList ProgramTableModel::iconSources() const {
    QStringList sources;
    for (auto it = rowsByIconSource_.cbegin(); it != rowsByIconSource_.cend(); ++it) {
        if (!icons_.contains(it.key())) {
            sources.append(it.key());
        }
    }
    return sources;
}

void ProgramTableModel::setIcon(const QString& source, const QImage& image) {
    icons_.insert(source, QIcon(QPixmap::fromImage(image)));
    for (const int row : rowsByIconSource_.value(source)) {
        const QModelIndex cell = index(row, NameColumn);
        Q_EMIT dataChanged(cell, cell, {Qt::DecorationRole});
    }
}

void ProgramTableModel::retranslate() {
    Q_EMIT headerDataChanged(Qt::Horizontal, 0, ColumnCount - 1);
    if (!rows_.empty()) {
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1), {Qt::DisplayRole});
    }
}

int ProgramTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int ProgramTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ProgramTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= static_cast<int>(rows_.size())) {
        return {};
    }
    const Row& row = rows_[static_cast<size_t>(index.row())];
    const inventory::Program& program = *row.program;

    switch (role) {
        case Qt::DisplayRole:
        case Qt::ToolTipRole:
            switch (index.column()) {
                case NameColumn:
                    return row.name;
                case PublisherColumn:
                    return row.publisher;
                case VersionColumn:
                    return row.version;
                case SizeColumn:
                    return program.estimated_size_bytes == 0 ? QString()
                                                             : formatSize(program.estimated_size_bytes);
                case DateColumn:
                    return program.install_date
                               ? QLocale().toString(to_qdate(*program.install_date), QLocale::ShortFormat)
                               : QString();
                case TypeColumn:
                    return ProgramText::typeLabel(program);
            }
            break;
        case SortRole:
            switch (index.column()) {
                case SizeColumn:
                    return QVariant::fromValue<qulonglong>(program.estimated_size_bytes);
                case DateColumn:
                    return program.install_date ? QVariant(to_qdate(*program.install_date))
                                                : QVariant(QDate());
                default:
                    return data(index, Qt::DisplayRole);
            }
        case Qt::DecorationRole:
            if (index.column() == NameColumn) {
                return icons_.value(row.iconSource, defaultIcon_);
            }
            break;
        case Qt::TextAlignmentRole:
            if (index.column() == SizeColumn) {
                return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
            }
            break;
        default:
            break;
    }
    return {};
}

QVariant ProgramTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    switch (section) {
        case NameColumn:
            return tr("Name");
        case PublisherColumn:
            return tr("Publisher");
        case VersionColumn:
            return tr("Version");
        case SizeColumn:
            return tr("Size");
        case DateColumn:
            return tr("Installed");
        case TypeColumn:
            return tr("Type");
    }
    return {};
}

}  // namespace avel::app
