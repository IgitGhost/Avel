#include "uninstall_progress_dialog.h"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <windows.h>

#include <algorithm>

#include "program_text.h"

namespace avel::app {

using uninstall::Outcome;

UninstallProgressDialog::UninstallProgressDialog(std::shared_ptr<const inventory::Inventory> inventory,
                                                 std::vector<uninstall::BatchItem> items,
                                                 uninstall::BatchOptions options, QWidget* parent)
    : QDialog(parent),
      inventory_(std::move(inventory)),
      items_(std::move(items)),
      options_(std::move(options)) {
    setWindowTitle(options_.dry_run ? tr("Simulating uninstall") : tr("Uninstalling"));
    results_.resize(items_.size());

    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, static_cast<int>(items_.size()));
    progress_->setValue(0);

    table_ = new QTableWidget(static_cast<int>(items_.size()), 2, this);
    table_->setHorizontalHeaderLabels({tr("Program"), tr("Status")});
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setShowGrid(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto* program = items_[static_cast<size_t>(row)].program;
        table_->setItem(
            row, 0,
            new QTableWidgetItem(program->display_name.empty() ? tr("(no name)")
                                                               : toQString(program->display_name)));
        setStatus(row, tr("Waiting"));
    }
    table_->resizeColumnToContents(0);
    table_->setColumnWidth(0, std::min(table_->columnWidth(0) + 16, 320));

    auto* buttons = new QDialogButtonBox(this);
    stopButton_ = buttons->addButton(tr("Stop"), QDialogButtonBox::ActionRole);
    closeButton_ = buttons->addButton(QDialogButtonBox::Close);
    closeButton_->setEnabled(false);
    connect(stopButton_, &QPushButton::clicked, this, &UninstallProgressDialog::requestStop);
    connect(closeButton_, &QPushButton::clicked, this, &QDialog::accept);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(statusLabel_);
    layout->addWidget(progress_);
    layout->addWidget(table_, 1);
    layout->addWidget(buttons);
    resize(720, std::min(220 + static_cast<int>(items_.size()) * 26, 600));
}

UninstallProgressDialog::~UninstallProgressDialog() {
    if (worker_.joinable()) {
        // Only reachable if the dialog is destroyed mid-batch (e.g. the application quits).
        cancellation_.stop_batch = true;
        worker_.join();
    }
}

void UninstallProgressDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!started_) {
        started_ = true;
        start();
    }
}

void UninstallProgressDialog::closeEvent(QCloseEvent* event) {
    if (running_) {
        event->ignore();
        requestStop();
        return;
    }
    QDialog::closeEvent(event);
}

void UninstallProgressDialog::reject() {
    // Esc while running asks to stop instead of closing.
    if (running_) {
        requestStop();
        return;
    }
    QDialog::reject();
}

void UninstallProgressDialog::start() {
    running_ = true;
    const bool any_possible =
        std::any_of(items_.begin(), items_.end(), [](const auto& item) { return item.plan.possible(); });
    statusLabel_->setText(options_.restore_point && !options_.dry_run && any_possible
                              ? tr("Creating a System Restore point. This can take a minute…")
                              : tr("Starting…"));

    // The worker only touches the batch data; everything visible happens on the GUI thread.
    worker_ = std::thread([this] {
        uninstall::BatchEvents events;
        events.restore_point_done = [this](uninstall::RestorePointStatus status, std::uint32_t error) {
            QMetaObject::invokeMethod(
                this, [=, this] { onRestorePointDone(status, error); }, Qt::QueuedConnection);
        };
        events.continue_without_restore_point = [this](std::uint32_t error) {
            bool go_on = false;
            QMetaObject::invokeMethod(
                this, [&, this] { go_on = askContinueWithoutRestorePoint(error); },
                Qt::BlockingQueuedConnection);
            return go_on;
        };
        events.item_started = [this](std::size_t index) {
            QMetaObject::invokeMethod(this, [=, this] { onItemStarted(index); }, Qt::QueuedConnection);
        };
        events.item_finished = [this](std::size_t index, const uninstall::UninstallResult& result) {
            QMetaObject::invokeMethod(
                this, [=, this] { onItemFinished(index, result); }, Qt::QueuedConnection);
        };
        uninstall::run_batch(items_, options_, events, cancellation_);
        QMetaObject::invokeMethod(this, [this] { onBatchFinished(); }, Qt::QueuedConnection);
    });
}

void UninstallProgressDialog::onRestorePointDone(uninstall::RestorePointStatus status, std::uint32_t) {
    if (status == uninstall::RestorePointStatus::Created) {
        statusLabel_->setText(tr("System Restore point created."));
    }
}

