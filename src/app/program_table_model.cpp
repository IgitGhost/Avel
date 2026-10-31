#include "program_table_model.h"

#include <QDate>
#include <QLocale>
#include <QPainter>
#include <QPixmap>

#include <windows.h>
#include <shellapi.h>

#include "program_text.h"
#include "program_traces.h"

namespace avel::app {

namespace {

QImage stock_icon(SHSTOCKICONID id, UINT size) {
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(::SHGetStockIconInfo(id, SHGSI_ICON | size, &info)) || !info.hIcon) {
        return {};
    }
    QImage image = QImage::fromHICON(info.hIcon);
    ::DestroyIcon(info.hIcon);
    return image;
}

// Windows' stock "application" icon, used until (or instead of) a program's own icon.
QIcon stock_application_icon() {
    const QImage image = stock_icon(SIID_APPLICATION, SHGSI_LARGEICON);
    return image.isNull() ? QIcon() : QIcon(QPixmap::fromImage(image));
}

QDate to_qdate(std::chrono::sys_days date) {
    return QDate(1970, 1, 1).addDays(date.time_since_epoch().count());
}

}  // namespace

ProgramTableModel::ProgramTableModel(QObject* parent)
    : QAbstractTableModel(parent),
      defaultIcon_(stock_application_icon()),
      warningBadge_(stock_icon(SIID_WARNING, SHGSI_LARGEICON)) {}

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
            row.broken = inventory::is_broken(program);
            if (!row.iconSource.isEmpty()) {
                rowsByIconSource_[row.iconSource].append(static_cast<int>(rows_.size()));
            }
            rows_.push_back(std::move(row));
        }
    }
    endResetModel();
}

void ProgramTableModel::setTraces(const ProgramTraces* traces) {
    traces_ = traces;
    connect(traces_, &ProgramTraces::changed, this, [this] {
        if (!rows_.empty()) {
            Q_EMIT dataChanged(index(0, TraceColumn), index(rowCount() - 1, TraceColumn),
                               {Qt::DisplayRole, Qt::ToolTipRole});
        }
    });
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
    brokenIcons_.remove(source);
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
    const auto* trace =
        traces_ != nullptr && index.column() == TraceColumn ? traces_->find(program.id) : nullptr;

    switch (role) {
        case Qt::ToolTipRole:
            if (index.column() == NameColumn && row.broken) {
                return row.name + u'\n' + ProgramText::brokenReasons(program).join(u'\n');
            }
            if (trace != nullptr) {
                const QString summary = ProgramTraces::summaryText(*trace);
                return trace->incomplete ? summary + u'\n' + ProgramTraces::incompleteText() : summary;
            }
            [[fallthrough]];
        case Qt::DisplayRole:
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
                case TraceColumn:
                    return trace != nullptr ? ProgramTraces::markText(*trace) : QString();
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
                return row.broken ? brokenIcon(row.iconSource) : icons_.value(row.iconSource, defaultIcon_);
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

QIcon ProgramTableModel::brokenIcon(const QString& source) const {
    const auto cached = brokenIcons_.constFind(source);
    if (cached != brokenIcons_.cend()) {
        return *cached;
    }
    const QIcon base = icons_.value(source, defaultIcon_);
    const QList<QSize> sizes = base.availableSizes();
    QPixmap pixmap = base.pixmap(sizes.isEmpty() ? QSize(32, 32) : sizes.front());
    if (!pixmap.isNull() && !warningBadge_.isNull()) {
        // The badge covers the bottom right, like Explorer's overlay icons.
        const QSize full = pixmap.size();
        const QSize badge = full * 0.6;
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QRect(QPoint(full.width() - badge.width(), full.height() - badge.height()), badge),
                          warningBadge_);
    }
    const QIcon icon(pixmap);
    brokenIcons_.insert(source, icon);
    return icon;
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
        case TraceColumn:
            return tr("Trace");
    }
    return {};
}

}  // namespace avel::app
