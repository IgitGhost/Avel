#include "change_set_progress_dialog.h"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "leftover_text.h"
#include "program_text.h"
#include "right_elide_delegate.h"

using namespace Qt::StringLiterals;

namespace avel::app {

using changes::ItemState;
using changes::Reason;

namespace {

// How long to wait for another Avel process (the CLI, or an Undo Center) to finish its changes.
constexpr auto kLockTimeout = std::chrono::seconds(30);
constexpr std::size_t kMaxLeftBehindShown = 20;

bool removed(const changes::ItemStatus& status) {
    return status.state == ItemState::Done || status.state == ItemState::Partial ||
           (status.state == ItemState::Skipped && status.reason == Reason::Contained);
}

bool restorable(const changes::ItemStatus& status) {
    return status.state == ItemState::Done || status.state == ItemState::Partial ||
           status.state == ItemState::RestoreSkipped || status.state == ItemState::RestoreFailed;
}

}  // namespace

ChangeSetProgressDialog::ChangeSetProgressDialog(std::shared_ptr<changes::ChangeStore> store,
                                                 changes::ChangePlan plan, QWidget* parent)
    : QDialog(parent), store_(std::move(store)), plan_(std::move(plan)) {
    statuses_.resize(plan_.items.size());

    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    progress_ = new QProgressBar(this);

    problemsLabel_ = new QLabel(this);
    problemsLabel_->hide();
    problems_ = new QTreeWidget(this);
    problems_->setColumnCount(3);
    problems_->setHeaderLabels({tr("Item"), tr("Type"), tr("Result")});
    problems_->setUniformRowHeights(true);
    problems_->setSelectionMode(QAbstractItemView::NoSelection);
    problems_->setTextElideMode(Qt::ElideMiddle);
    problems_->setItemDelegateForColumn(2, new RightElideDelegate(problems_));
    problems_->header()->setStretchLastSection(true);
    problems_->setColumnWidth(0, 420);
    problems_->setColumnWidth(1, 110);
    problems_->hide();

    auto* buttons = new QDialogButtonBox(this);
    undoButton_ = buttons->addButton(tr("Undo"), QDialogButtonBox::ActionRole);
    undoButton_->setToolTip(tr("Put back everything this removed"));
    undoButton_->hide();
    closeButton_ = buttons->addButton(QDialogButtonBox::Close);
    closeButton_->setEnabled(false);
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::accept);
    connect(undoButton_, &QPushButton::clicked, this, [this] {
        only_.clear();
        startRestore();
    });

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(statusLabel_);
    layout->addWidget(progress_);
    layout->addWidget(problemsLabel_);
    layout->addWidget(problems_, 1);
    layout->addStretch();
    layout->addWidget(buttons);
    resize(760, 220);
}

ChangeSetProgressDialog::ChangeSetProgressDialog(std::shared_ptr<changes::ChangeStore> store,
                                                 changes::ChangePlan plan, bool allowIrreversible,
                                                 QWidget* parent)
    : ChangeSetProgressDialog(std::move(store), std::move(plan), parent) {
    allowIrreversible_ = allowIrreversible;
    setWindowTitle(tr("Removing leftovers"));
}

ChangeSetProgressDialog::ChangeSetProgressDialog(std::shared_ptr<changes::ChangeStore> store,
                                                 std::wstring setId, changes::ChangePlan plan,
                                                 std::vector<changes::ItemStatus> statuses,
                                                 std::vector<std::size_t> only, QWidget* parent)
    : ChangeSetProgressDialog(std::move(store), std::move(plan), parent) {
    setId_ = std::move(setId);
    if (statuses.size() == statuses_.size()) {
        statuses_ = std::move(statuses);
    }
    only_ = std::move(only);
    restoring_ = true;
    setWindowTitle(tr("Restoring"));
}

ChangeSetProgressDialog::~ChangeSetProgressDialog() {
    if (worker_.joinable()) {
        // Only reachable if the application quits mid-run; the run finishes first.
        worker_.join();
    }
}

void ChangeSetProgressDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!started_) {
        started_ = true;
        restoring_ ? startRestore() : startRemoval();
    }
}

