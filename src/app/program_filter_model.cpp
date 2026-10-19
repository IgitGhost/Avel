#include "program_filter_model.h"

#include "program_table_model.h"

namespace avel::app {

ProgramFilterModel::ProgramFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    setSortRole(ProgramTableModel::SortRole);
    setSortLocaleAware(true);
    setSortCaseSensitivity(Qt::CaseInsensitive);
}

void ProgramFilterModel::setSourceProgramModel(ProgramTableModel* model) {
    programs_ = model;
    setSourceModel(model);
}

void ProgramFilterModel::setCategory(Category category) {
    if (category == category_) {
        return;
    }
    beginFilterChange();
    category_ = category;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

void ProgramFilterModel::setSearchText(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed == searchText_) {
        return;
    }
    beginFilterChange();
    searchText_ = trimmed;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

int ProgramFilterModel::categoryRowCount() const {
    if (programs_ == nullptr) {
        return 0;
    }
    int count = 0;
    for (int row = 0; row < programs_->rowCount(); ++row) {
        count += inCategory(row) ? 1 : 0;
    }
    return count;
}

bool ProgramFilterModel::inCategory(int sourceRow) const {
    const auto* program = programs_ ? programs_->programAt(sourceRow) : nullptr;
    if (program == nullptr) {
        return false;
    }
    const bool listed = inventory::is_listed_in_settings(*program);
    const bool packaged = program->source == inventory::Source::Appx;
    switch (category_) {
        case Category::Apps:
            return listed;
        case Category::DesktopApps:
            return listed && !packaged;
        case Category::StoreApps:
            return listed && packaged;
        case Category::Updates:
            return program->is_update;
        case Category::SystemComponents:
            return !listed && !program->is_update;
        case Category::Everything:
            return true;
    }
    return false;
}

bool ProgramFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    if (sourceParent.isValid() || !inCategory(sourceRow)) {
        return false;
    }
    if (searchText_.isEmpty()) {
        return true;
    }
    for (const int column : {ProgramTableModel::NameColumn, ProgramTableModel::PublisherColumn,
                             ProgramTableModel::VersionColumn}) {
        const QString text = programs_->index(sourceRow, column).data(Qt::DisplayRole).toString();
        if (text.contains(searchText_, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

}  // namespace avel::app