bool UninstallProgressDialog::askContinueWithoutRestorePoint(std::uint32_t error) {
    const QString reason = error == ERROR_SERVICE_DISABLED
                               ? tr("System Protection is turned off for the system drive.")
                               : ProgramText::systemErrorText(error);
    const auto answer = QMessageBox::warning(
        this, tr("No restore point"),
        tr("Avel couldn't create a System Restore point: %1\n\nUninstall anyway?").arg(reason),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    return answer == QMessageBox::Yes;
}

void UninstallProgressDialog::onItemStarted(std::size_t index) {
    const auto& item = items_[index];
    const auto row = static_cast<int>(index);
    const QString name = table_->item(row, 0)->text();
    statusLabel_->setText(tr("Uninstalling %1 of %2: %3").arg(row + 1).arg(items_.size()).arg(name));
    if (!item.plan.possible() || options_.dry_run) {
        return;  // Finishes immediately
    }
    setStatus(row,
              item.plan.quiet ? tr("Uninstalling silently…") : tr("Waiting for the uninstaller window…"));
    table_->scrollToItem(table_->item(row, 0));
}

void UninstallProgressDialog::onItemFinished(std::size_t index, const uninstall::UninstallResult& result) {
    results_[index] = result;
    const auto row = static_cast<int>(index);
    setStatus(row, ProgramText::outcomeText(result));
    QColor color;
    switch (result.outcome) {
        case Outcome::Removed:
        case Outcome::RebootRequired:
            color = QColor(0x2E, 0x8B, 0x57);
            break;
        case Outcome::Simulated:
        case Outcome::Cancelled:
            break;
        default:
            color = QColor(0xC0, 0x40, 0x30);
            break;
    }
    if (color.isValid()) {
        table_->item(row, 1)->setForeground(color);
    }
    if (result.exit_code || result.outcome == Outcome::Removed || result.outcome == Outcome::RebootRequired ||
        result.outcome == Outcome::Failed) {
        changedAnything_ = true;
    }
    progress_->setValue(row + 1);
}

void UninstallProgressDialog::onBatchFinished() {
    if (worker_.joinable()) {
        worker_.join();
    }
    running_ = false;
    stopButton_->setEnabled(false);
    closeButton_->setEnabled(true);
    closeButton_->setDefault(true);
    progress_->setValue(progress_->maximum());

    const auto count = [this](std::initializer_list<Outcome> outcomes) {
        return static_cast<int>(std::count_if(results_.begin(), results_.end(), [&](const auto& result) {
            return std::find(outcomes.begin(), outcomes.end(), result.outcome) != outcomes.end();
        }));
    };
    if (options_.dry_run) {
        statusLabel_->setText(tr("Simulation finished. Nothing was changed."));
        return;
    }
    const int removed = count({Outcome::Removed, Outcome::RebootRequired});
    const int not_removed = static_cast<int>(results_.size()) - removed;
    QString summary = tr("%n program(s) removed.", nullptr, removed);
    if (not_removed > 0) {
        summary += u' ' + tr("%n not removed.", nullptr, not_removed);
    }
    if (count({Outcome::RebootRequired}) > 0) {
        summary += u' ' + tr("Restart Windows to finish.");
    }
    statusLabel_->setText(summary);
}

void UninstallProgressDialog::requestStop() {
    if (!running_ || cancellation_.terminate_current) {
        return;
    }
    QMessageBox box(QMessageBox::Question, tr("Stop uninstalling"), tr("What should Avel do?"),
                    QMessageBox::NoButton, this);
    QPushButton* after = box.addButton(tr("Stop after this program"), QMessageBox::AcceptRole);
    QPushButton* now = box.addButton(tr("Terminate the uninstaller now"), QMessageBox::DestructiveRole);
    box.addButton(tr("Keep going"), QMessageBox::RejectRole);
    box.setInformativeText(tr("Terminating an uninstaller can leave its program half removed."));
    box.exec();
    if (box.clickedButton() == after) {
        cancellation_.stop_batch = true;
        statusLabel_->setText(tr("Stopping after the current program…"));
    } else if (box.clickedButton() == now) {
        cancellation_.stop_batch = true;
        cancellation_.terminate_current = true;
        statusLabel_->setText(tr("Terminating the uninstaller…"));
    }
}

void UninstallProgressDialog::setStatus(int row, const QString& text) {
    auto* item = table_->item(row, 1);
    if (item == nullptr) {
        item = new QTableWidgetItem;
        table_->setItem(row, 1, item);
    }
    item->setText(text);
    item->setToolTip(text);
}

}  // namespace avel::app