void ChangeSetProgressDialog::closeEvent(QCloseEvent* event) {
    if (running_) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void ChangeSetProgressDialog::reject() {
    if (!running_) {
        QDialog::reject();
    }
}

void ChangeSetProgressDialog::startRemoval() {
    running_ = true;
    statusLabel_->setText(
        tr("Backing up and removing %n item(s)…", nullptr, static_cast<int>(plan_.items.size())));
    progress_->setRange(0, static_cast<int>(plan_.items.size()));
    progress_->setValue(0);
    // The worker reads the plan, which stays as it is while it runs; the rest happens here.
    worker_ = std::thread([this, store = store_] {
        const auto guard = store->lock(kLockTimeout);
        if (!guard) {
            QMetaObject::invokeMethod(this, [this] { onBusy(); }, Qt::QueuedConnection);
            return;
        }
        changes::ExecuteOptions options;
        options.allow_irreversible = allowIrreversible_;
        changes::ChangeEvents events;
        events.item_finished = [this](std::size_t index, const changes::ItemResult& result) {
            QMetaObject::invokeMethod(
                this, [=, this] { onItemFinished(index, result); }, Qt::QueuedConnection);
        };
        const auto result = changes::execute(*store, plan_, options, events);
        QMetaObject::invokeMethod(this, [=, this] { onRemovalFinished(result); }, Qt::QueuedConnection);
    });
}

void ChangeSetProgressDialog::startRestore() {
    if (worker_.joinable()) {
        worker_.join();
    }
    restoring_ = true;
    running_ = true;
    setWindowTitle(tr("Restoring"));
    undoButton_->setEnabled(false);
    closeButton_->setEnabled(false);
    problems_->clear();
    problems_->hide();
    problemsLabel_->hide();
    const auto count = static_cast<int>(restorableCount());
    statusLabel_->setText(tr("Restoring %n item(s)…", nullptr, count));
    progress_->setRange(0, std::max(count, 1));
    progress_->setValue(0);
    worker_ = std::thread([this, store = store_, id = setId_, only = only_] {
        const auto guard = store->lock(kLockTimeout);
        if (!guard) {
            QMetaObject::invokeMethod(this, [this] { onBusy(); }, Qt::QueuedConnection);
            return;
        }
        changes::ChangeEvents events;
        events.item_finished = [this](std::size_t index, const changes::ItemResult& result) {
            QMetaObject::invokeMethod(
                this, [=, this] { onItemFinished(index, result); }, Qt::QueuedConnection);
        };
        const auto result = changes::restore(*store, id, only, {}, events);
        QMetaObject::invokeMethod(this, [=, this] { onRestoreFinished(result); }, Qt::QueuedConnection);
    });
}

void ChangeSetProgressDialog::onItemFinished(std::size_t index, const changes::ItemResult& result) {
    if (index < statuses_.size()) {
        statuses_[index] = result.status;
    }
    const ItemState state = result.status.state;
    changedAnything_ = changedAnything_ || state == ItemState::Done || state == ItemState::Partial ||
                       state == ItemState::Restored;
    progress_->setValue(std::min(progress_->value() + 1, progress_->maximum()));
    const bool problem =
        restoring_ ? state != ItemState::Restored : !removed(result.status) || state == ItemState::Partial;
    if (problem) {
        addProblem(index, result);
    }
}

void ChangeSetProgressDialog::onRemovalFinished(const changes::ExecuteResult& result) {
    finishRun();
    if (result.set_id.empty()) {
        statusLabel_->setText(
            tr("Avel couldn't start the removal because its backup couldn't be written. "
               "Nothing was removed."));
        return;
    }
    setId_ = result.set_id;
    std::size_t removedCount = 0;
    bool packageRemoved = false;
    for (std::size_t i = 0; i < result.items.size() && i < plan_.items.size(); ++i) {
        statuses_[i] = result.items[i].status;
        if (removed(statuses_[i])) {
            ++removedCount;
            packageRemoved = packageRemoved || (statuses_[i].state == ItemState::Done &&
                                                !changes::is_undoable(plan_.items[i].location));
        }
    }
    const std::size_t kept = plan_.items.size() - removedCount;
    QString text = tr("%n item(s) removed.", nullptr, static_cast<int>(removedCount));
    if (kept > 0) {
        text += u' ' + tr("%n item(s) left in place.", nullptr, static_cast<int>(kept));
    }
    if (result.quarantined_bytes > 0) {
        text += u' ' + tr("%1 of files moved to the backup.").arg(formatSize(result.quarantined_bytes));
    }
    if (removedCount > 0) {
        text += u"\n\n"_s + tr("Undo puts it all back, now or within 30 days from Tools > Undo Center.");
        if (packageRemoved) {
            text += u' ' + tr("Removed packages can't be put back.");
        }
    }
    statusLabel_->setText(text);
    undoButton_->setVisible(restorableCount() > 0);
    undoButton_->setEnabled(restorableCount() > 0);
}

void ChangeSetProgressDialog::onRestoreFinished(const changes::RestoreResult& result) {
    finishRun();
    undoButton_->hide();
    if (!result.found) {
        statusLabel_->setText(
            tr("The backup of this removal is missing or in use by another Avel process. "
               "Nothing was restored."));
        return;
    }
    const auto restored =
        static_cast<int>(std::count_if(result.items.begin(), result.items.end(), [](const auto& item) {
            return item.second.status.state == ItemState::Restored;
        }));
    const int notRestored = static_cast<int>(result.items.size()) - restored;
    QString text = tr("%n item(s) restored.", nullptr, restored);
    if (notRestored > 0) {
        text +=
            u' ' + tr("%n item(s) couldn't be restored; their backup is kept, so you can try again later.",
                      nullptr, notRestored);
    }
    statusLabel_->setText(text);
}

void ChangeSetProgressDialog::onBusy() {
    finishRun();
    statusLabel_->setText(
        tr("Another Avel process is changing the system. Nothing was changed; try again when "
           "it's done."));
}

void ChangeSetProgressDialog::finishRun() {
    if (worker_.joinable()) {
        worker_.join();
    }
    running_ = false;
    progress_->setValue(progress_->maximum());
    closeButton_->setEnabled(true);
    closeButton_->setDefault(true);
    closeButton_->setFocus();
}

void ChangeSetProgressDialog::addProblem(std::size_t index, const changes::ItemResult& result) {
    if (index >= plan_.items.size()) {
        return;
    }
    const auto& item = plan_.items[index];
    auto* row = new QTreeWidgetItem(problems_);
    row->setText(0, LeftoverText::locationText(item.location));
    row->setToolTip(0, row->text(0));
    row->setText(1, LeftoverText::kindLabel(item.location, item.category));
    row->setText(2, LeftoverText::statusText(result.status, item.location));
    row->setToolTip(2, row->text(2));
    for (const auto& holder : result.holders) {
        auto* child = new QTreeWidgetItem(row);
        child->setText(0, tr("In use by %1 (process %2)").arg(toQString(holder.name)).arg(holder.pid));
    }
    if (!result.left_behind.empty()) {
        QStringList files;
        for (std::size_t i = 0; i < result.left_behind.size() && i < kMaxLeftBehindShown; ++i) {
            files << toQString(result.left_behind[i]);
        }
        if (result.left_behind.size() > kMaxLeftBehindShown) {
            files << tr("… and %n more", nullptr,
                        static_cast<int>(result.left_behind.size() - kMaxLeftBehindShown));
        }
        row->setToolTip(2, row->text(2) + u"\n\n"_s + files.join(u'\n'));
    }
    if (problems_->isHidden()) {
        problemsLabel_->setText(restoring_ ? tr("Not restored:") : tr("Not removed, or not completely:"));
        problemsLabel_->show();
        problems_->show();
        resize(width(), std::max(height(), 460));
    }
}

std::size_t ChangeSetProgressDialog::restorableCount() const {
    std::size_t count = 0;
    for (std::size_t i = 0; i < statuses_.size(); ++i) {
        if (restorable(statuses_[i]) && changes::is_undoable(plan_.items[i].location) &&
            (only_.empty() || std::find(only_.begin(), only_.end(), i) != only_.end())) {
            ++count;
        }
    }
    return count;
}

}  // namespace avel::app
