#include "undo_center_dialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <chrono>

#include "change_set_progress_dialog.h"
#include "leftover_text.h"
#include "program_text.h"
#include "right_elide_delegate.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using changes::ItemState;

namespace {

enum SetColumn { DateColumn, DescriptionColumn, StateColumn, RemovedColumn, RestoredColumn, SizeColumn };
enum ItemColumn { ItemPathColumn, ItemTypeColumn, ItemStateColumn };

constexpr int kSetIdRole = Qt::UserRole;
constexpr int kItemIndexRole = Qt::UserRole;

// Reading a set's journal waits this long for another Avel process that is changing it.
constexpr auto kReadTimeout = std::chrono::seconds(2);

bool restorable(const changes::ItemStatus& status, const leftovers::Location& location) {
    return (status.state == ItemState::Done || status.state == ItemState::Partial ||
            status.state == ItemState::RestoreSkipped || status.state == ItemState::RestoreFailed) &&
           changes::is_undoable(location);
}

}  // namespace

UndoCenterDialog::UndoCenterDialog(std::shared_ptr<changes::ChangeStore> store,
                                   const QString& unavailableReason, QWidget* parent)
    : QDialog(parent), store_(std::move(store)) {
    setWindowTitle(tr("Undo Center"));

    setsLabel_ = new QLabel(this);
    setsLabel_->setWordWrap(true);

    sets_ = new QTreeWidget(this);
    sets_->setColumnCount(6);
    sets_->setHeaderLabels(
        {tr("Date"), tr("Description"), tr("State"), tr("Removed"), tr("Restored"), tr("Backup size")});
    sets_->setRootIsDecorated(false);
    sets_->setUniformRowHeights(true);
    sets_->setAllColumnsShowFocus(true);
    sets_->header()->setStretchLastSection(false);
    sets_->header()->setSectionResizeMode(DescriptionColumn, QHeaderView::Stretch);
    sets_->setColumnWidth(DateColumn, 150);
    sets_->setColumnWidth(StateColumn, 130);
    sets_->setColumnWidth(RemovedColumn, 80);
    sets_->setColumnWidth(RestoredColumn, 80);
    sets_->setColumnWidth(SizeColumn, 100);

    itemsLabel_ = new QLabel(this);
    itemsLabel_->setWordWrap(true);
    items_ = new QTreeWidget(this);
    items_->setColumnCount(3);
    items_->setHeaderLabels({tr("Item"), tr("Type"), tr("State")});
    items_->setRootIsDecorated(false);
    items_->setUniformRowHeights(true);
    items_->setAllColumnsShowFocus(true);
    items_->setTextElideMode(Qt::ElideMiddle);
    items_->setItemDelegateForColumn(ItemStateColumn, new RightElideDelegate(items_));
    items_->setColumnWidth(ItemPathColumn, 520);
    items_->setColumnWidth(ItemTypeColumn, 120);
    items_->header()->setStretchLastSection(true);

    auto* lower = new QWidget(this);
    auto* lowerLayout = new QVBoxLayout(lower);
    lowerLayout->setContentsMargins(0, 0, 0, 0);
    lowerLayout->addWidget(itemsLabel_);
    lowerLayout->addWidget(items_, 1);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(sets_);
    splitter->addWidget(lower);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);

    auto* buttons = new QDialogButtonBox(this);
    restoreAllButton_ = buttons->addButton(tr("Restore all"), QDialogButtonBox::ActionRole);
    restoreCheckedButton_ = buttons->addButton(tr("Restore checked"), QDialogButtonBox::ActionRole);
    deleteButton_ = buttons->addButton(tr("Delete backup…"), QDialogButtonBox::ActionRole);
    deleteButton_->setToolTip(
        tr("Delete this set's backup now. What it removed can't be restored afterwards."));
    QPushButton* close = buttons->addButton(QDialogButtonBox::Close);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(restoreAllButton_, &QPushButton::clicked, this, [this] { restore(false); });
    connect(restoreCheckedButton_, &QPushButton::clicked, this, [this] { restore(true); });
    connect(deleteButton_, &QPushButton::clicked, this, &UndoCenterDialog::deleteSet);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(setsLabel_);
    layout->addWidget(splitter, 1);
    layout->addWidget(buttons);
    resize(1000, 640);

    connect(sets_, &QTreeWidget::currentItemChanged, this, &UndoCenterDialog::showSelectedSet);
    connect(items_, &QTreeWidget::itemChanged, this, &UndoCenterDialog::updateButtons);

    if (!store_) {
        setsLabel_->setText(unavailableReason);
        splitter->setEnabled(false);
        updateButtons();
        return;
    }
    reloadSets();
}

void UndoCenterDialog::reloadSets(const QString& select) {
    const QSignalBlocker blocker(sets_);
    sets_->clear();
    const auto summaries = store_->list();
    QTreeWidgetItem* selected = nullptr;
    for (const auto& summary : summaries) {
        auto* row = new QTreeWidgetItem(sets_);
        const QString id = toQString(summary.id);
        row->setData(DateColumn, kSetIdRole, id);
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(summary.created.time_since_epoch()).count();
        row->setText(DateColumn,
                     QLocale().toString(QDateTime::fromSecsSinceEpoch(seconds), QLocale::ShortFormat));
        row->setText(DescriptionColumn, toQString(summary.description));
        row->setToolTip(DescriptionColumn, toQString(summary.description));
        row->setText(StateColumn, LeftoverText::setStateText(summary.state));
        row->setText(RemovedColumn, QLocale().toString(static_cast<qulonglong>(summary.done)));
        row->setText(RestoredColumn, QLocale().toString(static_cast<qulonglong>(summary.restored)));
        row->setText(SizeColumn,
                     summary.quarantined_bytes > 0 ? formatSize(summary.quarantined_bytes) : QString());
        for (const int column : {RemovedColumn, RestoredColumn, SizeColumn}) {
            row->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        }
        if (id == select || (select.isEmpty() && selected == nullptr)) {
            selected = row;
        }
    }
    setsLabel_->setText(summaries.empty()
                            ? tr("Nothing to undo. Avel keeps a backup of what it removes for 30 days.")
                            : tr("What Avel removed in the last 30 days, newest first. Select a "
                                 "removal to see its items."));
    if (selected == nullptr && sets_->topLevelItemCount() > 0) {
        selected = sets_->topLevelItem(0);
    }
    sets_->setCurrentItem(selected);
    showSelectedSet();
}

void UndoCenterDialog::showSelectedSet() {
    const QSignalBlocker blocker(items_);
    items_->clear();
    plan_ = {};
    statuses_.clear();
    shownId_ = selectedSetId();
    if (shownId_.isEmpty()) {
        itemsLabel_->clear();
        updateButtons();
        return;
    }
    {
        // Read and let go at once: undo opens the set again, and only one may hold it.
        const auto guard = store_->lock(kReadTimeout);
        const auto set = guard ? store_->open_set(shownId_.toStdWString()) : nullptr;
        if (!set) {
            itemsLabel_->setText(
                tr("This removal is in use by another Avel process. Try again when it's done."));
            shownId_.clear();
            updateButtons();
            return;
        }
        plan_ = set->plan();
        statuses_ = set->statuses();
    }
    QList<QTreeWidgetItem*> rows;
    rows.reserve(static_cast<qsizetype>(plan_.items.size()));
    for (std::size_t i = 0; i < plan_.items.size() && i < statuses_.size(); ++i) {
        const auto& item = plan_.items[i];
        auto* row = new QTreeWidgetItem;
        row->setData(ItemPathColumn, kItemIndexRole, static_cast<qulonglong>(i));
        row->setText(ItemPathColumn, LeftoverText::locationText(item.location));
        row->setToolTip(ItemPathColumn, row->text(ItemPathColumn));
        row->setText(ItemTypeColumn, LeftoverText::kindLabel(item.location, item.category));
        row->setText(ItemStateColumn, LeftoverText::statusText(statuses_[i]));
        row->setToolTip(ItemStateColumn, row->text(ItemStateColumn));
        if (restorable(statuses_[i], item.location)) {
            row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
            row->setCheckState(ItemPathColumn, Qt::Unchecked);
        }
        rows << row;
    }
    items_->addTopLevelItems(rows);
    itemsLabel_->setText(
        tr("%n item(s) in “%1”:", nullptr, static_cast<int>(rows.size())).arg(toQString(plan_.description)));
    updateButtons();
}

void UndoCenterDialog::restore(bool checkedOnly) {
    if (shownId_.isEmpty()) {
        return;
    }
    std::vector<std::size_t> only;
    if (checkedOnly) {
        for (int row = 0; row < items_->topLevelItemCount(); ++row) {
            const QTreeWidgetItem* item = items_->topLevelItem(row);
            if (item->checkState(ItemPathColumn) == Qt::Checked) {
                only.push_back(
                    static_cast<std::size_t>(item->data(ItemPathColumn, kItemIndexRole).toULongLong()));
            }
        }
        if (only.empty()) {
            return;
        }
    }
    auto* progress =
        new ChangeSetProgressDialog(store_, shownId_.toStdWString(), plan_, statuses_, std::move(only), this);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    connect(progress, &QDialog::finished, this, [this, progress, id = shownId_] {
        changedAnything_ = changedAnything_ || progress->changedAnything();
        reloadSets(id);
    });
    progress->open();
}

void UndoCenterDialog::deleteSet() {
    if (shownId_.isEmpty()) {
        return;
    }
    auto* box = new QMessageBox(QMessageBox::Warning, tr("Delete backup"),
                                tr("Delete the backup of “%1”? What it removed can't be restored afterwards.")
                                    .arg(toQString(plan_.description)),
                                QMessageBox::Yes | QMessageBox::Cancel, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setDefaultButton(QMessageBox::Cancel);
    connect(box, &QMessageBox::finished, this, [this, id = shownId_](int result) {
        if (result != QMessageBox::Yes) {
            return;
        }
        const auto guard = store_->lock(kReadTimeout);
        if (!guard || !store_->remove(id.toStdWString())) {
            QMessageBox::warning(this, tr("Delete backup"),
                                 tr("The backup couldn't be deleted. Another Avel process may be using it."));
        }
        reloadSets();
    });
    box->open();
}

void UndoCenterDialog::updateButtons() {
    int restorableCount = 0;
    int checked = 0;
    for (int row = 0; row < items_->topLevelItemCount(); ++row) {
        const QTreeWidgetItem* item = items_->topLevelItem(row);
        if (item->flags().testFlag(Qt::ItemIsUserCheckable)) {
            ++restorableCount;
            checked += item->checkState(ItemPathColumn) == Qt::Checked;
        }
    }
    const bool shown = store_ && !shownId_.isEmpty();
    restoreAllButton_->setEnabled(shown && restorableCount > 0);
    restoreCheckedButton_->setEnabled(shown && checked > 0);
    restoreCheckedButton_->setText(checked > 0 ? tr("Restore %n checked", nullptr, checked)
                                               : tr("Restore checked"));
    deleteButton_->setEnabled(shown);
}

QString UndoCenterDialog::selectedSetId() const {
    const QTreeWidgetItem* item = sets_->currentItem();
    return item != nullptr ? item->data(DateColumn, kSetIdRole).toString() : QString();
}

}  // namespace avel::app
